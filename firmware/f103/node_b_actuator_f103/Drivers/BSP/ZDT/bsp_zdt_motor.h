#ifndef __BSP_ZDT_MOTOR_H
#define __BSP_ZDT_MOTOR_H

#include "main.h"
#include "usart.h"
#include <stdbool.h>

/* ==========================================================================
 * 张大头 ZDT X42S 第二代闭环步进电机 —— 串口协议 BSP 驱动
 *
 * 通讯格式（摘自 ZDT_X42S 用户手册第 5 章，默认 115200 8N1）：
 *
 *     主机发送: [地址][功能码][数据...][0x6B]
 *     电机返回: [地址][功能码][返回数据][0x6B]
 *
 * 三个必须记住的规则：
 *   1. 多字节参数一律 **大端序**（高字节在前、低字节在后）
 *   2. 0x6B 是默认的固定校验字节（可以改成 XOR/CRC8/Modbus）
 *   3. X 固件下：位置单位 0.1°、速度单位 0.1RPM、加速度单位 RPM/s
 *      （Emm 固件的位置单位是脉冲数，速度单位是 RPM，两者命令格式不同）
 *
 * 本驱动采用"发一条、等一条"的阻塞式收发，不占用中断和 DMA，
 * 适合用在一条独立的 FreeRTOS 任务里轮询。
 * ========================================================================== */

#define ZDT_FRAME_TAIL       0x6BU   /* 固定校验字节 */
#define ZDT_BROADCAST_ADDR   0x00U   /* 广播地址 */

#define ZDT_TX_TIMEOUT_MS    100U    /* 发送超时 */
#define ZDT_RX_TIMEOUT_MS    20U     /* 等回复的超时：电机在线时 1ms 内就回来了 */
#define ZDT_FRAME_MAX        20U     /* 最长的一帧（梯形位置模式 16 字节） */

/* 如果电机被设成"不返回信息"（应答方式 = 00/None），把它改成 0 */
#ifndef ZDT_WAIT_ACK
#define ZDT_WAIT_ACK         1
#endif

/*
 * 固件类型开关：
 *   0 = X 固件（X42S 出厂默认，力矩-速度-位置三闭环，手册推荐）
 *   1 = Emm5.0 固件（电压模式力矩闭环 + 位置闭环，常见于 Emm42 驱动板）
 *
 * 两种固件只有"速度模式"和"位置模式"的命令格式不同，其余命令（使能、
 * 急停、同步、回零、读参数）完全一样。
 *   X 固件  ：速度单位 0.1RPM、位置单位 0.1°、加速度单位 RPM/s
 *   Emm 固件：速度单位 1RPM 、位置单位 脉冲、加速度 0~255 档
 *
 * 不确定的话先用 0；如果电机对速度命令回 0xE2/0xEE（参数或格式错误），
 * 就把这里改成 1 再试。
 */
/*
 * 本机的 X42S 实测 FWType = FW_Emm（见电机菜单），所以这里用 1。
 * 如果以后把电机切成 X 固件，改回 0 即可。
 */
#ifndef ZDT_FIRMWARE_EMM
#define ZDT_FIRMWARE_EMM     1
#endif

/* ------------------------------ 枚举 ------------------------------ */

/* 运动方向 */
typedef enum {
    ZDT_DIR_CW  = 0,    /* 顺时针 */
    ZDT_DIR_CCW = 1     /* 逆时针 */
} ZDT_Dir_t;

/* 位置模式的参考点 */
typedef enum {
    ZDT_RAF_ABS     = 0,    /* 相对坐标零点做绝对位置运动 */
    ZDT_RAF_REL     = 1,    /* 相对上一目标位置做增量运动 */
    ZDT_RAF_REL_CUR = 2     /* 相对当前实时位置做增量运动 */
} ZDT_Raf_t;

