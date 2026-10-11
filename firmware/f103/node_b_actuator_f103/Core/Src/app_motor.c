#include "app_motor.h"

#include "app_can.h"
#include "can_protocol.h"
#include "cmsis_os.h"
#include "usart.h"
#include <string.h>

/* 电机实体：整个工程只有这一台 */
static ZDT_Motor_t g_motor;

/* ==========================================================================
 * 调试打印
 * 电机不转的时候，靠这几行就能判断是哪一环断的：
 *   mode 不是 RUN      -> CAN 那边没通，电机本来就不该转（问题不在电机）
 *   online=0           -> 串口和电机没通上（共地 / R T 接反 / 地址 / 波特率）
 *   online=1 但没转    -> 命令发出去了，看 read_ok 和 fb_spd 有没有变化
 * ========================================================================== */

/* 无符号整数转十进制字符串 */
static char *app_motor_append_u32(char *dst, uint32_t value)
{
    char digits[12];
    uint8_t n = 0U;
    uint8_t i;

    if (value == 0U) {
        *dst++ = '0';
        *dst = '\0';
        return dst;
    }

    while ((value > 0U) && (n < 10U)) {
        digits[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    for (i = n; i > 0U; i--) {
        *dst++ = digits[i - 1U];
    }
    *dst = '\0';
    return dst;
}

/* 一个字节写成两位大写十六进制 */
static char *app_motor_append_hex2(char *dst, uint8_t value)
{
    const char *hex = "0123456789ABCDEF";

    *dst++ = hex[(value >> 4) & 0x0FU];
    *dst++ = hex[value & 0x0FU];
    *dst = '\0';
    return dst;
}

static const char *app_motor_mode_text(uint8_t mode)
{
    switch (mode) {
    case CAN_MODE_OFF:     return "OFF";
    case CAN_MODE_STANDBY: return "STANDBY";
    case CAN_MODE_RUN:     return "RUN";
    case CAN_MODE_FAULT:   return "FAULT";
    default:               return "?";
    }
}

static void app_motor_print_raw(const char *text)
{
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)text,
                            (uint16_t)strlen(text), 200U);
}

/* 打印一行完整的电机状态 */
static void app_motor_print_status(uint8_t mode, uint16_t cmd_out,
                                   uint16_t target_speed, uint8_t online,
                                   uint32_t read_ok, uint32_t read_fail)
{
    const ZDT_Feedback_t *fb = &g_motor.fb;
    char line[200];
    char *p = line;

    (void)strcpy(p, "MOTOR: mode=");
    p += strlen(p);
    (void)strcpy(p, app_motor_mode_text(mode));
    p += strlen(p);

    (void)strcpy(p, " cmd_out=");
    p += strlen(p);
    p = app_motor_append_u32(p, cmd_out);

    (void)strcpy(p, " target_speed=");
    p += strlen(p);
    p = app_motor_append_u32(p, target_speed);

    (void)strcpy(p, " online=");
    p += strlen(p);
    *p++ = (online != 0U) ? '1' : '0';
    *p = '\0';

    (void)strcpy(p, " fb_spd=");
    p += strlen(p);
    if (fb->speed_x10 < 0) {
        *p++ = '-';
        *p = '\0';
        p = app_motor_append_u32(p, (uint32_t)(-fb->speed_x10));
    } else {
        p = app_motor_append_u32(p, (uint32_t)fb->speed_x10);
    }

    (void)strcpy(p, " fb_pos=");
    p += strlen(p);
    if (fb->position_x10 < 0) {
        *p++ = '-';
        *p = '\0';
        p = app_motor_append_u32(p, (uint32_t)(-fb->position_x10));
    } else {
        p = app_motor_append_u32(p, (uint32_t)fb->position_x10);
    }

    (void)strcpy(p, " vbus_mV=");
    p += strlen(p);
    p = app_motor_append_u32(p, fb->bus_voltage_mv);

    (void)strcpy(p, " ibus_mA=");
    p += strlen(p);
    p = app_motor_append_u32(p, fb->bus_current_ma);

    (void)strcpy(p, " temp=");
    p += strlen(p);
    if (fb->temperature_c < 0) {
        *p++ = '-';
        *p = '\0';
        p = app_motor_append_u32(p, (uint32_t)(-fb->temperature_c));
    } else {
        p = app_motor_append_u32(p, (uint32_t)fb->temperature_c);
    }

    (void)strcpy(p, " read_ok=");
    p += strlen(p);
    p = app_motor_append_u32(p, read_ok);

    (void)strcpy(p, " read_fail=");
    p += strlen(p);
    p = app_motor_append_u32(p, read_fail);

    (void)strcpy(p, "\r\n");

    app_motor_print_raw(line);
}

