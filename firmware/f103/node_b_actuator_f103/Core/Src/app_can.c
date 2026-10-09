#include "app_can.h"

#include "can.h"
#include "usart.h"
#include "cmsis_os.h"
#include "can_protocol.h"

#include <string.h>

#if defined(STM32F407xx)
#define CAN_HANDLE hcan1
#else
#define CAN_HANDLE hcan
#endif

static volatile uint16_t g_target_output_x10 = 0U;
static volatile uint8_t g_mode = CAN_MODE_STANDBY;
static osMutexId_t uartMutexHandle;
static void app_can_print(const char *text)
{
    if (uartMutexHandle != NULL) {
        osMutexAcquire(uartMutexHandle, osWaitForever);
    }
    HAL_UART_Transmit(&huart1,(uint8_t *)text,(uint16_t)strlen(text),100U);
    if (uartMutexHandle != NULL) {
        osMutexRelease(uartMutexHandle);
    }
}

static bool app_can_config_filter(void)
{
    CAN_FilterTypeDef filter = {0};

    filter.FilterBank = 0U;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = 0x0000U;
    filter.FilterIdLow = 0x0000U;
    filter.FilterMaskIdHigh = 0x0000U;
    filter.FilterMaskIdLow = 0x0000U;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
#if defined(STM32F407xx)
    filter.SlaveStartFilterBank = 14U;
#else
    filter.SlaveStartFilterBank = 0U;
#endif

    return HAL_CAN_ConfigFilter(&CAN_HANDLE, &filter) == HAL_OK;
}

void AppCan_Init(void)
{
    uartMutexHandle = osMutexNew(NULL);
    if (uartMutexHandle == NULL) {
        /* 互斥量创建失败，通常是 FreeRTOS 堆不够 */
    }

    if (!app_can_config_filter()) {
        app_can_print("CAN filter init failed\r\n");
        return;
    }

    if (HAL_CAN_Start(&CAN_HANDLE) != HAL_OK) {
        app_can_print("CAN start failed\r\n");
        return;
    }

    app_can_print("CAN normal mode ready\r\n");
}

static bool app_can_send(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    CAN_TxHeaderTypeDef tx_header = {0};
    uint32_t mailbox = 0U;

    tx_header.StdId = id;
    tx_header.DLC = dlc;
    tx_header.IDE = CAN_ID_STD;
    tx_header.RTR = CAN_RTR_DATA;

    return HAL_CAN_AddTxMessage(&CAN_HANDLE,
                                &tx_header,
                                (uint8_t *)data,
                                &mailbox) == HAL_OK;
}

void AppCan_TaskCanRx(void *argument)
{
    (void)argument;

    for (;;) {
        CAN_RxHeaderTypeDef rx_header;
        uint8_t data[8];

        while (HAL_CAN_GetRxFifoFillLevel(&CAN_HANDLE, CAN_RX_FIFO0) > 0U) {
            if (HAL_CAN_GetRxMessage(&CAN_HANDLE,
                                     CAN_RX_FIFO0,
                                     &rx_header,
                                     data) != HAL_OK) {
                break;
            }

            if (rx_header.StdId == CAN_ID_CONTROL_CMD &&
                rx_header.DLC == 8U) {
                control_cmd_t cmd;
                if (can_decode_control_cmd(data, &cmd)) {
                    g_target_output_x10 = cmd.target_output_pct_x10;
                    g_mode = cmd.mode;
                    app_can_print("RX 0x200 Control_Cmd\r\n");

#ifdef LED_Pin
                    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
#endif
                }
            } else if (rx_header.StdId == CAN_ID_VCU_STATUS &&
                       rx_header.DLC == 8U) {
                vcu_status_t status;
                if (can_decode_vcu_status(data, &status)) {
                    app_can_print("RX 0x100 VCU_Status\r\n");
                }
            }
        }

        osDelay(1U);
    }
}

void AppCan_TaskCanTx(void *argument)
{
    uint8_t counter = 0U;

    (void)argument;

    for (;;) {
        actuator_status_t status;
        uint8_t data[8];

        status.mode = g_mode;
        status.output_pct_x10 = g_target_output_x10;
        status.current_a_x100 = 50U;   /* 0.50 A */
        status.temperature_c = 45;     /* 45 C */
        status.counter = counter++;
        status.checksum = 0U;

        if (can_encode_actuator_status(&status, data) &&
            app_can_send(CAN_ID_ACTUATOR_STATUS, data, 8U)) {
            app_can_print("TX 0x101 Actuator_Status\r\n");
        }

        osDelay(1000U);  /* 调试阶段 1 秒一帧，通了以后改成 50ms */
    }
}