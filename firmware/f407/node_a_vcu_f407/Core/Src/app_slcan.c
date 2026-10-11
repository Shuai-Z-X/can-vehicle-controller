#include "app_slcan.h"

#include "app_can.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

/* ==========================================================================
 * 接收：USART2 中断 + 环形缓冲
 * 必须用中断收：115200 下一个字节只要 87us，如果靠任务轮询，
 * 被高优先级任务抢占超过 87us 就会丢字节，SLCAN 的行就断了。
 * ========================================================================== */
#define SLCAN_RX_BUF_SIZE 256U
#define SLCAN_LINE_MAX    64U

/* 待转发的 CAN 帧队列（发送侧由别的任务调用，实际发送由本任务做） */
#define SLCAN_TXQ_SIZE    8U

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} slcan_frame_t;

static volatile uint8_t  s_rx_buf[SLCAN_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0U;
static volatile uint16_t s_rx_tail = 0U;
static uint8_t s_rx_byte;

/* 队列元素本身不加 volatile：写入受临界区保护，头尾指针才是同步点 */
static slcan_frame_t s_txq[SLCAN_TXQ_SIZE];
static volatile uint8_t s_txq_head = 0U;
static volatile uint8_t s_txq_tail = 0U;

static char s_line[SLCAN_LINE_MAX];
static uint8_t s_line_len = 0U;

/* 通道是否已经打开（Linux 侧 ip link set can0 up 之后会发 'O'） */
static volatile uint8_t s_channel_open = 0U;

static const char s_hex[] = "0123456789ABCDEF";

/* ==========================================================================
 * 底层收发
 * ========================================================================== */

static void slcan_send_bytes(const char *text, uint16_t len)
{
    (void)HAL_UART_Transmit(&huart2, (uint8_t *)text, len, 100U);
}

static void slcan_reply_ok(void)
{
    slcan_send_bytes("\r", 1U);
}

static void slcan_reply_error(void)
{
    slcan_send_bytes("\a", 1U);      /* BEL，SLCAN 规定的出错应答 */
}

/* USART2 每收到一个字节进一次这里，塞进环形缓冲后重新开中断 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        uint16_t next = (uint16_t)((s_rx_head + 1U) % SLCAN_RX_BUF_SIZE);

        if (next != s_rx_tail) {
            s_rx_buf[s_rx_head] = s_rx_byte;
            s_rx_head = next;
        }
        /* 缓冲满了就丢这一个字节，不能让中断停摆 */

        (void)HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U);
    }
}

/* ==========================================================================
 * CAN -> 上位机：把一帧排进发送队列
 * ========================================================================== */

void AppSlcan_ForwardFrame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    uint8_t i;
    uint8_t next;

    /* 这个回调会被 CAN 收发两个任务调用，用临界区保护队列指针 */
    taskENTER_CRITICAL();

    next = (uint8_t)((s_txq_head + 1U) % SLCAN_TXQ_SIZE);
    if (next == s_txq_tail) {
        taskEXIT_CRITICAL();
        return;
    }

    s_txq[s_txq_head].id = id;
    s_txq[s_txq_head].dlc = (dlc > 8U) ? 8U : dlc;
    for (i = 0U; i < 8U; i++) {
        s_txq[s_txq_head].data[i] = (i < dlc) ? data[i] : 0U;
    }
    s_txq_head = next;

    taskEXIT_CRITICAL();
}

/* 把队列里的帧按 SLCAN 格式发给上位机 */
static void slcan_flush_txq(void)
{
    while (s_txq_tail != s_txq_head) {
        const slcan_frame_t *f = (const slcan_frame_t *)&s_txq[s_txq_tail];
        char buf[32];
        uint8_t n = 0U;
        uint8_t i;

        buf[n++] = 't';
        buf[n++] = s_hex[(f->id >> 8) & 0x0FU];
        buf[n++] = s_hex[(f->id >> 4) & 0x0FU];
        buf[n++] = s_hex[f->id & 0x0FU];
        buf[n++] = s_hex[f->dlc & 0x0FU];

        for (i = 0U; i < f->dlc; i++) {
            buf[n++] = s_hex[(f->data[i] >> 4) & 0x0FU];
            buf[n++] = s_hex[f->data[i] & 0x0FU];
        }
        buf[n++] = '\r';

        if (s_channel_open != 0U) {
            slcan_send_bytes(buf, n);
        }

        s_txq_tail = (uint8_t)((s_txq_tail + 1U) % SLCAN_TXQ_SIZE);
    }
}

/* ==========================================================================
 * 上位机 -> CAN：解析命令
 * ========================================================================== */

static uint8_t slcan_hex_nibble(char c)
{
    if ((c >= '0') && (c <= '9')) {
        return (uint8_t)(c - '0');
    }
    if ((c >= 'A') && (c <= 'F')) {
        return (uint8_t)(c - 'A' + 10);
    }
    if ((c >= 'a') && (c <= 'f')) {
        return (uint8_t)(c - 'a' + 10);
    }
    return 0xFFU;
}