/* 把串口收到的原始字节按十六进制打出来，len=0 时打印 (nothing) */
static void app_motor_print_rx_hex(const char *label, const uint8_t *buf,
                                   uint16_t len)
{
    char line[96];
    char *p = line;
    uint16_t i;

    (void)strcpy(p, "MOTOR: ");
    p += strlen(p);
    (void)strcpy(p, label);
    p += strlen(p);
    (void)strcpy(p, " rx raw =");
    p += strlen(p);

    if (len == 0U) {
        (void)strcpy(p, " (nothing)\r\n");
        app_motor_print_raw(line);
        return;
    }

    for (i = 0U; i < len; i++) {
        *p++ = ' ';
        *p = '\0';
        p = app_motor_append_hex2(p, buf[i]);
    }

    (void)strcpy(p, "\r\n");
    app_motor_print_raw(line);
}

/* 打印 USART2 的寄存器状态和 HAL 状态，用来判断串口有没有卡死 */
static void app_motor_print_uart_state(void)
{
    char line[80];
    char *p = line;
    uint32_t sr = (uint32_t)huart2.Instance->SR;

    (void)strcpy(p, "MOTOR: uart SR=");
    p += strlen(p);
    p = app_motor_append_hex2(p, (uint8_t)((sr >> 8) & 0xFFU));
    p = app_motor_append_hex2(p, (uint8_t)(sr & 0xFFU));

    (void)strcpy(p, " gState=");
    p += strlen(p);
    p = app_motor_append_u32(p, (uint32_t)huart2.gState);

    (void)strcpy(p, " RxState=");
    p += strlen(p);
    p = app_motor_append_u32(p, (uint32_t)huart2.RxState);

    (void)strcpy(p, " err=");
    p += strlen(p);
    p = app_motor_append_u32(p, huart2.ErrorCode);

    (void)strcpy(p, "\r\n");
    app_motor_print_raw(line);
}

/* CAN 里的"目标输出百分比"换算成电机转速（0.1RPM） */
static uint16_t app_motor_speed_from_cmd(uint16_t output_pct_x10)
{
    uint32_t speed = (uint32_t)output_pct_x10 * APP_MOTOR_SPEED_GAIN;

    if (speed > APP_MOTOR_MAX_SPEED_X10) {
        speed = APP_MOTOR_MAX_SPEED_X10;    /* 封顶，别让 42 步进飞出去 */
    }

    return (uint16_t)speed;
}

