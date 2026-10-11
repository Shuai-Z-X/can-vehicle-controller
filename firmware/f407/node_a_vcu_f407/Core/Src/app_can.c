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

/* ==================== 第 2 步：报文周期 ====================
 * 0x100 VCU_Status 和 0x200 Control_Cmd 都按 20ms 一帧发送。
 * 用 osDelayUntil 做“绝对时刻”延时，周期不会因为任务执行时间而漂移。
 */
#define VCU_TX_PERIOD_MS 20U

/* ==================== 第 3 步：CAN 通讯超时 ====================
 * 500ms 收不到 0x101 Actuator_Status，就认为和执行器失去通讯。
 * 恢复收到 0x101 后自动退出故障。
 */
#define ACTUATOR_TIMEOUT_MS   500U
#define VCU_FAULT_CAN_TIMEOUT 0x01U  /* fault_flags 的 bit0 = CAN 超时 */

/* ==================== 第 5 步：串口打印节流 ====================
 * 每 50 帧打印一次，20ms * 50 = 1s 一行，不会刷屏。
 */
#define PRINT_EVERY_N_TX 50U
#define PRINT_EVERY_N_RX 20U

/* ------------------------------------------------------------------
 * 跨任务共享状态
 * Tx 任务和 Rx 任务都会读写这些变量，所以必须加 volatile：
 * 让编译器每次都真正去内存里读/写，而不是把它缓存到寄存器里。
 * ------------------------------------------------------------------ */
static volatile uint32_t g_last_rx_101_tick = 0U; /* 最近一帧 0x101 到达的 tick */
static volatile uint8_t  g_rx_101_seen = 0U;      /* 是否收到过第一帧 0x101 */
static volatile uint8_t  g_vcu_mode = CAN_MODE_STANDBY;
static volatile uint8_t  g_vcu_fault_flags = 0U;
static volatile uint16_t g_vcu_speed_x10 = 123U;      /* 12.3 km/h */
static volatile uint16_t g_vcu_voltage_x100 = 2450U;  /* 24.50 V */
static volatile uint32_t g_rx_101_count = 0U;         /* 收到多少帧 0x101（诊断用） */

/* ------------------------------------------------------------------
 * SLCAN 网关用的帧回调。
 * CAN 节点收不到自己发出去的报文，所以自己发的帧要单独回调一次，
 * 否则上位机看不到 0x100 / 0x200。
 * ------------------------------------------------------------------ */
static volatile AppCan_FrameHook_t g_tx_hook = 0;
static volatile AppCan_FrameHook_t g_rx_hook = 0;

static osMutexId_t g_uart_mutex = NULL;

static const char *app_can_mode_text(uint8_t mode)
{
    switch (mode) {
    case CAN_MODE_OFF:     return "OFF";
    case CAN_MODE_STANDBY: return "STANDBY";
    case CAN_MODE_RUN:     return "RUN";
    case CAN_MODE_FAULT:   return "FAULT";
    default:               return "UNKNOWN";
    }
}

static void app_can_print(const char *text)
{
     /* 两个任务都可能打印，加互斥量避免串口输出交叉成乱码 */
    if (g_uart_mutex != NULL) {
        (void)osMutexAcquire(g_uart_mutex, osWaitForever);
    }

    (void)HAL_UART_Transmit(&huart1,
                            (uint8_t *)text,
                            (uint16_t)strlen(text),
                            100U);

    if (g_uart_mutex != NULL) {
        (void)osMutexRelease(g_uart_mutex);
    }
}