/*
 * 标准帧发送命令：t III D DD DD ...（共 5 + 2*DLC 个字符）
 *   line[1..3] = 11 位 CAN ID 的十六进制
 *   line[4]    = DLC
 *   line[5..]  = 数据字节
 */
static void slcan_handle_tx(const char *line, uint8_t len)
{
    uint8_t data[8];
    uint32_t id;
    uint8_t dlc;
    uint8_t i;

    if (len < 5U) {
        slcan_reply_error();
        return;
    }

    for (i = 1U; i <= 3U; i++) {
        if (slcan_hex_nibble(line[i]) == 0xFFU) {
            slcan_reply_error();
            return;
        }
    }

    id = ((uint32_t)slcan_hex_nibble(line[1]) << 8) |
         ((uint32_t)slcan_hex_nibble(line[2]) << 4) |
         (uint32_t)slcan_hex_nibble(line[3]);

    if (slcan_hex_nibble(line[4]) == 0xFFU || slcan_hex_nibble(line[4]) > 8U) {
        slcan_reply_error();
        return;
    }
    dlc = slcan_hex_nibble(line[4]);

    if (len < (uint8_t)(5U + (2U * dlc))) {
        slcan_reply_error();
        return;
    }

    for (i = 0U; i < dlc; i++) {
        uint8_t hi = slcan_hex_nibble(line[5U + (2U * i)]);
        uint8_t lo = slcan_hex_nibble(line[6U + (2U * i)]);

        if ((hi == 0xFFU) || (lo == 0xFFU)) {
            slcan_reply_error();
            return;
        }
        data[i] = (uint8_t)((hi << 4) | lo);
    }

    if (!AppCan_SendRaw(id, data, dlc)) {
        slcan_reply_error();
        return;
    }

    slcan_reply_ok();
}

static void slcan_handle_line(const char *line, uint8_t len)
{
    if (len == 0U) {
        return;
    }

    switch (line[0]) {
    case 'S':                       /* 设置波特率，固件里 CAN 固定 500k，直接确认 */
    case 's':
        slcan_reply_ok();
        break;

    case 'O':                       /* 打开 CAN 通道 */
    case 'L':                       /* 只听模式，这里当成普通打开 */
        s_channel_open = 1U;
        slcan_reply_ok();
        break;

    case 'C':                       /* 关闭通道 */
        s_channel_open = 0U;
        slcan_reply_ok();
        break;

    case 'V':
    case 'v':
        slcan_send_bytes("V1013\r", 6U);   /* 版本号，格式 V<4 位> */
        break;

    case 'N':
        slcan_send_bytes("N0001\r", 6U);   /* 序列号 */
        break;

    case 'F':                       /* 状态标志：00 = 无错误 */
        slcan_send_bytes("F00\r", 4U);
        break;

    case 'Z':                       /* 时间戳开关，本实现不带时间戳 */
    case 'z':
        slcan_reply_ok();
        break;

    case 't':                       /* 发送标准帧 */
    case 'T':                       /* 发送扩展帧，本项目不用，按标准帧处理 */
        slcan_handle_tx(line, len);
        break;

    default:
        slcan_reply_error();
        break;
    }
}

/* ==========================================================================
 * 任务
 * ========================================================================== */

static void app_slcan_init(void)
{
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_txq_head = 0U;
    s_txq_tail = 0U;
    s_line_len = 0U;
    s_channel_open = 0U;

    /* 挂上 CAN 帧回调：自己发的 + 收到的，都转发给上位机 */
    AppCan_SetTxHook(AppSlcan_ForwardFrame);
    AppCan_SetRxHook(AppSlcan_ForwardFrame);

    /* 启动 USART2 逐字节接收中断 */
    (void)HAL_UART_Receive_IT(&huart2, &s_rx_byte, 1U);
}

void AppSlcan_Task(void *argument)
{
    (void)argument;

    app_slcan_init();

    for (;;) {
        /* ---------- 1. 把串口收到的字节拼成命令行 ---------- */
        if (s_rx_tail != s_rx_head) {
            uint8_t ch = s_rx_buf[s_rx_tail];
            s_rx_tail = (uint16_t)((s_rx_tail + 1U) % SLCAN_RX_BUF_SIZE);

            if ((ch == '\r') || (ch == '\n')) {
                if (s_line_len > 0U) {
                    slcan_handle_line(s_line, s_line_len);
                    s_line_len = 0U;
                }
            } else if (s_line_len < (SLCAN_LINE_MAX - 1U)) {
                s_line[s_line_len] = (char)ch;
                s_line_len++;
            } else {
                s_line_len = 0U;    /* 行太长，丢弃重新同步 */
            }
        }

        /* ---------- 2. 把待转发的 CAN 帧发给上位机 ---------- */
        slcan_flush_txq();

        osDelay(1);
    }
}