void AppMotor_Init(void)
{
    uint8_t id = 0U;
    uint8_t f1 = 0U;
    uint8_t f2 = 0U;
    uint8_t raw[16];
    char line[128];
    char *p;

    ZDT_Create(&g_motor, &huart2, APP_MOTOR_ADDR);

    /* ------------------------------------------------------------------
     * 自检 1：广播读 ID（发送 00 15 6B）
     * 用广播地址发，不管电机的地址被设成几都会回复。
     * 这里把收到的**原始字节**全部打出来，即使格式不对也能看到。
     * 正常回复是 [Addr][15][地址][6B]。
     * ------------------------------------------------------------------ */
    {
        uint16_t raw_len = 0U;
        uint8_t  attempt;
        uint16_t i;

        /*
         * 连试 10 次，每次间隔 200ms：
         *   中间某次成功 -> 说明是接触不良（线松了、插头氧化）
         *   10 次全空   -> 说明这根线根本不通
         * 打出来的 [try N/10] 就是第几次才收到回复。
         */
        for (attempt = 1U; attempt <= 10U; attempt++) {
            raw_len = ZDT_ReadIdRaw(&g_motor, raw, (uint16_t)sizeof(raw));

            if ((raw_len >= 4U) && (raw[1] == 0x15U) &&
                (raw[raw_len - 1U] == 0x6BU)) {
                id = raw[2];
                break;
            }

            osDelay(200);
        }

        if (attempt > 10U) {
            attempt = 10U;
        }

        p = line;
        (void)strcpy(p, "MOTOR: 00 15 bcast rx raw =");
        p += strlen(p);

        if (raw_len == 0U) {
            (void)strcpy(p, " (nothing)");
            p += strlen(p);
        } else {
            for (i = 0U; i < raw_len; i++) {
                *p++ = ' ';
                *p = '\0';
                p = app_motor_append_hex2(p, raw[i]);
            }
        }

        (void)strcpy(p, " [try ");
        p += strlen(p);
        p = app_motor_append_u32(p, (uint32_t)attempt);
        (void)strcpy(p, "/10]\r\n");
        app_motor_print_raw(line);
    }

    if (id != 0U) {
        p = line;
        (void)strcpy(p, "MOTOR: motor is online, real addr=");
        p += strlen(p);
        p = app_motor_append_u32(p, id);
        (void)strcpy(p, "\r\n");
        app_motor_print_raw(line);

        if ((id != 0U) && (id != g_motor.addr)) {
            /* 电机的地址和我们代码里的不一样，自动跟上，省得改了 */
            g_motor.addr = id;
            app_motor_print_raw("MOTOR: addr auto adopted (can change APP_MOTOR_ADDR)\r\n");
        }
    } else {
        app_motor_print_raw("MOTOR: no valid reply to broadcast read-ID\r\n");

        /* 电机不回话时，顺便验证一下单片机自己的串口通不通。
         * 把 PA2 和 PA3 用杜邦线短接，看这里是不是 OK。 */
        if (ZDT_LoopbackTest(&g_motor)) {
            app_motor_print_raw("MOTOR: loopback=OK -> MCU UART OK, check motor side\r\n");
        } else {
            app_motor_print_raw("MOTOR: loopback=FAIL -> short PA2-PA3 then retry\r\n");
        }
    }

    /* ------------------------------------------------------------------
     * 自检 2：逐条命令探测
     * 把每条命令收到的原始字节都打出来，这样即使电机回复的长度和手册
     * 不一致，也能一眼看出来。排查完这一段可以删掉，只留正式操作。
     * ------------------------------------------------------------------ */

    /* 1A 读选项参数（能看出固件类型是 X 还是 Emm） */
    {
        uint16_t len = ZDT_RawCommand(&g_motor, g_motor.addr, 0x1AU, 0, 0U,
                                      raw, (uint16_t)sizeof(raw));
        app_motor_print_rx_hex("1A opt", raw, len);

        if ((len >= 4U) && (raw[1] == 0x1AU) && (raw[len - 1U] == 0x6BU)) {
            f1 = raw[2];
            p = line;
            (void)strcpy(p, "MOTOR: options=");
            p += strlen(p);
            p = app_motor_append_hex2(p, f1);
            (void)strcpy(p, " fw=");
            p += strlen(p);
            (void)strcpy(p, ((f1 & 0x02U) != 0U) ? "Emm" : "X");
            (void)strcpy(p, "\r\n");
            app_motor_print_raw(line);
        }
    }

    /* 36 读实时位置 */
    {
        uint16_t len = ZDT_RawCommand(&g_motor, g_motor.addr, 0x36U, 0, 0U,
                                      raw, (uint16_t)sizeof(raw));
        app_motor_print_rx_hex("36 pos", raw, len);
    }

    /* 0A 位置清零（顺便作为"控制命令"的回显样本） */
    {
        uint8_t d = 0x6DU;
        uint16_t len = ZDT_RawCommand(&g_motor, g_motor.addr, 0x0AU, &d, 1U,
                                      raw, (uint16_t)sizeof(raw));
        app_motor_print_rx_hex("0A clr", raw, len);
    }

    /* F3 使能（锁轴） */
    {
        uint8_t d[3];
        uint16_t len;

        d[0] = 0xABU;
        d[1] = 0x01U;
        d[2] = 0x00U;
        len = ZDT_RawCommand(&g_motor, g_motor.addr, 0xF3U, d, 3U,
                             raw, (uint16_t)sizeof(raw));
        app_motor_print_rx_hex("F3 ena", raw, len);

        if ((len >= 4U) && (raw[1] == 0xF3U) && (raw[len - 1U] == 0x6BU)) {
            g_motor.enabled = 1U;
        }
    }

    /* 串口寄存器 / HAL 状态，用来判断串口有没有卡死 */
    app_motor_print_uart_state();

    (void)f2;
}

