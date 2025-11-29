/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dcmi.h"
#include "dma.h"
#include "fatfs.h"
#include "i2c.h"
#include "libjpeg.h"
#include "sdmmc.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "camera.h"
#include "capture.h"
#include "lcd.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define PREVIEW_WIDTH  160
#define PREVIEW_HEIGHT 120
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

#define TFT96
HAL_SD_CardCIDTypedef pCID;
HAL_SD_CardCSDTypedef pCSD;
HAL_SD_CardInfoTypeDef pCardInfo;
FRESULT res;

uint32_t photo_id = 0;


uint16_t pic[PREVIEW_HEIGHT][PREVIEW_WIDTH];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
// QQVGA

// picture buffer
volatile  uint32_t DCMI_FrameIsReady = 0;
volatile uint32_t DCMI_VsyncFlag = 0;
volatile uint32_t DCMI_CallbackCount = 0; 
uint32_t Camera_FPS=0;
static void DCMI_ReinitDMAMode(uint32_t mode);
static void DCMI_SetJPEGMode(uint32_t mode);
typedef enum {
    CAM_MODE_PREVIEW,
    CAM_MODE_JPEG
} cam_mode_t;
static void Camera_SetMode(cam_mode_t mode);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();
	
  /* Configure the MPU attributes for the QSPI 256MB without instruction access */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress      = QSPI_BASE;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_256MB;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.SubRegionDisable = 0x00;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);
	
  /* Configure the MPU attributes for the QSPI 8MB (QSPI Flash Size) to Cacheable WT */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress      = QSPI_BASE;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.SubRegionDisable = 0x00;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);
	
  /* Setup AXI SRAM in Cacheable WB */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.BaseAddress      = D1_AXISRAM_BASE;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER2;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);
	
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static void CPU_CACHE_Enable(void)
{
  /* Enable I-Cache */
  SCB_EnableICache();

  /* Enable D-Cache */
  SCB_EnableDCache();
}

void LED_Blink(uint32_t Hdelay, uint32_t Ldelay)
{
  HAL_GPIO_WritePin(PE3_GPIO_Port, PE3_Pin, GPIO_PIN_SET);
  HAL_Delay(Hdelay - 1);
  HAL_GPIO_WritePin(PE3_GPIO_Port, PE3_Pin, GPIO_PIN_RESET);
  HAL_Delay(Ldelay - 1);
}

static void DCMI_ReinitDMAMode(uint32_t mode)
{
    DMA_HandleTypeDef *hdma = hdcmi.DMA_Handle;
    if (hdma == NULL) {
        return;
    }
    HAL_DMA_DeInit(hdma);
    hdma->Init.Mode = mode;
    if (HAL_DMA_Init(hdma) != HAL_OK) {
        Error_Handler();
    }
}

static void DCMI_SetJPEGMode(uint32_t mode)
{
    if (hdcmi.Init.JPEGMode == mode) {
        return;
    }
    hdcmi.Init.JPEGMode = mode;
    HAL_DCMI_DeInit(&hdcmi);
    if (HAL_DCMI_Init(&hdcmi) != HAL_OK) {
        Error_Handler();
    }
}

static void Camera_SetMode(cam_mode_t mode)
{
    HAL_DCMI_Stop(&hdcmi);
    HAL_Delay(10);
    __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_IT_FRAME | DCMI_IT_OVR | DCMI_IT_ERR | DCMI_IT_LINE | DCMI_IT_VSYNC);
    __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_FRAME | DCMI_IT_VSYNC);

    if (mode == CAM_MODE_PREVIEW) {
        DCMI_SetJPEGMode(DCMI_JPEG_DISABLE);
        hdcmi.Instance->CR &= ~DCMI_CR_JPEG;
        DCMI_ReinitDMAMode(DMA_CIRCULAR);

        Camera_Init_Device(&hi2c1, FRAMESIZE_QQVGA);
        HAL_Delay(80);

        DCMI_FrameIsReady = 0;
        uint32_t buf_bytes = PREVIEW_WIDTH * PREVIEW_HEIGHT * sizeof(uint16_t);
        uint32_t length_words = (buf_bytes + 3) / 4;
    #if defined(SCB_CleanDCache_by_Addr)
        SCB_CleanDCache_by_Addr((uint32_t*)pic, buf_bytes);
        SCB_InvalidateDCache_by_Addr((uint32_t*)pic, buf_bytes);
    #endif
        HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_CONTINUOUS, (uint32_t)pic, length_words);
    } else {
        DCMI_SetJPEGMode(DCMI_JPEG_ENABLE);
        hdcmi.Instance->CR |= DCMI_CR_JPEG;
        DCMI_ReinitDMAMode(DMA_NORMAL);
    }
}


