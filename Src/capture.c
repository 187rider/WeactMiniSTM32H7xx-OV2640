#include "fatfs.h"
#include "stdio.h"
#include "string.h"
#include "main.h"
#include "i2c.h"
#include "camera.h"
#include "lcd.h"

extern uint32_t photo_id;
extern volatile uint32_t DCMI_FrameIsReady;
extern volatile uint32_t DCMI_VsyncFlag;
extern volatile uint32_t DCMI_CallbackCount;

// JPEG capture buffer - single snapshot mode
#define JPEG_BUFFER_SIZE   (448*1024)  // 448KB buffer for JPEG snapshot
#define JPEG_BUFFER_WORDS  (JPEG_BUFFER_SIZE/4)
__attribute__((section(".sram1"))) static uint8_t jpeg_buffer[JPEG_BUFFER_SIZE];

// Timeout constants
#define VSYNC_TIMEOUT_MS    1000
#define FRAME_TIMEOUT_MS    4000
#define DELAY_STEP_MS       10

// File naming constants
#define MAX_PHOTO_ID        100000
#define MAX_FILENAME_ATTEMPTS 1000

// Helper: convert char to lowercase
static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Parse photo ID from filename (PHOTO_12345.jpeg or P12345.JPG)
static int parse_photo_id(const char *name, uint32_t *id_out)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    
    // Check for .jpeg or .jpg extension (case insensitive)
    int is_jpeg = (to_lower(dot[1]) == 'j' && to_lower(dot[2]) == 'p' && 
                   to_lower(dot[3]) == 'e' && to_lower(dot[4]) == 'g' && dot[5] == '\0');
    int is_jpg = (to_lower(dot[1]) == 'j' && to_lower(dot[2]) == 'p' && 
                  to_lower(dot[3]) == 'g' && dot[4] == '\0');
    
    if (!is_jpeg && !is_jpg) return 0;
    
    // Check for PHOTO_ or P prefix
    const char *p;
    if (strncmp(name, "PHOTO_", 6) == 0 || strncmp(name, "photo_", 6) == 0) {
        p = name + 6;
    } else if (name[0] == 'P' || name[0] == 'p') {
        p = name + 1;
    } else {
        return 0;
    }
    
    if (p >= dot) return 0;
    
    // Parse numeric ID
    uint32_t id = 0;
    while (p < dot) {
        if (*p < '0' || *p > '9') return 0;
        id = id * 10 + (uint32_t)(*p - '0');
        p++;
    }
    
    *id_out = id;
    return 1;
}

// Find next available photo ID by scanning SD card
static uint32_t find_next_photo_id(void)
{
    DIR dir;
    FILINFO finfo;
    uint32_t max_id = 0;
    
    if (f_opendir(&dir, "/") == FR_OK) {
        while (f_readdir(&dir, &finfo) == FR_OK && finfo.fname[0]) {
            uint32_t id = 0;
            if (parse_photo_id(finfo.fname, &id) && id >= max_id) {
                max_id = id + 1;
            }
        }
        f_closedir(&dir);
    }
    
    return max_id % MAX_PHOTO_ID;
}

// Ensure SD card is mounted
static int ensure_sd_mounted(void)
{
    if (SDFatFS.fs_type != 0) {
        return 1; // Already mounted
    }
    
    FRESULT res = f_mount(&SDFatFS, SDPath, 1);
    if (res != FR_OK) {
        char msg[32];
        snprintf(msg, sizeof(msg), "SD mount err:%d", res);
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        return 0;
    }
    
    return 1;
}

// Find unused filename, trying both long and short name formats
static FRESULT find_unused_filename(char *filename, size_t filename_size, uint32_t *photo_id_ptr)
{
    FILINFO finfo;
    FRESULT res;
    int use_short_name = 0;
    uint32_t attempts = 0;
    uint32_t id = *photo_id_ptr;
    
    while (attempts < MAX_FILENAME_ATTEMPTS) {
        // Generate filename
        if (use_short_name) {
            snprintf(filename, filename_size, "P%05lu.JPG", id % MAX_PHOTO_ID);
        } else {
            snprintf(filename, filename_size, "PHOTO_%05lu.jpeg", id % MAX_PHOTO_ID);
        }
        
        // Check if file exists
        res = f_stat(filename, &finfo);
        
        // If long filename not supported, switch to short names
        if (res == FR_INVALID_NAME && !use_short_name) {
            use_short_name = 1;
            continue; // Don't increment attempts, just retry with short name
        }
        
        // File doesn't exist - we can use this name
        if (res == FR_NO_FILE || res == FR_INVALID_NAME) {
            *photo_id_ptr = (id + 1) % MAX_PHOTO_ID;
            return FR_OK;
        }
        
        // File exists or other error - try next ID
        id = (id + 1) % MAX_PHOTO_ID;
        attempts++;
    }
    
    return FR_DENIED; // No free filename found
}

