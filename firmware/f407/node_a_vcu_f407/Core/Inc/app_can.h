#ifndef APP_CAN_H
#define APP_CAN_H

void AppCan_Init(void);
void AppCan_TaskCanTx(void *argument);
void AppCan_TaskCanRx(void *argument);

#endif