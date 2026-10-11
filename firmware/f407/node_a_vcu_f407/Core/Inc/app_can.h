#ifndef APP_CAN_H
#define APP_CAN_H

#include <stdint.h>
#include <stdbool.h>

void AppCan_Init(void);
void AppCan_TaskCanTx(void *argument);
void AppCan_TaskCanRx(void *argument);

/* ---- 给 OLED 显示任务读取的参数（每隔几十毫秒更新一次） ---- */
uint8_t  AppCan_GetVcuMode(void);
uint8_t  AppCan_GetVcuFaultFlags(void);
uint16_t AppCan_GetVcuSpeedX10(void);
uint16_t AppCan_GetVcuVoltageX100(void);

/* ---- 给 SLCAN 网关用的帧回调 ---- */
/* 一帧 CAN 报文的回调：id + 数据 + 长度 */
typedef void (*AppCan_FrameHook_t)(uint32_t id, const uint8_t *data, uint8_t dlc);

/* 本节点自己发出去的帧（CAN 节点收不到自己的报文，所以要单独回调一次） */
void AppCan_SetTxHook(AppCan_FrameHook_t hook);

/* 从总线上收到的帧 */
void AppCan_SetRxHook(AppCan_FrameHook_t hook);

/* 原始发送（给网关用）：只发，不触发 Tx 回调，避免把上位机自己发的帧又回显回去 */
bool AppCan_SendRaw(uint32_t id, const uint8_t *data, uint8_t dlc);

#endif
