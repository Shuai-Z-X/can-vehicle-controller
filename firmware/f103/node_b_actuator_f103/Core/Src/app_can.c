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
 * 0x101 Actuator_Status 按 50ms 一帧发送。
 */
#define ACT_TX_PERIOD_MS 50U

/* ==================== 第 4 步：CAN 通讯超时 ====================
 * 300ms 收不到 0x200 Control_Cmd，就认为和 VCU 失去通讯：
 * 模式切 FAULT，目标输出强制为 0（安全状态）。
 */
#define CMD_TIMEOUT_MS 300U

/* ==================== 第 5 步：串口打印节流 ====================
 * 20 帧 * 50ms = 1s 一行，不会刷屏。
 */
#define PRINT_EVERY_N_TX 20U
#define PRINT_EVERY_N_RX 50U

/* ------------------------------------------------------------------
 * 跨任务共享状态：Tx 任务和 Rx 任务都会读写，所以加 volatile。
 * ------------------------------------------------------------------ */
static volatile uint32_t g_last_rx_200_tick = 0U; /* 最近一帧 0x200 到达的 tick */
static volatile uint8_t  g_rx_200_seen = 0U;      /* 是否收到过第一帧 0x200 */
static volatile uint8_t  g_cmd_mode = CAN_MODE_STANDBY;   /* VCU 下发的模式 */
static volatile uint16_t g_cmd_output_x10 = 0U;           /* VCU 下发的目标输出 */
static volatile uint8_t  g_act_mode = CAN_MODE_STANDBY;   /* 实际生效的模式 */
static volatile uint16_t g_act_output_x10 = 0U;           /* 实际生效的输出 */
static volatile uint8_t  g_act_fault = 0U;                /* 0=正常 1=通讯故障 */
static volatile uint16_t g_act_current_x100 = 50U;        /* 0.50 A */
static volatile int16_t  g_act_temp_c = 45;               /* 45 C */
static volatile uint32_t g_rx_200_count = 0U;             /* 收到多少帧 0x200（诊断用） */

/* ------------------------------------------------------------------
 * 电机任务回填的真实反馈（ZDT X42S 闭环步进）
 * 电机在线时用这组值，离线时自动退回上面的模拟值，
 * 这样即使不接电机，整条链路也照样能跑起来。
 * ------------------------------------------------------------------ */
static volatile uint8_t  g_motor_fb_online = 0U;
static volatile uint16_t g_motor_fb_output_x10 = 0U;      /* 实测转速换算的输出 0.1% */
static volatile uint16_t g_motor_fb_current_x100 = 0U;    /* 总线电流 0.01A */
static volatile int16_t  g_motor_fb_temp_c = 0;           /* 驱动温度 C */

static osMutexId_t uartMutexHandle;

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
    if (uartMutexHandle != NULL) {
        (void)osMutexAcquire(uartMutexHandle, osWaitForever);
    }
    HAL_UART_Transmit(&huart1,(uint8_t *)text,(uint16_t)strlen(text),100U);
    if (uartMutexHandle != NULL) {
        (void)osMutexRelease(uartMutexHandle);
    }
}

/* 把“放大 10 倍”的整数转成小数字符串：300 -> "30.0"。
 * 手写转换是为了不引入 printf，省 flash 也更符合嵌入式的做法。 */
