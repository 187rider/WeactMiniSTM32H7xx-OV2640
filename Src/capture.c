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

#define MAX_PICTURE_BUFF (100*2048)
 //uint8_t pBuff[MAX_PICTURE_BUFF];
// DMA-accessible buffer for JPEG capture, placed in AXI SRAM (.sram1)
__attribute__((section(".sram1"))) static uint8_t pBuff[MAX_PICTURE_BUFF];
//uint8_t pBuff[MAX_PICTURE_BUFF] __attribute__((aligned(32)));

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int parse_photo_id(const char *name, uint32_t *id_out)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (!(to_lower(dot[1]) == 'j' && to_lower(dot[2]) == 'p' && to_lower(dot[3]) == 'e' && to_lower(dot[4]) == 'g' && dot[5] == '\0')) {
        return 0;
    }
    if (strncmp(name, "PHOTO_", 6) != 0 && strncmp(name, "photo_", 6) != 0) return 0;
    const char *p = name + 6;
    if (p >= dot) return 0;
    uint32_t id = 0;
    while (p < dot) {
        if (*p < '0' || *p > '9') return 0;
        id = id * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *id_out = id;
    return 1;
}

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
    return max_id % 100000;
}

static int ensure_sd_mounted(void)
{
    if (SDFatFS.fs_type != 0) {
        return 1; // already mounted
    }
    FRESULT res = f_mount(&SDFatFS, SDPath, 1);
    if (res != FR_OK) {
        char msg[32];
        sprintf(msg, "SD mount err:%d", res);
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        return 0;
    }
    return 1;
}
uint8_t take_A_Picture(DCMI_HandleTypeDef *hdcmi)
{
    FIL file;
    FRESULT res;
    uint32_t f_begin = 0, f_end = 0, sublen;
    uint16_t f_count, r;
    UINT bw;
    uint8_t picture_ok = 0;
    uint8_t headerFinder = 0;
    char msg[50];

    char fn[32];
    if (!ensure_sd_mounted()) {
        return 0;
    }
    FILINFO finfo;
    /* Find next free PHOTO_%05u.jpeg slot (0-99999), start from current max+1.
       If long filenames are unsupported (FR_INVALID_NAME), fall back to 8.3 P#####.JPG. */
    photo_id = find_next_photo_id();
    uint32_t attempts = 0;
    int use_short_name = 0;
    FRESULT stat_res;
    while (attempts < 100000) {
        if (use_short_name) {
            sprintf(fn, "P%05lu.JPG", photo_id % 100000); // 8.3 safe
        } else {
            sprintf(fn, "PHOTO_%05lu.jpeg", photo_id % 100000);
        }
        stat_res = f_stat(fn, &finfo);
        if (stat_res == FR_INVALID_NAME && !use_short_name) {
            // Long filenames not supported; switch to short names
            use_short_name = 1;
            attempts = 0;
            continue;
        }
        if (stat_res != FR_OK) {
            break; // free name found
        }
        photo_id = (photo_id + 1) % 100000;
        attempts++;
    }
    if (attempts >= 100000) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"SD Full (no free name)");
        return 0;
    }
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Creating file...");

    res = f_open(&file, fn, FA_CREATE_NEW | FA_WRITE);
    if(res == FR_INVALID_NAME && !use_short_name) {
        // Retry with short name if long name creation fails
        sprintf(fn, "P%05lu.JPG", photo_id % 100000);
        res = f_open(&file, fn, FA_CREATE_NEW | FA_WRITE);
    }
    if(res != FR_OK) {
        sprintf(msg, "SD Error:%d", res);
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        return 0;
    }
    photo_id = (photo_id + 1) % 100000;

    // Stop any ongoing capture and reset flags
    HAL_DCMI_Stop(hdcmi);
    HAL_Delay(50);
    DCMI_FrameIsReady = 0;
    DCMI_CallbackCount = 0;
    DCMI_VsyncFlag = 0;

    // Initialize camera in JPEG mode
    Camera_Picture_Device(&hi2c1);
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Init camera...");
    HAL_Delay(500);

    // Clear buffer
    memset(pBuff, 0, MAX_PICTURE_BUFF);

    // **CORRECTED: Clear and enable BOTH VSYNC and FRAME interrupts**
    __HAL_DCMI_CLEAR_FLAG(hdcmi, DCMI_IT_VSYNC | DCMI_IT_FRAME);
    __HAL_DCMI_ENABLE_IT(hdcmi, DCMI_IT_VSYNC | DCMI_IT_FRAME);

    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Starting DMA...");

    // **CORRECTED: Start DMA FIRST, then wait for VSYNC**
    if(HAL_DCMI_Start_DMA(hdcmi, DCMI_MODE_SNAPSHOT, (uint32_t)pBuff, MAX_PICTURE_BUFF / 4) != HAL_OK) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"DMA Start Failed");
        f_close(&file);
        return 0;
    }

    // **CORRECTED: Wait for VSYNC (frame start)**
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Waiting VSYNC...");
    uint32_t vsync_timeout = 0;
    while(DCMI_VsyncFlag == 0) {
        if(vsync_timeout++ > 1000) {  // 1 second for VSYNC
            sprintf(msg, "VSYNC Timeout!");
            LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
            HAL_DCMI_Stop(hdcmi);
            f_close(&file);
            return 0;
        }
        HAL_Delay(1);
    }

    // VSYNC detected - frame has started
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"VSYNC OK - Capturing...");

    // **CORRECTED: Now wait for FRAME complete (end of frame)**
    LCD_ShowString(0, 62, ST7735Ctx.Width, 5, 12, (uint8_t*)"Waiting frame...");
    uint32_t frame_timeout = 0;
    while(DCMI_FrameIsReady == 0) {
        if(frame_timeout % 200 == 0) {
            sprintf(msg, "Frame T:%lu", frame_timeout);
            LCD_ShowString(0, 74, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        }
        
        if(frame_timeout++ > 3000) {  // 3 second timeout for frame completion
            sprintf(msg, "Frame Timeout!");
            LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
            HAL_DCMI_Stop(hdcmi);
            f_close(&file);
            return 0;
        }
        HAL_Delay(1);
    }

    // Frame complete - stop capture
    HAL_DCMI_Stop(hdcmi);
    HAL_Delay(10);

    // Rest of your JPEG processing code remains the same...
    LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Scanning buffer...");

    // Check first few bytes
    sprintf(msg, "First bytes: %02X %02X %02X %02X", pBuff[0], pBuff[1], pBuff[2], pBuff[3]);
    LCD_ShowString(0, 62, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
    HAL_Delay(1000);

    // Scan for JPEG markers
    for(uint32_t idx = 0; idx < MAX_PICTURE_BUFF - 1; idx++) {
        if(headerFinder == 0 && pBuff[idx] == 0xFF && pBuff[idx+1] == 0xD8) {
            headerFinder = 1;
            f_begin = idx;
            sprintf(msg, "SOI at: %lu", f_begin);
            LCD_ShowString(0, 74, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
        }
        if(headerFinder == 1 && pBuff[idx] == 0xFF && pBuff[idx+1] == 0xD9) {
            f_end = idx + 1;
            picture_ok = 1;
            sprintf(msg, "EOI at: %lu", f_end);
            LCD_ShowString(0, 86, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);

            uint8_t *buff = pBuff + f_begin;
            sublen = f_end - f_begin + 1;

            sprintf(msg, "Size: %lu bytes", sublen);
            LCD_ShowString(0, 98, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);

            f_count = sublen / 512;
            r = sublen % 512;

            for(int i = 0; i < f_count; i++) {
                res = f_write(&file, buff, 512, &bw);
                if(res != FR_OK || bw != 512) {
                    sprintf(msg, "Write err: %d", res);
                    LCD_ShowString(0, 110, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
                }
                buff += 512;
            }
            if(r > 0) {
                res = f_write(&file, buff, r, &bw);
                if(res != FR_OK || bw != r) {
                    sprintf(msg, "Final write err: %d", res);
                    LCD_ShowString(0, 110, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
                }
            }
            break;
        }
    }

    f_close(&file);

    if(!picture_ok) {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"No JPEG markers!");
        uint32_t non_zero = 0;
        for(uint32_t i = 0; i < 1000; i++) {
            if(pBuff[i] != 0) non_zero++;
        }
        sprintf(msg, "Non-zero: %lu/1000", non_zero);
        LCD_ShowString(0, 62, ST7735Ctx.Width, 5, 12, (uint8_t*)msg);
    } else {
        LCD_ShowString(0, 50, ST7735Ctx.Width, 5, 12, (uint8_t*)"Picture saved!");
    }

    HAL_Delay(500);

    return picture_ok;
}
