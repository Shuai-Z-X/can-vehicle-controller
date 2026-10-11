#include "bsp_zdt_motor.h"

/* ==========================================================================
 * 低位辅助函数
 * ========================================================================== */

/* 大端序写入：高字节在前 */
static void zdt_put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFU);
}

static void zdt_put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)((value >> 16) & 0xFFU);
    p[2] = (uint8_t)((value >> 8) & 0xFFU);
    p[3] = (uint8_t)(value & 0xFFU);
}

static uint16_t zdt_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t zdt_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* 把串口接收缓冲里残留的字节读干净，避免上一次的回复串到这一次 */
static void zdt_flush_rx(ZDT_Motor_t *motor)
{
    uint8_t  dummy;
    uint32_t guard = 0U;

    while ((__HAL_UART_GET_FLAG(motor->huart, UART_FLAG_RXNE) != RESET) &&
           (guard < 64U)) {
        (void)HAL_UART_Receive(motor->huart, &dummy, 1U, 1U);
        guard++;
    }

    /* 清掉溢出标志：否则后面每次接收都会被这个残留错误影响 */
    __HAL_UART_CLEAR_OREFLAG(motor->huart);
}

/* 用指定地址发一帧：[地址][功能码][数据...][0x6B] */
static void zdt_send_ex(ZDT_Motor_t *motor, uint8_t addr, uint8_t code,
                        const uint8_t *data, uint8_t data_len)
{
    uint8_t frame[ZDT_FRAME_MAX];
    uint8_t n = 0U;
    uint8_t i;

    frame[n++] = addr;
    frame[n++] = code;

    for (i = 0U; i < data_len; i++) {
        frame[n++] = data[i];
    }

    frame[n++] = ZDT_FRAME_TAIL;

    (void)HAL_UART_Transmit(motor->huart, frame, n, ZDT_TX_TIMEOUT_MS);
}

/* 用电机自己的地址发一帧 */
static void zdt_send(ZDT_Motor_t *motor, uint8_t code,
                     const uint8_t *data, uint8_t data_len)
{
    zdt_send_ex(motor, motor->addr, code, data, data_len);
}

/*
 * 发一条"只需要确认收到"的命令，并收掉它的应答。
 * 应答帧固定 4 字节：[地址][功能码][02/E2/EE][0x6B]
 *   0x02 = 命令接收正确
 *   0xE2 = 参数错误或条件不满足
 *   0xEE = 帧格式错误
 */
static bool zdt_send_and_ack(ZDT_Motor_t *motor, uint8_t code,
                             const uint8_t *data, uint8_t data_len)
{
    uint8_t rx[4];

    zdt_flush_rx(motor);
    zdt_send(motor, code, data, data_len);

#if ZDT_WAIT_ACK
    if (HAL_UART_Receive(motor->huart, rx, 4U, ZDT_RX_TIMEOUT_MS) != HAL_OK) {
        motor->fb.online = 0U;
        return false;
    }

    if ((rx[0] != motor->addr) || (rx[1] != code) ||
        (rx[3] != ZDT_FRAME_TAIL)) {
        motor->fb.online = 0U;
        return false;
    }

    motor->fb.online = 1U;
    return (rx[2] == 0x02U);
#else
    /* 电机被设成"不返回信息"，命令发出去了就算成功 */
    motor->fb.online = 1U;
    return true;
#endif
}

/* ==========================================================================
 * 对外 API
 * ========================================================================== */

void ZDT_Create(ZDT_Motor_t *motor, UART_HandleTypeDef *huart, uint8_t addr)
{
    motor->huart = huart;
    motor->addr = addr;
    motor->enabled = 0U;

    motor->fb.position_x10 = 0;
    motor->fb.target_pos_x10 = 0;
    motor->fb.speed_x10 = 0;
    motor->fb.bus_voltage_mv = 0U;
    motor->fb.bus_current_ma = 0U;
    motor->fb.phase_current_ma = 0U;
    motor->fb.temperature_c = 0;
    motor->fb.status_flag = 0U;
    motor->fb.origin_flag = 0U;
    motor->fb.online = 0U;
}

