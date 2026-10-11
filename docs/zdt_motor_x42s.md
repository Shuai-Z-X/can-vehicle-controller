# 张大头 ZDT X42S 闭环步进电机驱动

执行器节点（Node B / STM32F103ZET6）通过 **USART2** 用串口协议控制 X42S，
把 VCU 下发的 `0x200 Control_Cmd` 换算成电机转速，
再把电机的真实转速 / 电流 / 温度回填到 `0x101 Actuator_Status`。

## 1. 接线

| 电机侧引脚 | STM32F103ZET6 | 说明 |
| --- | --- | --- |
| V+ | 12~24V 电源正 | 推荐 24V 3A 以上 |
| Gnd | 电源负 **和** 单片机 GND | **必须共地**，否则串口收不到 |
| R / T（TTL 串口） | T→PA3(RX)、R→PA2(TX) | 交叉连接：单片机的 TX 接电机的 RX |
| A / B（RS485） | 不接 | 本项目不用 485 |
| H / L（CAN） | 不接 | 本项目用串口，不用电机自带 CAN |

注意：

- 电机的通信引脚是复用的（TTL / RS485 / CAN 共用几个脚），
  具体哪两个是 TTL 收发脚要看电机外壳上的丝印或说明书中的接口图。
- **单片机 TX(PA2) → 电机 R**，**单片机 RX(PA3) ← 电机 T**。
  接反了收不到任何回复，driver 里会一直报 `online = 0`。
- 电机是独立供电的，不要用单片机的 3.3V 去带电机。

## 2. 上电前要确认电机侧的四个参数

用张大头官方的上位机（或按键菜单）确认这几项，出厂默认值通常就是对的：

| 参数 | 应该是 | 不对怎么办 |
| --- | --- | --- |
| 通讯地址 | **1** | 改电机地址，或改 `app_motor.h` 里的 `APP_MOTOR_ADDR` |
| 波特率 | **115200** | 改电机波特率，或改 `usart.c` 里 `huart2.Init.BaudRate` |
| 应答方式 | `01(Receive)` 或 `03(Both)` | 设成 `00(None)` 时把 `bsp_zdt_motor.h` 里 `ZDT_WAIT_ACK` 改成 0 |
| 固件类型 | **X 固件** | 若是 Emm5.0 固件，把 `ZDT_FIRMWARE_EMM` 改成 1 |

固件判断方法：发一条速度命令，如果电机回 `0xE2` 或 `0xEE`（参数错误 / 格式错误），
说明命令格式对不上，换成另一种固件配置再试。

## 3. 协议速查

帧格式（默认 115200 8N1）：

```
主机发送: [地址][功能码][数据...][0x6B]
电机返回: [地址][功能码][返回数据][0x6B]
```

**多字节参数一律大端序**（高字节在前）。驱动里实现的主要命令：

| 功能 | 功能码 | 帧（X 固件） |
| --- | --- | --- |
| 使能/松轴 | `F3` | `Addr F3 AB 状态 同步 6B` |
| 速度模式 | `F6` | `Addr F6 方向 加速度(2B) 速度(2B) 同步 6B` |
| 直通限速位置 | `FB` | `Addr FB 方向 速度(2B) 位置(4B) 运动模式 同步 6B` |
| 梯形位置 | `FD` | `Addr FD 方向 加速度(2B) 减速度(2B) 最大速度(2B) 位置(4B) 运动模式 同步 6B` |
| 立即停止 | `FE` | `Addr FE 98 同步 6B` |
| 触发多机同步 | `FF` | `Addr FF 66 6B` |
| 位置清零 | `0A` | `Addr 0A 6D 6B` |
| 设回零零点 | `93` | `Addr 93 88 是否存储 6B` |
| 触发回零 | `9A` | `Addr 9A 回零模式 同步 6B` |
| 强制退出回零 | `9C` | `Addr 9C 48 6B` |
| 读实时位置 | `36` | `Addr 36 6B` → `Addr 36 符号 位置(4B) 6B` |
| 读实时转速 | `35` | `Addr 35 6B` → `Addr 35 符号 速度(2B) 6B` |
| 读驱动温度 | `39` | `Addr 39 6B` → `Addr 39 符号 温度 6B` |
| 读总线电流 | `26` | `Addr 26 6B` → `Addr 26 电流(2B) 6B` |
| 读电机状态 | `3A` | `Addr 3A 6B` → `Addr 3A 状态标志 6B` |