// Capture JPEG image and save to SD card
uint8_t take_A_Picture(DCMI_HandleTypeDef *hdcmi)
{
    FRESULT res;
    char filename[32];
    char msg[64];
    
    // Ensure SD card is mounted
    if (!ensure_sd_mounted()) {
        return 0;
    }
    
    // Find next available photo ID and filename
    photo_id = find_next_photo_id();
    res = find_unused_filename(filename, sizeof(filename), &photo_id);
    
    if (res != FR_OK) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"No free filename");
        return 0;
    }
    
    // Create file
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Creating file...");
    
    FIL capture_file;
    res = f_open(&capture_file, filename, FA_CREATE_NEW | FA_WRITE);
    if (res != FR_OK) {
        snprintf(msg, sizeof(msg), "File open err:%d", res);
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        return 0;
    }
    
    // Prepare for capture
    memset(jpeg_buffer, 0, JPEG_BUFFER_SIZE);
    DCMI_FrameIsReady = 0;
    DCMI_VsyncFlag = 0;
    
    // Configure camera for JPEG mode
    Camera_Picture_Device(&hi2c1);
    HAL_Delay(50);
    
    // Clear DCMI flags and enable interrupts
    __HAL_DCMI_CLEAR_FLAG(hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_VSYNCRI | 
                          DCMI_FLAG_ERRRI | DCMI_FLAG_OVRRI | DCMI_FLAG_LINERI);
    __HAL_DCMI_ENABLE_IT(hdcmi, DCMI_IT_FRAME | DCMI_IT_VSYNC);
    
    // CRITICAL: Invalidate cache before DMA writes to buffer
    // This discards any stale cache lines so DMA has clean memory to write to
#if defined(SCB_InvalidateDCache_by_Addr)
    SCB_InvalidateDCache_by_Addr((uint32_t*)jpeg_buffer, JPEG_BUFFER_SIZE);
#endif
    
    // Start DCMI DMA capture
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Capturing...");
    
    if (HAL_DCMI_Start_DMA(hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)jpeg_buffer, 
                           JPEG_BUFFER_WORDS) != HAL_OK) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"DMA start failed");
        f_close(&capture_file);
        return 0;
    }
    
    // Wait for VSYNC signal
    uint32_t wait_ms = 0;
    while (!DCMI_VsyncFlag && wait_ms < VSYNC_TIMEOUT_MS) {
        HAL_Delay(DELAY_STEP_MS);
        wait_ms += DELAY_STEP_MS;
    }
    
    if (!DCMI_VsyncFlag) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"VSYNC timeout");
        HAL_DCMI_Stop(hdcmi);
        f_close(&capture_file);
        return 0;
    }
    
    // Wait for frame capture to complete
    wait_ms = 0;
    while (!DCMI_FrameIsReady && wait_ms < FRAME_TIMEOUT_MS) {
        HAL_Delay(DELAY_STEP_MS);
        wait_ms += DELAY_STEP_MS;
    }
    
    if (!DCMI_FrameIsReady) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Frame timeout");
        HAL_DCMI_Stop(hdcmi);
        f_close(&capture_file);
        return 0;
    }
    
    // Stop DCMI
    if (HAL_DCMI_Stop(hdcmi) != HAL_OK) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"DCMI stop failed");
        f_close(&capture_file);
        return 0;
    }
    
    // Invalidate cache to ensure we read DMA data
#if defined(SCB_InvalidateDCache_by_Addr)
    SCB_InvalidateDCache_by_Addr((uint32_t*)jpeg_buffer, JPEG_BUFFER_SIZE);
#endif
    
    // Find JPEG Start of Image (SOI: 0xFFD8) and End of Image (EOI: 0xFFD9)
    uint32_t soi_pos = JPEG_BUFFER_SIZE;
    uint32_t eoi_pos = JPEG_BUFFER_SIZE;
    
    for (uint32_t i = 0; i + 1 < JPEG_BUFFER_SIZE; i++) {
        if (soi_pos == JPEG_BUFFER_SIZE && 
            jpeg_buffer[i] == 0xFF && jpeg_buffer[i+1] == 0xD8) {
            soi_pos = i;
        }
        if (soi_pos != JPEG_BUFFER_SIZE && 
            jpeg_buffer[i] == 0xFF && jpeg_buffer[i+1] == 0xD9) {
            eoi_pos = i + 2;
            break;
        }
    }
    
    // Validate JPEG markers found
    if (soi_pos == JPEG_BUFFER_SIZE || eoi_pos == JPEG_BUFFER_SIZE || eoi_pos <= soi_pos) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Invalid JPEG");
        f_close(&capture_file);
        return 0;
    }
    
    // Write JPEG data to file
    uint32_t jpeg_size = eoi_pos - soi_pos;
    UINT bytes_written = 0;
    
    res = f_write(&capture_file, &jpeg_buffer[soi_pos], jpeg_size, &bytes_written);
    if (res != FR_OK || bytes_written != jpeg_size) {
        snprintf(msg, sizeof(msg), "Write err:%d", res);
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        f_close(&capture_file);
        return 0;
    }
    
    // Sync and close file
    f_sync(&capture_file);
    f_close(&capture_file);
    
    // Display success message
    snprintf(msg, sizeof(msg), "Saved %lu bytes", (unsigned long)bytes_written);
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
    
    return 1;
}