/* 回零模式 */
typedef enum {
    ZDT_ORG_SINGLE_NEAR = 0,    /* 单圈就近回零 */
    ZDT_ORG_SINGLE_DIR  = 1,    /* 单圈方向回零 */
    ZDT_ORG_COLLISION   = 2,    /* 多圈无限位碰撞回零 */
    ZDT_ORG_LIMIT       = 3,    /* 多圈限位开关回零 */
    ZDT_ORG_ABS_ZERO    = 4,    /* 回到绝对位置坐标零点 */
    ZDT_ORG_LAST_POWER  = 5     /* 回到上次掉电位置角度 */
} ZDT_OriginMode_t;

/* 系统状态参数：读命令的"功能码"就是这个值 */
typedef enum {
    ZDT_S_VBUS  = 0x24,    /* 总线电压 mV */
    ZDT_S_CBUS  = 0x26,    /* 总线电流 mA */
    ZDT_S_CPHA  = 0x27,    /* 相电流 mA */
    ZDT_S_TPOS  = 0x33,    /* 目标位置 0.1° */
    ZDT_S_VEL   = 0x35,    /* 实时转速 0.1RPM */
    ZDT_S_CPOS  = 0x36,    /* 实时位置 0.1° */
    ZDT_S_PERR  = 0x37,    /* 位置误差 0.1° */
    ZDT_S_TEMP  = 0x39,    /* 驱动温度 ℃ */
    ZDT_S_FLAG  = 0x3A,    /* 电机状态标志 */
    ZDT_S_OFLAG = 0x3B,    /* 回零状态标志 */
    ZDT_S_OAF   = 0x3C     /* 回零标志 + 电机状态标志 */
} ZDT_SysParam_t;

/* 电机状态标志（ZDT_S_FLAG 返回的那个字节） */
#define ZDT_FLAG_ENS    0x01U   /* bit0 已使能 */
#define ZDT_FLAG_PRF    0x02U   /* bit1 位置到达 */
#define ZDT_FLAG_CGI    0x04U   /* bit2 堵转 */
#define ZDT_FLAG_CGP    0x08U   /* bit3 堵转保护已触发 */
#define ZDT_FLAG_ESI_L  0x10U   /* bit4 左限位 */
#define ZDT_FLAG_ESI_R  0x20U   /* bit5 右限位 */
#define ZDT_FLAG_OAC    0x80U   /* bit7 掉电标志 */

/* 回零状态标志（ZDT_S_OFLAG 返回的那个字节） */
#define ZDT_OFLAG_ENC_RDY   0x01U   /* bit0 编码器就绪 */
#define ZDT_OFLAG_CAL_RDY   0x02U   /* bit1 校准就绪 */
#define ZDT_OFLAG_ORG_SF    0x04U   /* bit2 正在回零 */
#define ZDT_OFLAG_ORG_CF    0x08U   /* bit3 回零失败 */
#define ZDT_OFLAG_OTP_TF    0x10U   /* bit4 过热保护 */
#define ZDT_OFLAG_OCP_TF    0x20U   /* bit5 过流保护 */

/* ---------------------------- 数据结构 ---------------------------- */

/* 电机反馈，由 ZDT_ReadSysParam() 逐项填进来 */
typedef struct {
    int32_t  position_x10;        /* 实时位置 0.1° */
    int32_t  target_pos_x10;      /* 目标位置 0.1° */
    int16_t  speed_x10;           /* 实时转速 0.1RPM */
    uint16_t bus_voltage_mv;      /* 总线电压 mV */
    uint16_t bus_current_ma;      /* 总线电流 mA */
    uint16_t phase_current_ma;    /* 相电流 mA */
    int16_t  temperature_c;       /* 驱动温度 ℃ */
    uint8_t  status_flag;         /* 电机状态标志 */
    uint8_t  origin_flag;         /* 回零状态标志 */
    uint8_t  online;              /* 最近一次通讯成功 = 1 */
} ZDT_Feedback_t;

/* 电机实体 */
typedef struct {
    UART_HandleTypeDef *huart;    /* 接电机的串口（本项目里是 USART2） */
    uint8_t  addr;                /* 电机地址 1~255 */
    uint8_t  enabled;             /* 使能状态 */
    ZDT_Feedback_t fb;            /* 反馈数据 */
} ZDT_Motor_t;

/* ------------------------------- API ------------------------------ */

