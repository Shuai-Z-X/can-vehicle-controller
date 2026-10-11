#ifndef __APP_SLCAN_H
#define __APP_SLCAN_H

#include <stdint.h>

/* ==========================================================================
 * SLCAN（LAWICEL）协议网关
 *
 * 作用：把 F407 变成一块"自己做的 CANable"。
 *   Linux 侧： sudo slcand -o -c -s6 -S 3000000 /dev/ttyUSB0 can0
 *            sudo ip link set can0 up
 *   然后 candump / can_monitor / can_send 全部照常用。
 *
 * 硬件：USB 转 TTL 接 F407 的 USART2（PA2=TX，PA3=RX），115200 8N1。
 *
 * 协议要点（LAWICEL ASCII，所有命令以 '\r' 结束）：
 *   主机->适配器: S6 设波特率 / O 打开 / C 关闭 / V 版本 / F 状态 / N 序列号
 *                 t III D DDDD...  发标准帧（III=11位ID，D=DLC）
 *   适配器->主机: 收到命令回 '\r'，出错回 '\a'；
 *                 收到 CAN 帧则主动发 t III D DDDD...\r
 * ========================================================================== */

void AppSlcan_Task(void *argument);

/*
 * 转发回调：注册给 app_can，本节点**自己发出**的帧（Tx hook）和
 * 从总线**收到**的帧（Rx hook）都会走这里，再转给上位机。
 *
 * 为什么要转发自己发的帧：CAN 节点收不到自己发出去的报文，
 * 如果不转发，上位机就看不到 0x100/0x200，只能看到对方的 0x101。
 */
void AppSlcan_ForwardFrame(uint32_t id, const uint8_t *data, uint8_t dlc);

#endif /* __APP_SLCAN_H */