单位（X 固件）：位置 `0.1°`、速度 `0.1RPM`、加速度 `RPM/s`、温度 `℃`、电流 `mA`。
Emm 固件的速度单位是 `1RPM`、位置单位是 `脉冲数`（1.8° 电机 16 细分时 3200 脉冲/圈）。

## 4. 代码结构

| 文件 | 作用 |
| --- | --- |
| `Drivers/BSP/ZDT/bsp_zdt_motor.h/.c` | 协议层：组帧、收发、解析，不含任何业务逻辑 |
| `Core/Src/app_motor.c` + `Core/Inc/app_motor.h` | 应用层：跑 FreeRTOS 任务，把 CAN 命令翻译成电机指令，读回反馈 |
| `Core/Src/usart.c` | 新增 USART2（PA2/PA3，115200 8N1）专门接电机 |
| `Core/Src/app_can.c` | 新增 `AppCan_GetCmdOutputX10()` 和 `AppCan_SetMotorFeedback()` |

数据流：

```
VCU 的 0x200 Control_Cmd（目标输出百分比）
        │
        ▼
   motorTask 换算成转速 ──► 0xF6 速度命令 ──► X42S 转动
        │                                        │
        │                                  0x36/0x35/0x26/0x39 读回
        ▼                                        │
  0x101 Actuator_Status ◄─────────── 真实转速/电流/温度
        │
        ├──► OLED 显示
        └──► Linux 上位机 (SocketCAN)
```

## 5. 速度映射

```
目标输出百分比 0~100.0%（CAN 里的 0~1000）
        × 3
        ▾
电机转速 0~300.0 RPM（协议里的 0~3000，单位 0.1RPM）
```

改映射只动三个宏（在 `app_motor.h`）：

- `APP_MOTOR_SPEED_GAIN`：百分比 → 转速的倍数
- `APP_MOTOR_MAX_SPEED_X10`：转速封顶
- `APP_MOTOR_ACC_RPM_S`：加速度

## 6. 用位置模式 / 回零

驱动里已经把这些 API 都做好了，要用的时候直接在 `app_motor.c` 里调：

```c
/* 走绝对位置：转到 90.0°（90.0 × 10 = 900），最大速度 120.0RPM，
 * 加减速 500RPM/s。ZDT_RAF_ABS 表示"相对坐标零点"的绝对位置。 */
ZDT_TrajPosControl(&g_motor, ZDT_DIR_CW, 500U, 500U, 1200U,
                   (uint32_t)(90.0f * 10.0f), ZDT_RAF_ABS, false);

/* 相对当前位置再转 45.0° */
ZDT_TrajPosControl(&g_motor, ZDT_DIR_CW, 500U, 500U, 1200U,
                   450U, ZDT_RAF_REL_CUR, false);

/* 碰撞回零（不需要限位开关，靠堵转检测找零点） */
ZDT_OriginTriggerReturn(&g_motor, ZDT_ORG_COLLISION, false);

/* 把当前位置直接设为零点（不动，只定义零点） */
ZDT_OriginSetZero(&g_motor, true);
```

想等电机到位，可以轮询状态标志里的 `ZDT_FLAG_PRF`（bit1 = 到位）：

```c
do {
    ZDT_ReadSysParam(&g_motor, ZDT_S_FLAG);
    osDelay(10);
} while ((g_motor.fb.status_flag & ZDT_FLAG_PRF) == 0U);
```

## 7. 常见问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 电机完全不动，`online = 0` | 没共地 / TX RX 接反 / 地址不对 / 波特率不对 | 从这四项依次查 |
| 电机不动，但 `online = 1` | 命令被拒绝 | 串口打印收到的是 `0xE2` 还是 `0xEE`，多半是固件类型选错 |
| 电机方向反了 | 机械定义不同 | 把 `ZDT_VelControl` 里的 `ZDT_DIR_CW` 换成 `ZDT_DIR_CCW` |
| 一转就报堵转 | 加速度太大或电流不够 | 调小 `APP_MOTOR_ACC_RPM_S`，或加大闭环最大电流 |
| OLED 上 `OUT` 一直是 0.0% | 电机没在线，回填的是模拟值 | 确认 `online = 1` 之后 OUT 才会跟着实测转速走 |