/* 初始化：绑定串口和地址（不发送任何命令） */
void ZDT_Create(ZDT_Motor_t *motor, UART_HandleTypeDef *huart, uint8_t addr);

/* --- 触发类 --- */
bool ZDT_EnControl(ZDT_Motor_t *motor, bool enable, bool sync);
bool ZDT_ClearPosition(ZDT_Motor_t *motor);                    /* 当前位置清零 */
bool ZDT_OriginSetZero(ZDT_Motor_t *motor, bool store);        /* 把当前位置设为回零零点 */
bool ZDT_OriginTriggerReturn(ZDT_Motor_t *motor, ZDT_OriginMode_t mode, bool sync);
bool ZDT_OriginInterrupt(ZDT_Motor_t *motor);                  /* 强制退出回零 */

/* --- 运动控制 --- */
bool ZDT_VelControl(ZDT_Motor_t *motor, ZDT_Dir_t dir,
                    uint16_t acc_rpm_s, uint16_t speed_x10, bool sync);
bool ZDT_BypassPosControl(ZDT_Motor_t *motor, ZDT_Dir_t dir, uint16_t speed_x10,
                          uint32_t pos_x10, ZDT_Raf_t raf, bool sync);
bool ZDT_TrajPosControl(ZDT_Motor_t *motor, ZDT_Dir_t dir,
                        uint16_t acc_rpm_s, uint16_t dec_rpm_s, uint16_t speed_x10,
                        uint32_t pos_x10, ZDT_Raf_t raf, bool sync);
bool ZDT_StopNow(ZDT_Motor_t *motor, bool sync);
bool ZDT_SyncMotion(ZDT_Motor_t *motor);   /* 触发已缓存命令的电机同步启动 */

/* --- 读取系统参数（结果写进 motor->fb） --- */
bool ZDT_ReadSysParam(ZDT_Motor_t *motor, ZDT_SysParam_t param);

/* --- 上电自检用 --- */
/*
 * 广播读 ID，并把收到的**原始字节**原样带回来：
 *   发送 00 15 6B，正常回复是 [Addr][15][电机地址][6B]。
 *   用广播地址发，不管电机的地址被设成几都会回复。
 * 返回值 = 实际收到的字节数（0 = 一个字节都没收到）。
 * 排查时把 raw 里的内容按十六进制打出来，就能知道串口线上到底有没有东西。
 */
uint16_t ZDT_ReadIdRaw(ZDT_Motor_t *motor, uint8_t *raw, uint16_t max_len);

/*
 * 通用"原始"命令：把一条命令原样发出去，再把收到的字节原样带回来，
 * **不检查格式**（收到几个字节就是几个字节）。返回实际收到的字节数。
 * 专门用来排查"电机到底回了什么、回了几字节"。
 */
uint16_t ZDT_RawCommand(ZDT_Motor_t *motor, uint8_t addr, uint8_t code,
                        const uint8_t *data, uint8_t data_len,
                        uint8_t *raw, uint16_t max_len);

/*
 * 读选项参数状态（发送 Addr 1A 6B），返回两个字节：
 *   flags1: bit0 电机类型 0=1.8° 1=0.9°
 *           bit1 **固件类型 0=X固件 1=Emm固件**  <- 判断该用哪套命令格式
 *           bit2 控制模式 0=开环 1=闭环
 *           bit4 运动方向 0=CW 1=CCW
 *           bit5 按键锁定
 *   flags2: bit0-1 锁定参数等级
 */
bool ZDT_ReadOptionFlags(ZDT_Motor_t *motor, uint8_t *flags1, uint8_t *flags2);

/*
 * USART2 自环自检：
 * 把 PA2 和 PA3 用一根杜邦线短接后重新上电，发送 55 AA 5A A5 并原样收回来。
 * 收回来 = 单片机的串口（引脚复用 / 时钟 / 波特率）完全正常，
 *          问题一定在电机那一侧（接线、共地、地址、波特率、校验方式）。
 */
bool ZDT_LoopbackTest(ZDT_Motor_t *motor);

#endif /* __BSP_ZDT_MOTOR_H */
