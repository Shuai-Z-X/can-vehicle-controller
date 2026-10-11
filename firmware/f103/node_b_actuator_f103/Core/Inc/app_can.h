#ifndef APP_CAN_H
#define APP_CAN_H

#include <stdint.h>

void AppCan_Init(void);
void AppCan_TaskCanTx(void *argument);
void AppCan_TaskCanRx(void *argument);

/* ---- 给 OLED 显示任务读取的参数（每隔几十毫秒更新一次） ---- */
uint8_t  AppCan_GetActuatorMode(void);
uint16_t AppCan_GetActuatorOutputX10(void);
uint8_t  AppCan_GetActuatorFault(void);
uint16_t AppCan_GetActuatorCurrentX100(void);
int16_t  AppCan_GetActuatorTempC(void);

/* ---- 和电机任务之间的接口 ---- */
/* 取 CAN 下发的目标输出（0.1%），给电机任务换算转速用 */
uint16_t AppCan_GetCmdOutputX10(void);

/*
 * 电机任务把真实反馈回填进来，会覆盖原来那组模拟值：
 *   online         : 电机是否在线（0 表示退回模拟值）
 *   output_pct_x10 : 实际输出百分比 ×10，由实测转速换算
 *   current_a_x100 : 总线电流 ×100（0.01A）
 *   temp_c         : 驱动温度 ℃
 */
void AppCan_SetMotorFeedback(uint8_t online, uint16_t output_pct_x10,
                             uint16_t current_a_x100, int16_t temp_c);

#endif
