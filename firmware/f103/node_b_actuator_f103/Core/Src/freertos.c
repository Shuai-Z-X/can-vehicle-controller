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
#include "app_can_selftest.h"
#include "app_can.h"
#include "app_motor.h"
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
osThreadId_t canSelfTestTaskHandle;
const osThreadAttr_t canSelfTestTask_attributes = {
  .name = "canSelfTestTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for displayTask */
osThreadId_t displayTaskHandle;
const osThreadAttr_t displayTask_attributes = {
  .name = "displayTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
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

/* 电机任务：把 CAN 命令换算成转速下发给 X42S，再把真实反馈读回来 */
osThreadId_t motorTaskHandle;
const osThreadAttr_t motorTask_attributes = {
  .name = "motorTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartDisplayTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 5 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   (void)xTask;
   (void)pcTaskName;

   /* 栈已经被破坏，这里不要调用 printf、malloc 等函数。 */
   taskDISABLE_INTERRUPTS();
   for (;;) {
   }
}

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

  /* creation of displayTask */
  displayTaskHandle = osThreadNew(StartDisplayTask, NULL, &displayTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  //canSelfTestTaskHandle = osThreadNew(CanSelfTest_Task, NULL, &canSelfTestTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  canTxTaskHandle = osThreadNew(AppCan_TaskCanTx, NULL, &canTxTask_attributes);
	canRxTaskHandle = osThreadNew(AppCan_TaskCanRx, NULL, &canRxTask_attributes);
	motorTaskHandle = osThreadNew(AppMotor_Task, NULL, &motorTask_attributes);
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

/* USER CODE BEGIN Header_StartDisplayTask */
/**
* @brief Function implementing the displayTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartDisplayTask */
void StartDisplayTask(void *argument)
{
  /* USER CODE BEGIN StartDisplayTask */
  uint8_t  last_mode = 0xFFU;
  uint8_t  last_fault = 0xFFU;
  uint16_t last_out = 0xFFFFU;
  uint16_t last_cur = 0xFFFFU;
  int16_t  last_temp = 0x7FFF;

  (void)argument;

  /* 开机自检：先探测 OLED 有没有应答 */
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
    uint8_t  mode  = AppCan_GetActuatorMode();
    uint8_t  fault = AppCan_GetActuatorFault();
    uint16_t out   = AppCan_GetActuatorOutputX10();
    uint16_t cur   = AppCan_GetActuatorCurrentX100();
    int16_t  temp  = AppCan_GetActuatorTempC();

    /* 软件 I2C 刷一次屏要好几毫秒，参数没变就什么都不做 */
    if ((mode != last_mode) || (fault != last_fault) || (out != last_out) ||
        (cur != last_cur) || (temp != last_temp))
    {
      last_mode  = mode;
      last_fault = fault;
      last_out   = out;
      last_cur   = cur;
      last_temp  = temp;

      /* 第1行：这是哪块板子 */
      OLED_ShowString(1, 1, "ACT STM32F103");

      /* 第2行：工作模式 + 故障标志位 */
      OLED_ShowString(2, 1, "MD:");
      OLED_ShowString(2, 4, (char *)app_display_mode_text(mode));
      OLED_ShowString(2, 12, "F:");
      OLED_ShowHexNum(2, 14, fault, 2);

      /* 第3行：实际输出百分比，显示成 30.0% */
      OLED_ShowString(3, 1, "OUT:");
      OLED_ShowNum(3, 5, out / 10U, 3);
      OLED_ShowChar(3, 8, '.');
      OLED_ShowNum(3, 9, out % 10U, 1);
      OLED_ShowChar(3, 10, '%');

      /* 第4行：温度 + 电流，显示成 T:45C I:0.50A */
      OLED_ShowString(4, 1, "T:");
      OLED_ShowSignedNum(4, 3, temp, 2);
      OLED_ShowChar(4, 6, 'C');
      OLED_ShowString(4, 8, "I:");
      OLED_ShowNum(4, 10, cur / 100U, 1);
      OLED_ShowChar(4, 11, '.');
      OLED_ShowNum(4, 12, cur % 100U, 2);
      OLED_ShowChar(4, 14, 'A');
    }

    osDelay(200);
  }
  /* USER CODE END StartDisplayTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