void Camera_StartPreview(void)
{
    Camera_SetMode(CAM_MODE_PREVIEW);
}

void Camera_CaptureJPEG(void)
{
    Camera_SetMode(CAM_MODE_JPEG);
    take_A_Picture(&hdcmi); // saves JPEG to SD
    // Return to preview automatically
    Camera_SetMode(CAM_MODE_PREVIEW);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

#ifdef W25Qxx
  SCB->VTOR = QSPI_BASE;
#endif
  MPU_Config();
  CPU_CACHE_Enable();
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_DCMI_Init();
  MX_I2C1_Init();
  MX_SPI4_Init();
  MX_TIM1_Init();
  MX_SDMMC1_SD_Init();
  MX_FATFS_Init();

  /* USER CODE BEGIN 2 */
  	HAL_SD_GetCardCID(&hsd1, &pCID);
  HAL_SD_GetCardCSD(&hsd1, &pCSD);
	HAL_SD_GetCardInfo(&hsd1, &pCardInfo);
	
	HAL_Delay(50);

   uint8_t text[20];
	
  LCD_Test();
	//clean Ypos 58
	ST7735_LCD_Driver.FillRect(&st7735_pObj, 0, 58, ST7735Ctx.Width, 16, BLACK);

  while (HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_RESET)
  {

    sprintf((char *)&text, "Camera id:0x%x   ", hcamera.device_id);
    LCD_ShowString(0, 58, ST7735Ctx.Width, 16, 12, text);

    LED_Blink(5, 500);

    sprintf((char *)&text, "LongPress K1 to Run");
    LCD_ShowString(0, 58, ST7735Ctx.Width, 16, 12, text);

    LED_Blink(5, 500);


  }

  res = f_mount(&SDFatFS, SDPath, 4);
HAL_Delay(100);
  //	HAL_TIM_PWM_Start(&htim1,TIM_CHANNEL_1);
  //	HAL_Delay(10);

    Camera_StartPreview();


  /* USER CODE END 2 */

//
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint8_t key_prev = GPIO_PIN_SET;
  while (1)
  {
     // Continuous preview update
    if (DCMI_FrameIsReady)
    {
        DCMI_FrameIsReady = 0;
        ST7735_FillRGBRect(&st7735_pObj, 0, 0, (uint8_t *)&pic[20][0], ST7735Ctx.Width, 80);
        sprintf((char *)text, "%luFPS", Camera_FPS);
        LCD_ShowString(5, 5, 60, 16, 12, text);
    }

    // Edge-detect K1 press to avoid blocking the preview loop
    uint8_t key_now = HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin);
    if (key_prev == GPIO_PIN_SET && key_now == GPIO_PIN_RESET)
    {
        Camera_CaptureJPEG();
        HAL_Delay(300); // Allow SD write to finish or sensor to stabilize
        Camera_StartPreview();
    }
    key_prev = key_now;
  }
}
/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 32;
  RCC_OscInitStruct.PLL.PLLN = 480;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_1;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI48, RCC_MCODIV_2);
}

/* USER CODE BEGIN 4 */
void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi)
{
	static uint32_t count = 0,tick = 0;
	   DCMI_CallbackCount++;  // Increment each time callback is called
	if(HAL_GetTick() - tick >= 1000)
	{
		tick = HAL_GetTick();
		Camera_FPS = count;
		count = 0;
	}
	count ++;
	
  DCMI_FrameIsReady = 1;
  HAL_GPIO_TogglePin(PE3_GPIO_Port, PE3_Pin);
}
void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *hdcmi)
{
    DCMI_VsyncFlag = 1;
    DCMI_CallbackCount++;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  //__disable_irq();
  while (1)
  {
    LED_Blink(5, 250);
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
