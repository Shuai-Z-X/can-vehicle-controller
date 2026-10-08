#ifndef APP_CAN_SELFTEST_H
#define APP_CAN_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FreeRTOS CAN 自检任务。
 *
 * 进入任务后：
 *   1. 检查或强制 CAN 进入 Loopback 模式；
 *   2. 配置接收过滤器；
 *   3. 启动 CAN；
 *   4. 每秒发送一帧 0x200，并从回环 FIFO 读回；
 *   5. 校验 ID、DLC 和数据，通过串口输出 CAN loopback OK / FAIL。
 */
void CanSelfTest_Task(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_SELFTEST_H */