/* ------------------------------ 触发类 ------------------------------ */

/* 使能控制：Addr F3 AB 状态 同步 6B */
bool ZDT_EnControl(ZDT_Motor_t *motor, bool enable, bool sync)
{
    uint8_t data[3];

    data[0] = 0xABU;
    data[1] = enable ? 0x01U : 0x00U;
    data[2] = sync ? 0x01U : 0x00U;

    motor->enabled = enable ? 1U : 0U;
    return zdt_send_and_ack(motor, 0xF3U, data, 3U);
}

/* 把当前位置清零：Addr 0A 6D 6B */
bool ZDT_ClearPosition(ZDT_Motor_t *motor)
{
    uint8_t data[1];

    data[0] = 0x6DU;
    return zdt_send_and_ack(motor, 0x0AU, data, 1U);
}

/* 把当前位置设为单圈回零的零点：Addr 93 88 是否存储 6B */
bool ZDT_OriginSetZero(ZDT_Motor_t *motor, bool store)
{
    uint8_t data[2];

    data[0] = 0x88U;
    data[1] = store ? 0x01U : 0x00U;
    return zdt_send_and_ack(motor, 0x93U, data, 2U);
}

/* 触发回零：Addr 9A 回零模式 同步 6B */
bool ZDT_OriginTriggerReturn(ZDT_Motor_t *motor, ZDT_OriginMode_t mode, bool sync)
{
    uint8_t data[2];

    data[0] = (uint8_t)mode;
    data[1] = sync ? 0x01U : 0x00U;
    return zdt_send_and_ack(motor, 0x9AU, data, 2U);
}

/* 强制中断并退出回零：Addr 9C 48 6B */
bool ZDT_OriginInterrupt(ZDT_Motor_t *motor)
{
    uint8_t data[1];

    data[0] = 0x48U;
    return zdt_send_and_ack(motor, 0x9CU, data, 1U);
}

/* ---------------------------- 运动控制 ---------------------------- */

/*
 * 速度模式（X 固件）：Addr F6 方向 加速度(2B) 速度(2B) 同步 6B
 *   acc_rpm_s : 加速度，单位 RPM/s
 *   speed_x10 : 速度，单位 0.1RPM（3000 = 300.0RPM）
 */
bool ZDT_VelControl(ZDT_Motor_t *motor, ZDT_Dir_t dir,
                    uint16_t acc_rpm_s, uint16_t speed_x10, bool sync)
{
#if ZDT_FIRMWARE_EMM
    /* Emm 固件：Addr F6 方向 速度(2B,RPM) 加速度(1B,0-255档) 同步 6B */
    uint8_t data[5];
    uint16_t speed_rpm = speed_x10 / 10U;

    if (speed_rpm > 0x0BB8U) {
        speed_rpm = 0x0BB8U;      /* Emm 固件速度上限 3000RPM */
    }

    data[0] = (uint8_t)dir;
    zdt_put_be16(&data[1], speed_rpm);
    data[3] = (acc_rpm_s > 255U) ? 255U : (uint8_t)acc_rpm_s;
    data[4] = sync ? 0x01U : 0x00U;

    return zdt_send_and_ack(motor, 0xF6U, data, 5U);
#else
    /* X 固件：Addr F6 方向 加速度(2B,RPM/s) 速度(2B,0.1RPM) 同步 6B */
    uint8_t data[6];

    data[0] = (uint8_t)dir;
    zdt_put_be16(&data[1], acc_rpm_s);
    zdt_put_be16(&data[3], speed_x10);
    data[5] = sync ? 0x01U : 0x00U;

    return zdt_send_and_ack(motor, 0xF6U, data, 6U);
#endif
}

/*
 * 直通限速位置模式（X 固件）：Addr FB 方向 速度(2B) 位置(4B) 运动模式 同步 6B
 * 不做加减速规划，速度到了就直接冲，适合快速点位。
 */