/* 把无符号整数转成十进制字符串（手写转换，避免引入 printf） */
static void app_can_fmt_u32(uint32_t value, char *out)
{
    char digits[12];
    uint8_t count = 0U;
    uint8_t i = 0U;

    if (value == 0U) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    while ((value > 0U) && (count < 10U)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (count > 0U) {
        out[i++] = digits[--count];
    }

    out[i] = '\0';
}

/* 第 5 步：打印工作状态、总线错误计数和收帧数：
 *   STATUS: mode=RUN fault=0x00 tec=0 rec=0 rx=1234
 *
 * tec = CAN 发送错误计数，rec = CAN 接收错误计数。
 * 这两个数一直往上涨说明是总线本身的问题（没接终端电阻、CANH/CANL
 * 接反、两边波特率不一致），而不是程序写错了；
 * rx 是从上电到现在收到的 0x101 帧数，rx 一直不涨说明这一侧收不到。
 */
static void app_can_print_status(void)
{
    char line[80];
    char num[12];
    uint32_t esr = CAN_HANDLE.Instance->ESR;

    (void)strcpy(line, "STATUS: mode=");
    (void)strcat(line, app_can_mode_text(g_vcu_mode));
    (void)strcat(line, " fault=");
    (void)strcat(line,
                 (g_vcu_fault_flags & VCU_FAULT_CAN_TIMEOUT) ? "0x01" : "0x00");

    (void)strcat(line, " tec=");
    app_can_fmt_u32((esr >> 16) & 0xFFU, num);
    (void)strcat(line, num);

    (void)strcat(line, " rec=");
    app_can_fmt_u32((esr >> 24) & 0xFFU, num);
    (void)strcat(line, num);

    (void)strcat(line, " rx=");
    app_can_fmt_u32(g_rx_101_count, num);
    (void)strcat(line, num);

    (void)strcat(line, "\r\n");

    app_can_print(line);
}

/* ------------------------------------------------------------------
 * 总线错误恢复
 * 终端电阻没接、CANH/CANL 接反、两边波特率不一致，都会让错误计数一直
 * 上涨，最后 CAN 外设进入 Bus-Off 并彻底停止收发。
 * 这里检测到 Bus-Off 就重新启动一次 CAN，让它自动恢复。
 * ------------------------------------------------------------------ */
static void app_can_recover_bus_off(void)
{
    if ((CAN_HANDLE.Instance->ESR & CAN_ESR_BOFF) != 0U) {
        (void)HAL_CAN_Stop(&CAN_HANDLE);
        (void)HAL_CAN_Start(&CAN_HANDLE);
        app_can_print("CAN bus-off detected -> restart CAN\r\n");
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
    g_uart_mutex = osMutexNew(NULL);
    if (g_uart_mutex == NULL) {
        /* 互斥量创建失败基本只有一个原因：FreeRTOS 堆太小。
         * 这里不 return，即使没有互斥量 CAN 功能也不受影响。 */
    }

    /* 第 3 步：上电先记录一个基准时刻，500ms 内收不到 0x101 就进故障 */
    g_last_rx_101_tick = osKernelGetTickCount();
    g_rx_101_seen = 0U;
    g_vcu_mode = CAN_MODE_STANDBY;
    g_vcu_fault_flags = 0U;

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

/* 真正把帧塞进 CAN 发送邮箱，不做任何回调 */
static bool app_can_send_raw(uint32_t id, const uint8_t *data, uint8_t dlc)
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

/* 本节点主动发送：发成功后回调一次，让网关转发给上位机 */
static bool app_can_send(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    bool ok = app_can_send_raw(id, data, dlc);

    if (ok && (g_tx_hook != 0)) {
        g_tx_hook(id, data, dlc);
    }

    return ok;
}

void AppCan_TaskCanTx(void *argument)
{
    uint8_t vcu_counter = 0U;
    uint8_t cmd_counter = 0U;
    uint32_t tx_div = 0U;
    uint32_t last_wake = osKernelGetTickCount();

    (void)argument;

    for (;;) {
        vcu_status_t vcu;
        control_cmd_t cmd;
        uint8_t data[8];
        uint32_t now = osKernelGetTickCount();
        uint8_t next_mode;
        bool comm_ok = ((uint32_t)(now - g_last_rx_101_tick) <=
                        (uint32_t)ACTUATOR_TIMEOUT_MS);

        /* ---------------- 总线被拖进 Bus-Off 时先救回来 ---------------- */
        app_can_recover_bus_off();

        /* ---------------- 第 3 步：0x101 超时判断 ---------------- */
        if (!comm_ok) {
            next_mode = CAN_MODE_FAULT;      /* 通讯丢了 -> 故障 */
        } else if (g_rx_101_seen != 0U) {
            next_mode = CAN_MODE_RUN;        /* 通讯正常 -> 运行 */
        } else {
            next_mode = CAN_MODE_STANDBY;    /* 还没收到第一帧 -> 待机 */
        }

        /* 只在状态发生跳变时打印，故障/恢复各打印一次 */
        if (next_mode != g_vcu_mode) {
            uint8_t old_mode = g_vcu_mode;

            g_vcu_mode = next_mode;

            if (next_mode == CAN_MODE_FAULT) {
                g_vcu_fault_flags |= VCU_FAULT_CAN_TIMEOUT;
                app_can_print("FAULT: CAN timeout, 0x101 lost > 500ms\r\n");
            } else {
                g_vcu_fault_flags &= (uint8_t)(~VCU_FAULT_CAN_TIMEOUT);
                if (old_mode == CAN_MODE_FAULT) {
                    app_can_print("CAN recovered: VCU back to RUN\r\n");
                } else {
                    app_can_print("CAN online: VCU RUN\r\n");
                }
            }
        }

        /* ---------- 第 2 步：20ms 发一帧 0x100 VCU_Status ---------- */
        vcu.mode = g_vcu_mode;
        vcu.speed_kmh_x10 = g_vcu_speed_x10;
        vcu.battery_voltage_x100 = g_vcu_voltage_x100;
        vcu.fault_flags = g_vcu_fault_flags;
        vcu.counter = vcu_counter++;
        vcu.checksum = 0U;

        if (can_encode_vcu_status(&vcu, data)) {
            (void)app_can_send(CAN_ID_VCU_STATUS, data, 8U);
        }

        /* ---------- 第 2 步：同一个 20ms 周期发 0x200 Control_Cmd ---------- */
        cmd.mode = g_vcu_mode;             /* 故障时把 FAULT 一起下发给执行器 */
        cmd.target_output_pct_x10 = 300U;  /* 30.0% */
        cmd.counter = cmd_counter++;
        cmd.checksum = 0U;

        if (can_encode_control_cmd(&cmd, data)) {
            (void)app_can_send(CAN_ID_CONTROL_CMD, data, 8U);
        }

        /* ---------- 第 5 步：每 1 秒打印一次状态 ---------- */
        if (++tx_div >= PRINT_EVERY_N_TX) {
            tx_div = 0U;
            app_can_print_status();
        }

        /* ---------- 第 2 步：固定 20ms 周期（绝对时刻延时） ---------- */
        last_wake += VCU_TX_PERIOD_MS;
        (void)osDelayUntil(last_wake);
    }
}

void AppCan_TaskCanRx(void *argument)
{
    uint32_t rx_div = 0U;

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

            /* 每一帧都交给网关（SLCAN 会把它转发给上位机） */
            if (g_rx_hook != 0) {
                g_rx_hook(rx_header.StdId, data, (uint8_t)rx_header.DLC);
            }

            if (rx_header.StdId == CAN_ID_ACTUATOR_STATUS &&
                rx_header.DLC == 8U) {
                actuator_status_t status;

                if (can_decode_actuator_status(data, &status)) {
                    /* ---------- 第 3 步：刷新通讯时间戳 ---------- */
                    g_last_rx_101_tick = osKernelGetTickCount();
                    g_rx_101_seen = 1U;
                    g_rx_101_count++;

                    if (++rx_div >= PRINT_EVERY_N_RX) {
                        rx_div = 0U;
                        app_can_print("RX 0x101 Actuator_Status\r\n");
                    }
                }
            } else {
                /* 其他 ID（包括自己发的 0x100/0x200）直接忽略 */
            }
        }

        osDelay(1U);
    }
}

/* ------------------------------------------------------------------
 * OLED 显示用的只读接口
 * 显示任务和 CAN 任务不在同一个线程里，所以这里都加 volatile 读取。
 * ------------------------------------------------------------------ */
uint8_t AppCan_GetVcuMode(void)
{
    return g_vcu_mode;
}

uint8_t AppCan_GetVcuFaultFlags(void)
{
    return g_vcu_fault_flags;
}

uint16_t AppCan_GetVcuSpeedX10(void)
{
    return g_vcu_speed_x10;
}

uint16_t AppCan_GetVcuVoltageX100(void)
{
    return g_vcu_voltage_x100;
}

/* ------------------------------------------------------------------
 * 给 SLCAN 网关用的接口
 * ------------------------------------------------------------------ */

void AppCan_SetTxHook(AppCan_FrameHook_t hook)
{
    g_tx_hook = hook;
}

void AppCan_SetRxHook(AppCan_FrameHook_t hook)
{
    g_rx_hook = hook;
}

/* 网关把上位机发来的帧塞进 CAN，不触发 Tx 回调（避免回显） */
bool AppCan_SendRaw(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    return app_can_send_raw(id, data, dlc);
}
