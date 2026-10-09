#include "app_can_selftest.h"

#include "can.h"
#include "usart.h"
#include "cmsis_os.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * F407 工程的 CAN 句柄叫 hcan1，F103 工程的 CAN 句柄叫 hcan。
 * 用宏让同一份代码可以放进两个工程。
 */
#if defined(STM32F407xx)
#define CAN_SELFTEST_HANDLE hcan1
#else
#define CAN_SELFTEST_HANDLE hcan
#endif

#define CAN_SELFTEST_ID 0x200U
#define CAN_SELFTEST_DLC 8U
#define CAN_SELFTEST_TIMEOUT_MS 100U

/*
 * 1 = 自检时自动切到 Loopback，不需要改 CubeMX。
 * 接真实 CAN 总线时改成 0，并把 .ioc 里的 CAN 改成 Normal，
 * 否则自检任务会强制 CAN 回到 Loopback，导致真实总线不能通信。
 */
#ifndef CAN_SELFTEST_FORCE_LOOPBACK
#define CAN_SELFTEST_FORCE_LOOPBACK 0
#endif

static void can_selftest_print(const char *text)
{
    HAL_UART_Transmit(&huart1,
                      (uint8_t *)text,
                      (uint16_t)strlen(text),
                      100U);
}

static bool can_selftest_enter_loopback(void)
{
#if CAN_SELFTEST_FORCE_LOOPBACK
    if (CAN_SELFTEST_HANDLE.Init.Mode != CAN_MODE_LOOPBACK) {
        (void)HAL_CAN_DeInit(&CAN_SELFTEST_HANDLE);
        CAN_SELFTEST_HANDLE.Init.Mode = CAN_MODE_LOOPBACK;
        if (HAL_CAN_Init(&CAN_SELFTEST_HANDLE) != HAL_OK) {
            return false;
        }
        can_selftest_print("CAN self-test: forced loopback\r\n");
    }
    return true;
#else
    if (CAN_SELFTEST_HANDLE.Init.Mode != CAN_MODE_LOOPBACK) {
        can_selftest_print("CAN self-test: set CAN_MODE_LOOPBACK\r\n");
        return false;
    }
    return true;
#endif
}

static bool can_selftest_configure_filter(void)
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

    return HAL_CAN_ConfigFilter(&CAN_SELFTEST_HANDLE, &filter) == HAL_OK;
}

static bool can_selftest_start(void)
{
    if (!can_selftest_enter_loopback()) {
        can_selftest_print("CAN self-test: loopback setup failed\r\n");
        return false;
    }

    if (!can_selftest_configure_filter()) {
        can_selftest_print("CAN self-test: filter config failed\r\n");
        return false;
    }

    if (HAL_CAN_Start(&CAN_SELFTEST_HANDLE) != HAL_OK) {
        can_selftest_print("CAN self-test: start failed\r\n");
        return false;
    }

    can_selftest_print("CAN self-test: init OK\r\n");
    return true;
}

static bool can_selftest_run_once(void)
{
    CAN_TxHeaderTypeDef tx_header = {0};
    CAN_RxHeaderTypeDef rx_header = {0};
    uint8_t tx_data[8] = {0x02, 0x2C, 0x01, 0x00, 0x00, 0x00, 0x09, 0x34};
    uint8_t rx_data[8] = {0};
    uint32_t mailbox = 0U;
    uint32_t start_tick = 0U;

    tx_header.StdId = CAN_SELFTEST_ID;
    tx_header.DLC = CAN_SELFTEST_DLC;
    tx_header.IDE = CAN_ID_STD;
    tx_header.RTR = CAN_RTR_DATA;

    if (HAL_CAN_AddTxMessage(&CAN_SELFTEST_HANDLE,
                             &tx_header,
                             tx_data,
                             &mailbox) != HAL_OK) {
        return false;
    }

    /*
     * 回环模式下，发送出去的帧会进入 CAN_RX_FIFO0。
     * 最多等 100 ms，避免任务死等。
     */
    start_tick = osKernelGetTickCount();
    while (HAL_CAN_GetRxFifoFillLevel(&CAN_SELFTEST_HANDLE,
                                      CAN_RX_FIFO0) == 0U) {
        if ((osKernelGetTickCount() - start_tick) > CAN_SELFTEST_TIMEOUT_MS) {
            return false;
        }
        osDelay(1U);
    }

    if (HAL_CAN_GetRxMessage(&CAN_SELFTEST_HANDLE,
                             CAN_RX_FIFO0,
                             &rx_header,
                             rx_data) != HAL_OK) {
        return false;
    }

    return (rx_header.StdId == CAN_SELFTEST_ID) &&
           (rx_header.DLC == CAN_SELFTEST_DLC) &&
           (memcmp(rx_data, tx_data, sizeof(tx_data)) == 0);
}

void CanSelfTest_Task(void *argument)
{
    (void)argument;

    if (!can_selftest_start()) {
        for (;;) {
            osDelay(1000U);
        }
    }

    for (;;) {
        if (can_selftest_run_once()) {
            can_selftest_print("CAN loopback OK\r\n");
        } else {
            can_selftest_print("CAN loopback FAIL\r\n");
        }
        osDelay(1000U);
    }
}