bool ZDT_BypassPosControl(ZDT_Motor_t *motor, ZDT_Dir_t dir, uint16_t speed_x10,
                          uint32_t pos_x10, ZDT_Raf_t raf, bool sync)
{
#if ZDT_FIRMWARE_EMM
    /* Emm 固件没有"直通限速位置"模式，退化成普通位置模式 */
    (void)motor;
    return ZDT_TrajPosControl(motor, dir, 0U, 0U, speed_x10, pos_x10, raf, sync);
#else
    uint8_t data[9];

    data[0] = (uint8_t)dir;
    zdt_put_be16(&data[1], speed_x10);
    zdt_put_be32(&data[3], pos_x10);
    data[7] = (uint8_t)raf;
    data[8] = sync ? 0x01U : 0x00U;

    return zdt_send_and_ack(motor, 0xFBU, data, 9U);
#endif
}

/*
 * 梯形曲线加减速位置模式（X 固件）—— 推荐用的位置控制
 *   Addr FD 方向 加速度(2B) 减速度(2B) 最大速度(2B) 位置(4B) 运动模式 同步 6B
 * 共 16 字节，是协议里最长的一条命令。
 */
bool ZDT_TrajPosControl(ZDT_Motor_t *motor, ZDT_Dir_t dir,
                        uint16_t acc_rpm_s, uint16_t dec_rpm_s, uint16_t speed_x10,
                        uint32_t pos_x10, ZDT_Raf_t raf, bool sync)
{
#if ZDT_FIRMWARE_EMM
    /*
     * Emm 固件没有梯形曲线模式，退化成普通位置模式：
     *   Addr FD 方向 速度(2B,RPM) 加速度(1B) 脉冲数(4B) 运动模式 同步 6B
     * 位置单位是脉冲：1.8° 电机 16 细分时，3200 个脉冲转一圈。
     */
    uint8_t data[10];
    uint16_t speed_rpm = speed_x10 / 10U;
    uint32_t pulses = (pos_x10 * 8U) / 9U;    /* 0.1° -> 脉冲(×3200/3600) */

    (void)dec_rpm_s;                          /* Emm 固件用不到减速度 */

    if (speed_rpm > 0x0BB8U) {
        speed_rpm = 0x0BB8U;
    }

    data[0] = (uint8_t)dir;
    zdt_put_be16(&data[1], speed_rpm);
    data[3] = (acc_rpm_s > 255U) ? 255U : (uint8_t)acc_rpm_s;
    zdt_put_be32(&data[4], pulses);
    data[8] = (uint8_t)raf;
    data[9] = sync ? 0x01U : 0x00U;

    return zdt_send_and_ack(motor, 0xFDU, data, 10U);
#else
    /* X 固件：梯形曲线加减速位置模式，共 16 字节 */
    uint8_t data[13];

    data[0] = (uint8_t)dir;
    zdt_put_be16(&data[1], acc_rpm_s);
    zdt_put_be16(&data[3], dec_rpm_s);
    zdt_put_be16(&data[5], speed_x10);
    zdt_put_be32(&data[7], pos_x10);
    data[11] = (uint8_t)raf;
    data[12] = sync ? 0x01U : 0x00U;

    return zdt_send_and_ack(motor, 0xFDU, data, 13U);
#endif
}

/* 立即停止：Addr FE 98 同步 6B */
bool ZDT_StopNow(ZDT_Motor_t *motor, bool sync)
{
    uint8_t data[2];

    data[0] = 0x98U;
    data[1] = sync ? 0x01U : 0x00U;
    return zdt_send_and_ack(motor, 0xFEU, data, 2U);
}

/* 触发多机同步运动：Addr FF 66 6B（广播地址 0x00 可以让所有电机同时启动） */
bool ZDT_SyncMotion(ZDT_Motor_t *motor)
{
    uint8_t data[1];

    data[0] = 0x66U;
    return zdt_send_and_ack(motor, 0xFFU, data, 1U);
}

/* --------------------------- 读取系统参数 --------------------------- */

