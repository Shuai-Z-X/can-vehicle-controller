/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_can.h"
#include "app_slcan.h"
#include "oled.h"
#include "usart.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* OLED 显示任务：优先级最低，参数不变时不做任何刷新 */
osThreadId_t displayTaskHandle;
const osThreadAttr_t displayTask_attributes = {
  .name = "displayTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
osThreadId_t canTxTaskHandle;
const osThreadAttr_t canTxTask_attributes = {
  .name = "canTxTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t canRxTaskHandle;
const osThreadAttr_t canRxTask_attributes = {
  .name = "canRxTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};

/* SLCAN 网关任务：把 F407 变成一块"自制 CANable"，Linux 侧挂成 can0 */
osThreadId_t slcanTaskHandle;
const osThreadAttr_t slcanTask_attributes = {
  .name = "slcanTask",
  .stack_size = 512 * 4,
  /* 优先级放低：串口是阻塞式发送，不能让网关任务拖慢 CAN 的 20ms 周期 */
  .priority = (osPriority_t) osPriorityLow,
};
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartDisplayTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
}
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
void vApplicationMallocFailedHook(void)
{
   /* vApplicationMallocFailedHook() will only be called if
   configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h. It is a hook
   function that will get called if a call to pvPortMalloc() fails.
   pvPortMalloc() is called internally by the kernel whenever a task, queue,
   timer or semaphore is created. It is also called by various parts of the
   demo application. If heap_1.c or heap_2.c are used, then the size of the
   heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
   FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
   to query the size of free heap space that remains (although it does not
   provide information on how the remaining heap might be fragmented). */
}
/* USER CODE END 5 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
	AppCan_Init();
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
	canTxTaskHandle = osThreadNew(AppCan_TaskCanTx, NULL, &canTxTask_attributes);
	canRxTaskHandle = osThreadNew(AppCan_TaskCanRx, NULL, &canRxTask_attributes);
	displayTaskHandle = osThreadNew(StartDisplayTask, NULL, &displayTask_attributes);
	slcanTaskHandle = osThreadNew(AppSlcan_Task, NULL, &slcanTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* 临时诊断用：直接把一行字打到串口上 */
static void app_display_print(const char *text)
{
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)text,
                          (uint16_t)strlen(text), 100U);
}

/* 模式名字固定 7 个字符宽，避免 "STANDBY" 换成 "RUN" 时屏上留下残字 */
static const char *app_display_mode_text(uint8_t mode)
{
  switch (mode) {
  case 0:  return "OFF    ";
  case 1:  return "STANDBY";
  case 2:  return "RUN    ";
  case 3:  return "FAULT  ";
  default: return "UNKNOWN";
  }
}

/**
  * @brief  第 7 步：OLED 显示 VCU 参数
  *         第1行 节点名 / 第2行 模式+故障位 / 第3行 车速 / 第4行 电压
  */
void StartDisplayTask(void *argument)
{
  /* USER CODE BEGIN StartDisplayTask */
  uint8_t  last_mode  = 0xFFU;
  uint8_t  last_fault = 0xFFU;
  uint16_t last_speed = 0xFFFFU;
  uint16_t last_volt  = 0xFFFFU;

  (void)argument;

  /* 开机自检：先探测 OLED 有没有应答。
   *   有应答 -> 接线和供电没问题，屏不亮就要往初始化和显示内容上找
   *   没应答 -> 查 VCC/GND 有没有接好、SCL 是不是 PB6、SDA 是不是 PB7，
   *             以及模块的 I2C 地址是不是 0x78（有的模块是 0x7A） */
  if (OLED_Probe(0x78U) != 0U) {
    app_display_print("OLED probe: ACK at 0x78 (OK)\r\n");
  } else if (OLED_Probe(0x7AU) != 0U) {
    app_display_print("OLED probe: ACK at 0x7A, change 0x78 to 0x7A in oled.c\r\n");
  } else {
    app_display_print("OLED probe: NO ACK at 0x78/0x7A, check VCC/GND/SCL/SDA\r\n");
  }

  OLED_Init();     /* 内部包含了上电延时和 I2C 引脚初始化 */

  /* 开机自检：整屏点亮 0.3 秒。
   * 上电时能看见整屏发白 -> 模块、供电、接线、时序全都是好的
   * 连白屏都看不见     -> 是硬件侧的问题，不用再折腾代码 */
  OLED_Fill(0xFFU);
  osDelay(300U);

  OLED_Clear();

  for(;;)
  {
    uint8_t  mode  = AppCan_GetVcuMode();
    uint8_t  fault = AppCan_GetVcuFaultFlags();
    uint16_t speed = AppCan_GetVcuSpeedX10();
    uint16_t volt  = AppCan_GetVcuVoltageX100();

    /* 软件 I2C 刷一次屏要好几毫秒，参数没变就什么都不做 */
    if ((mode != last_mode) || (fault != last_fault) ||
        (speed != last_speed) || (volt != last_volt))
    {
      last_mode  = mode;
      last_fault = fault;
      last_speed = speed;
      last_volt  = volt;

      /* 第1行：这是哪块板子 */
      OLED_ShowString(1, 1, "VCU  STM32F407");

      /* 第2行：工作模式 + 故障标志位 */
      OLED_ShowString(2, 1, "MD:");
      OLED_ShowString(2, 4, (char *)app_display_mode_text(mode));
      OLED_ShowString(2, 12, "F:");
      OLED_ShowHexNum(2, 14, fault, 2);

      /* 第3行：车速，显示成 12.3km/h */
      OLED_ShowString(3, 1, "SPD:");
      OLED_ShowNum(3, 5, speed / 10U, 2);
      OLED_ShowChar(3, 7, '.');
      OLED_ShowNum(3, 8, speed % 10U, 1);
      OLED_ShowString(3, 9, "km/h");

      /* 第4行：电池电压，显示成 24.50V */
      OLED_ShowString(4, 1, "BAT:");
      OLED_ShowNum(4, 5, volt / 100U, 2);
      OLED_ShowChar(4, 7, '.');
      OLED_ShowNum(4, 8, volt % 100U, 2);
      OLED_ShowString(4, 10, "V");
    }

    osDelay(200);
  }
  /* USER CODE END StartDisplayTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