static void app_can_fmt_x10(uint16_t value_x10, char *out)
{
    char digits[6];
    uint8_t count = 0U;
    uint8_t i = 0U;
    uint8_t frac = (uint8_t)(value_x10 % 10U);
    uint16_t whole = (uint16_t)(value_x10 / 10U);

    if (whole == 0U) {
        digits[count++] = '0';
    } else {
        while ((whole > 0U) && (count < 5U)) {
            digits[count++] = (char)('0' + (whole % 10U));
            whole = (uint16_t)(whole / 10U);
        }
    }

    while (count > 0U) {
        out[i++] = digits[--count];
    }

    out[i++] = '.';
    out[i++] = (char)('0' + frac);
    out[i] = '\0';
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
 *   STATUS: mode=RUN out=30.0% fault=0x00 tec=0 rec=0 rx=1234
 *
 * tec / rec 是 CAN 的发送、接收错误计数，一直涨说明总线本身有问题；
 * rx 是从上电到现在收到的 0x200 帧数，rx 不涨说明这一侧没收到命令。
 */
static void app_can_print_status(void)
{
    char value[8];
    char num[12];
    char line[96];
    uint32_t esr = CAN_HANDLE.Instance->ESR;

    app_can_fmt_x10(g_act_output_x10, value);

    (void)strcpy(line, "STATUS: mode=");
    (void)strcat(line, app_can_mode_text(g_act_mode));
    (void)strcat(line, " out=");
    (void)strcat(line, value);
    (void)strcat(line, "% fault=");
    (void)strcat(line, (g_act_fault != 0U) ? "0x01" : "0x00");

    (void)strcat(line, " tec=");
    app_can_fmt_u32((esr >> 16) & 0xFFU, num);
    (void)strcat(line, num);

    (void)strcat(line, " rec=");
    app_can_fmt_u32((esr >> 24) & 0xFFU, num);
    (void)strcat(line, num);

    (void)strcat(line, " rx=");
    app_can_fmt_u32(g_rx_200_count, num);
    (void)strcat(line, num);

    (void)strcat(line, "\r\n");

    app_can_print(line);
}

/* ------------------------------------------------------------------
 * 总线错误恢复（和 F407 侧同理）：进入 Bus-Off 后 CAN 会彻底停止收发，
 * 检测到就把 CAN 重新启动一次。
 * ------------------------------------------------------------------ */
static void app_can_recover_bus_off(void)
{
    if ((CAN_HANDLE.Instance->ESR & CAN_ESR_BOFF) != 0U) {
        (void)HAL_CAN_Stop(&CAN_HANDLE);
        (void)HAL_CAN_Start(&CAN_HANDLE);
        app_can_print("CAN bus-off detected -> restart CAN\r\n");
    }
}

/* 电机在线就用真实反馈，离线就用原来的模拟值 */
static void app_can_effective_feedback(uint16_t *out_pct_x10,
                                       uint16_t *current_a_x100,
                                       int16_t  *temp_c)
{
    if (g_motor_fb_online != 0U) {
        *out_pct_x10 = g_motor_fb_output_x10;
        *current_a_x100 = g_motor_fb_current_x100;
        *temp_c = g_motor_fb_temp_c;
    } else {
        *out_pct_x10 = g_act_output_x10;
        *current_a_x100 = g_act_current_x100;
        *temp_c = g_act_temp_c;
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

    /* 第 4 步：上电先记录基准时刻，300ms 内收不到 0x200 就进故障 */
    g_last_rx_200_tick = osKernelGetTickCount();
    g_rx_200_seen = 0U;
    g_cmd_mode = CAN_MODE_STANDBY;
    g_cmd_output_x10 = 0U;
    g_act_mode = CAN_MODE_STANDBY;
    g_act_output_x10 = 0U;
    g_act_fault = 0U;

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
    uint32_t rx_div = 0U;
    uint32_t vcu_div = 0U;

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
                    /* ---------- 第 4 步：收到合法 0x200 就刷新时间戳 ---------- */
                    g_last_rx_200_tick = osKernelGetTickCount();
                    g_rx_200_seen = 1U;
                    g_rx_200_count++;
                    g_cmd_mode = cmd.mode;
                    g_cmd_output_x10 = cmd.target_output_pct_x10;

                    if (++rx_div >= PRINT_EVERY_N_RX) {
                        rx_div = 0U;
                        app_can_print("RX 0x200 Control_Cmd\r\n");
                    }

#ifdef LED_Pin
                    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
#endif
                }
            } else if (rx_header.StdId == CAN_ID_VCU_STATUS &&
                       rx_header.DLC == 8U) {
                vcu_status_t status;

                if (can_decode_vcu_status(data, &status)) {
                    if (++vcu_div >= PRINT_EVERY_N_RX) {
                        vcu_div = 0U;
                        app_can_print("RX 0x100 VCU_Status\r\n");
                    }
                }
            }
        }

        osDelay(1U);
    }
}