void AppMotor_Task(void *argument)
{
    uint32_t tick = 0U;
    uint32_t read_ok = 0U;
    uint32_t read_fail = 0U;
    uint16_t last_speed = 0U;
    uint8_t  last_mode = 0xFFU;

    (void)argument;

    AppMotor_Init();

    for (;;) {
        uint8_t  mode = AppCan_GetActuatorMode();
        uint16_t cmd_output = AppCan_GetCmdOutputX10();
        uint8_t  online;

        /* ------------------------------------------------------------------
         * 1. 按 CAN 命令驱动电机
         *
         *    mode == RUN   -> 把目标输出百分比换算成转速，用速度模式下发
         *    mode != RUN   -> 立即停机（OFF / STANDBY / FAULT 都算安全状态）
         *
         *    只在目标值变化时才发命令，避免总线上一直刷重复的帧。
         * ------------------------------------------------------------------ */
        if (mode == CAN_MODE_RUN) {
            uint16_t speed_x10 = app_motor_speed_from_cmd(cmd_output);

            if (speed_x10 == 0U) {
                if (last_speed != 0U) {
                    (void)ZDT_StopNow(&g_motor, false);
                    last_speed = 0U;
                }
            } else if ((speed_x10 != last_speed) || (last_mode != mode)) {
                (void)ZDT_VelControl(&g_motor, ZDT_DIR_CW, APP_MOTOR_ACC_RPM_S,
                                     speed_x10, false);
                last_speed = speed_x10;
            }
        } else if (last_speed != 0U) {
            (void)ZDT_StopNow(&g_motor, false);
            last_speed = 0U;
        }
        last_mode = mode;

        /* ------------------------------------------------------------------
         * 2. 周期读反馈
         *    在线 -> 每轮都读位置和转速，温度电流降低频率读
         *    掉线 -> 每 10 轮才试探一次，避免一直卡在接收超时上，
         *            把 CPU 时间让给 OLED 显示任务
         * ------------------------------------------------------------------ */
        online = g_motor.fb.online;

        if (online != 0U) {
            if (ZDT_ReadSysParam(&g_motor, ZDT_S_CPOS)) {
                read_ok++;
            } else {
                read_fail++;
            }
            if (ZDT_ReadSysParam(&g_motor, ZDT_S_VEL)) {
                read_ok++;
            } else {
                read_fail++;
            }

            if ((tick % 10U) == 0U) {
                if (ZDT_ReadSysParam(&g_motor, ZDT_S_TEMP)) {
                    read_ok++;
                } else {
                    read_fail++;
                }
                if (ZDT_ReadSysParam(&g_motor, ZDT_S_CBUS)) {
                    read_ok++;
                } else {
                    read_fail++;
                }
            }
        } else if ((tick % 10U) == 0U) {
            if (ZDT_ReadSysParam(&g_motor, ZDT_S_CPOS)) {
                read_ok++;
            } else {
                read_fail++;
            }
        }

        /* ------------------------------------------------------------------
         * 3. 把真实反馈回填给 CAN 状态帧 0x101
         *    这样 OLED 和 Linux 上位机看到的就是电机真实数据，
         *    而不是写死的模拟值。
         * ------------------------------------------------------------------ */
        online = g_motor.fb.online;

        {
            int32_t  speed = g_motor.fb.speed_x10;      /* 0.1RPM */
            uint16_t out_pct_x10;
            uint16_t cur_a_x100;

            if (speed < 0) {
                speed = -speed;
            }

            /* 用同一个换算系数反推"实际输出百分比"（0.1%） */
            out_pct_x10 = (uint16_t)(speed / (int32_t)APP_MOTOR_SPEED_GAIN);

            /* 总线电流 mA -> 0.01A */
            cur_a_x100 = (uint16_t)(g_motor.fb.bus_current_ma / 10U);

            AppCan_SetMotorFeedback(online, out_pct_x10, cur_a_x100,
                                    g_motor.fb.temperature_c);
        }

        /* ---------- 4. 每 1 秒打一行完整状态，方便定位问题 ---------- */
        if ((tick % 20U) == 0U) {
            app_motor_print_status(mode, cmd_output, last_speed,
                                   g_motor.fb.online, read_ok, read_fail);
        }

        tick++;
        osDelay(APP_MOTOR_PERIOD_MS);
    }
}

const ZDT_Motor_t *AppMotor_Get(void)
{
    return &g_motor;
}