/*
 * 每种参数的返回长度不一样，这里按手册逐个写清楚：
 *   状态标志            : 地址 3A 标志 6B                    -> 4 字节
 *   温度                : 地址 39 符号 温度 6B                -> 5 字节
 *   回零+电机状态       : 地址 3C 回零标志 电机标志 6B        -> 5 字节
 *   转速                : 地址 35 符号 速度(2B) 6B            -> 6 字节
 *   电压/电流/相电流    : 地址 xx 数值(2B) 6B                 -> 5 字节
 *   位置/目标位置/误差  : 地址 xx 符号 位置(4B) 6B            -> 8 字节
 */
static uint8_t zdt_param_rx_len(ZDT_SysParam_t param)
{
    switch (param) {
    case ZDT_S_FLAG:
    case ZDT_S_OFLAG:
        return 4U;
    case ZDT_S_TEMP:
    case ZDT_S_VBUS:
    case ZDT_S_CBUS:
    case ZDT_S_CPHA:
    case ZDT_S_OAF:
        return 5U;
    case ZDT_S_VEL:
        return 6U;
    case ZDT_S_CPOS:
    case ZDT_S_TPOS:
    case ZDT_S_PERR:
    default:
        return 8U;
    }
}

bool ZDT_ReadSysParam(ZDT_Motor_t *motor, ZDT_SysParam_t param)
{
    uint8_t rx[8];
    uint8_t need = zdt_param_rx_len(param);
    uint32_t raw;

    zdt_flush_rx(motor);
    zdt_send(motor, (uint8_t)param, 0, 0U);

    if (HAL_UART_Receive(motor->huart, rx, need, ZDT_RX_TIMEOUT_MS) != HAL_OK) {
        motor->fb.online = 0U;
        return false;
    }

    if ((rx[0] != motor->addr) || (rx[1] != (uint8_t)param) ||
        (rx[need - 1U] != ZDT_FRAME_TAIL)) {
        motor->fb.online = 0U;
        return false;
    }

    motor->fb.online = 1U;

    switch (param) {
    case ZDT_S_CPOS:
        raw = zdt_get_be32(&rx[3]);
#if ZDT_FIRMWARE_EMM
        /* Emm 固件的位置单位是脉冲（1.8°电机16细分 = 3200脉冲/圈），
         * 统一换算成 0.1° 再交给上层：pulse × 360/3200 × 10 = pulse × 9/8 */
        raw = (raw * 9U) / 8U;
#endif
        motor->fb.position_x10 = (rx[2] == 0x01U) ? -(int32_t)raw : (int32_t)raw;
        break;

    case ZDT_S_TPOS:
        raw = zdt_get_be32(&rx[3]);
#if ZDT_FIRMWARE_EMM
        raw = (raw * 9U) / 8U;
#endif
        motor->fb.target_pos_x10 = (rx[2] == 0x01U) ? -(int32_t)raw : (int32_t)raw;
        break;

    case ZDT_S_PERR:
        raw = zdt_get_be32(&rx[3]);
        break;      /* 位置误差暂时不用，先保证帧解析正确 */

    case ZDT_S_VEL:
        raw = zdt_get_be16(&rx[3]);
#if ZDT_FIRMWARE_EMM
        /* Emm 固件的转速单位是 1RPM，统一换算成 0.1RPM */
        raw = raw * 10U;
#endif
        motor->fb.speed_x10 = (rx[2] == 0x01U) ? -(int16_t)raw : (int16_t)raw;
        break;

    case ZDT_S_TEMP:
        motor->fb.temperature_c = (rx[2] == 0x01U) ? (int16_t)rx[3]
                                                   : -(int16_t)rx[3];
        break;

    case ZDT_S_VBUS:
        motor->fb.bus_voltage_mv = zdt_get_be16(&rx[2]);
        break;

    case ZDT_S_CBUS:
        motor->fb.bus_current_ma = zdt_get_be16(&rx[2]);
        break;

    case ZDT_S_CPHA:
        motor->fb.phase_current_ma = zdt_get_be16(&rx[2]);
        break;

    case ZDT_S_FLAG:
        motor->fb.status_flag = rx[2];
        break;

    case ZDT_S_OFLAG:
        motor->fb.origin_flag = rx[2];
        break;

    case ZDT_S_OAF:
        motor->fb.origin_flag = rx[2];
        motor->fb.status_flag = rx[3];
        break;

    default:
        break;
    }

    return true;
}

