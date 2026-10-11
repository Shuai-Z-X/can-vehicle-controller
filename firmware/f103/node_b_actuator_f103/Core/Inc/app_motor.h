#ifndef __APP_MOTOR_H
#define __APP_MOTOR_H

#include <stdint.h>
#include "bsp_zdt_motor.h"

/* ==========================================================================
 * 执行器节点（Node B）的电机任务
 *
 * 数据流：
 *   VCU 的 0x200 Control_Cmd  ──►  目标输出百分比
 *                                    │
 *                                    ▼
 *                          换算成转速，下发给 X42S
 *                                    │
 *                                    ▼
 *                          读回位置/转速/电流/温度
 *                                    │
 *                                    ▼
 *                 回填到 0x101 Actuator_Status ──► OLED + Linux 上位机
 *
 * 这样"上位机发指令 → 电机真的转 → 真实反馈回到上位机"就闭环了。
 * ========================================================================== */

/* 电机在 USART2 上，出厂默认地址是 1 */
#define APP_MOTOR_ADDR                 1U

/* 转速上限：300.0 RPM（42 步进在台面上够用，也留了安全余量） */
#define APP_MOTOR_MAX_SPEED_X10        3000U

/* 加速度：500 RPM/s */
#define APP_MOTOR_ACC_RPM_S            500U

/* CAN 里的 0.1% 换算成 0.1RPM 的系数：1000(100.0%) × 3 = 3000(300.0RPM) */
#define APP_MOTOR_SPEED_GAIN           3U

/* 任务周期 */
#define APP_MOTOR_PERIOD_MS            50U

void AppMotor_Init(void);
void AppMotor_Task(void *argument);

/* 给调试/显示用的只读指针 */
const ZDT_Motor_t *AppMotor_Get(void);

#endif /* __APP_MOTOR_H */