void AppCan_TaskCanTx(void *argument)
{
    uint8_t counter = 0U;
    uint32_t tx_div = 0U;
    uint32_t last_wake = osKernelGetTickCount();

    (void)argument;

    for (;;) {
        actuator_status_t status;
        uint8_t data[8];
        uint32_t now = osKernelGetTickCount();
        bool comm_ok = ((uint32_t)(now - g_last_rx_200_tick) <=
                        (uint32_t)CMD_TIMEOUT_MS);

        /* ---------------- 总线被拖进 Bus-Off 时先救回来 ---------------- */
        app_can_recover_bus_off();

        /* ---------------- 第 4 步：0x200 超时判断 ---------------- */
        if (!comm_ok) {
            if (g_act_fault == 0U) {
                g_act_fault = 1U;
                app_can_print("FAULT: CAN timeout, 0x200 lost > 300ms\r\n");
            }
            /* 安全状态：模式 FAULT，输出强制 0 */
            g_act_mode = CAN_MODE_FAULT;
            g_act_output_x10 = 0U;
        } else if (g_rx_200_seen == 0U) {
            /* 还没收到第一帧命令，先待机、不输出 */
            g_act_mode = CAN_MODE_STANDBY;
            g_act_output_x10 = 0U;
        } else {
            if (g_act_fault != 0U) {
                g_act_fault = 0U;
                app_can_print("CAN recovered: actuator online\r\n");
            }

            if (g_cmd_mode == CAN_MODE_RUN) {
                g_act_mode = CAN_MODE_RUN;
                g_act_output_x10 = g_cmd_output_x10;
            } else {
                /* OFF / STANDBY / FAULT 一律不输出 */
                g_act_mode = g_cmd_mode;
                g_act_output_x10 = 0U;
            }
        }

        /* ---------- 第 2 步：50ms 发一帧 0x101 Actuator_Status ---------- */
        {
            uint16_t out_pct_x10;
            uint16_t cur_a_x100;
            int16_t  temp_c;

            /* 电机在线时这三个字段是实测值，离线时是模拟值 */
            app_can_effective_feedback(&out_pct_x10, &cur_a_x100, &temp_c);

            status.mode = g_act_mode;
            status.output_pct_x10 = out_pct_x10;
            status.current_a_x100 = cur_a_x100;
            status.temperature_c = temp_c;
        }

        status.counter = counter++;
        status.checksum = 0U;

        if (can_encode_actuator_status(&status, data)) {
            (void)app_can_send(CAN_ID_ACTUATOR_STATUS, data, 8U);
        }

        /* ---------- 第 5 步：每 1 秒打印一次状态 ---------- */
        if (++tx_div >= PRINT_EVERY_N_TX) {
            tx_div = 0U;
            app_can_print_status();
        }

        /* ---------- 第 2 步：固定 50ms 周期（绝对时刻延时） ---------- */
        last_wake += ACT_TX_PERIOD_MS;
        (void)osDelayUntil(last_wake);
    }
}

/* ------------------------------------------------------------------
 * OLED 显示用的只读接口
 * ------------------------------------------------------------------ */
uint8_t AppCan_GetActuatorMode(void)
{
    return g_act_mode;
}

uint16_t AppCan_GetActuatorOutputX10(void)
{
    uint16_t out_pct_x10;
    uint16_t cur_a_x100;
    int16_t  temp_c;

    app_can_effective_feedback(&out_pct_x10, &cur_a_x100, &temp_c);
    return out_pct_x10;
}

uint8_t AppCan_GetActuatorFault(void)
{
    return g_act_fault;
}

uint16_t AppCan_GetActuatorCurrentX100(void)
{
    uint16_t out_pct_x10;
    uint16_t cur_a_x100;
    int16_t  temp_c;

    app_can_effective_feedback(&out_pct_x10, &cur_a_x100, &temp_c);
    return cur_a_x100;
}

int16_t AppCan_GetActuatorTempC(void)
{
    uint16_t out_pct_x10;
    uint16_t cur_a_x100;
    int16_t  temp_c;

    app_can_effective_feedback(&out_pct_x10, &cur_a_x100, &temp_c);
    return temp_c;
}

/* ------------------------------------------------------------------
 * 和电机任务之间的接口
 * ------------------------------------------------------------------ */

/* CAN 下发的目标输出（0.1%），电机任务用它换算转速 */
uint16_t AppCan_GetCmdOutputX10(void)
{
    return g_cmd_output_x10;
}

/* 电机任务回填真实反馈 */
void AppCan_SetMotorFeedback(uint8_t online, uint16_t output_pct_x10,
                             uint16_t current_a_x100, int16_t temp_c)
{
    g_motor_fb_online = (online != 0U) ? 1U : 0U;
    g_motor_fb_output_x10 = output_pct_x10;
    g_motor_fb_current_x100 = current_a_x100;
    g_motor_fb_temp_c = temp_c;
}