/* ------------------------- 上电自检用的两条命令 ------------------------- */

/*
 * 广播读 ID：发送 00 15 6B
 * 用广播地址发出去，任何地址的电机都会回复，
 * 返回帧是 [Addr][15][电机地址][6B]。
 */
uint16_t ZDT_RawCommand(ZDT_Motor_t *motor, uint8_t addr, uint8_t code,
                        const uint8_t *data, uint8_t data_len,
                        uint8_t *raw, uint16_t max_len)
{
    uint16_t n = 0U;

    zdt_flush_rx(motor);
    zdt_send_ex(motor, addr, code, data, data_len);

    /* 第一个字节多等一会儿，后面的字节间隙很短，几毫秒收不到就算结束 */
    while (n < max_len) {
        uint32_t timeout = (n == 0U) ? ZDT_RX_TIMEOUT_MS : 5U;

        if (HAL_UART_Receive(motor->huart, &raw[n], 1U, timeout) != HAL_OK) {
            break;
        }
        n++;
    }

    return n;
}

uint16_t ZDT_ReadIdRaw(ZDT_Motor_t *motor, uint8_t *raw, uint16_t max_len)
{
    return ZDT_RawCommand(motor, ZDT_BROADCAST_ADDR, 0x15U, 0, 0U,
                          raw, max_len);
}

/*
 * 读选项参数状态：发送 Addr 1A 6B，返回 [Addr][1A][flags1][flags2][6B]
 * flags1 的 bit1 就是固件类型（0=X 固件，1=Emm 固件）。
 */
bool ZDT_ReadOptionFlags(ZDT_Motor_t *motor, uint8_t *flags1, uint8_t *flags2)
{
    uint8_t rx[5];

    zdt_flush_rx(motor);
    zdt_send(motor, 0x1AU, 0, 0U);

    if (HAL_UART_Receive(motor->huart, rx, 5U, ZDT_RX_TIMEOUT_MS) != HAL_OK) {
        return false;
    }

    if ((rx[0] != motor->addr) || (rx[1] != 0x1AU) ||
        (rx[4] != ZDT_FRAME_TAIL)) {
        return false;
    }

    if (flags1 != 0) {
        *flags1 = rx[2];
    }
    if (flags2 != 0) {
        *flags2 = rx[3];
    }
    return true;
}

/*
 * USART2 自环自检：把 PA2 和 PA3 短接，看发出去的字节能不能原样收回来。
 * 这一条只验证"单片机自己的串口通不通"，跟电机没有关系。
 */
bool ZDT_LoopbackTest(ZDT_Motor_t *motor)
{
    static const uint8_t tx_pattern[4] = {0x55U, 0xAAU, 0x5AU, 0xA5U};
    uint8_t rx;
    uint8_t i;

    zdt_flush_rx(motor);

    /*
     * 必须"发一个字节、马上收一个字节"，不能一次把 4 个字节全发完再收：
     * 自环回来的字节是在发送过程中就回来的，如果这段时间没人读，
     * 硬件接收寄存器会被后面的字节覆盖（溢出），测试就会假报 FAIL。
     */
    for (i = 0U; i < 4U; i++) {
        if (HAL_UART_Transmit(motor->huart, (uint8_t *)&tx_pattern[i], 1U,
                              ZDT_TX_TIMEOUT_MS) != HAL_OK) {
            return false;
        }

        if (HAL_UART_Receive(motor->huart, &rx, 1U, 50U) != HAL_OK) {
            return false;
        }

        if (rx != tx_pattern[i]) {
            return false;
        }
    }

    return true;
}

/************************ (C) COPYRIGHT *****END OF FILE***********************/
