# OLED 参数显示（Node A / Node B）

## 1. 功能说明

两块板子各接一块 0.96 寸 OLED（128x64，SSD1306 控制器，4 针 I2C 模块），
由 FreeRTOS 里一个独立的 `displayTask` 显示当前参数。

显示任务优先级最低（`osPriorityLow`），并且**只有参数发生变化时才重画**，
所以不会影响 20ms / 50ms 的 CAN 收发周期。

## 2. 接线

两块板子用的是同一组引脚，接线方式完全一样。

| OLED 引脚 | STM32F407VET6（Node A / VCU） | STM32F103ZET6（Node B / 执行器） |
| --- | --- | --- |
| VCC | 3.3V | 3.3V |
| GND | GND | GND |
| SCL | PB6 | PB6 |
| SDA | PB7 | PB7 |

注意事项：

- 一定接 **3.3V**，不要接 5V，大多数这种模块虽然是 5V 兼容的，但接 3.3V 最安全。
- 模块上的 4 个针脚顺序不统一，有的板子是 `GND VCC SCL SDA`，接线前先看丝印。
- 驱动是**软件 I2C**（GPIO 模拟时序），不占用 STM32 的硬件 I2C 外设。

## 3. 两块屏各显示什么

Node A（VCU / STM32F407）：

```
VCU  STM32F407
MD:RUN     F:00
SPD:12.3km/h
BAT:24.50V
```

- 第 2 行 `MD:` 是工作模式（OFF / STANDBY / RUN / FAULT），`F:` 是故障标志位。
  `F:01` 就表示 bit0 的 CAN 通讯超时故障被置起来了。
- 第 3 行是车速，第 4 行是电池电压。

Node B（执行器 / STM32F103）：

```
ACT STM32F103
MD:RUN     F:00
OUT:30.0%
T:45C I:0.50A
```

- 第 3 行是实际输出百分比，进入 FAULT 时会立刻变成 `OUT:0.0%`。
- 第 4 行是温度和电流。

## 4. 代码结构

| 文件 | 作用 |
| --- | --- |
| `firmware/f103/.../Drivers/BSP/OLED/oled.c` | 软件 I2C 驱动 + SSD1306 初始化命令 |
| `firmware/f407/.../Drivers/BSP/OLED/oled.c` | 同上，两份文件内容一致（引脚都是 PB6/PB7） |
| `Core/Src/freertos.c` 里的 `StartDisplayTask` | 每 200ms 读一次参数，变化才刷新 |
| `Core/Src/app_can.c` 里的 `AppCan_GetXxx()` | 把 CAN 任务里的参数安全地暴露给显示任务 |

参数是跨任务读取的，`app_can.c` 里所有共享变量都加了 `volatile`，
显示任务通过 `AppCan_Get...()` 读取，不需要额外的锁。

## 5. 想改成显示别的内容

在 `StartDisplayTask` 里，每一行对应一次 `OLED_ShowString` / `OLED_ShowNum`：

```c
OLED_ShowString(3, 1, "SPD:");          /* 第 3 行第 1 列开始写文字 */
OLED_ShowNum(3, 5, speed / 10U, 2);     /* 第 3 行第 5 列写 2 位整数 */
OLED_ShowChar(3, 7, '.');               /* 第 3 行第 7 列写小数点 */
```

- 第一个参数是**行**（1~4），第二个是**列**（1~16），一行最多 16 个字符。
- 写数字用的是「放大 10 倍 / 100 倍」的整数，比如 `speed = 123` 表示 12.3，
  所以显示时要写成 `speed / 10` 和 `speed % 10`。
- 模式那一列固定占 7 个字符宽，这样从 `STANDBY` 变成 `RUN` 不会在屏上留下残字。

## 6. 不亮怎么办

按顺序排查：

1. **整屏全黑**：先量 VCC 和 GND，再看 SCL/SDA 有没有接反。
2. **有供电但不亮**：确认模块 I2C 地址是 `0x78`（驱动里写死的）。
   部分模块背面电阻改过以后地址会变成 `0x7A`，这时把 `oled.c` 里的
   `0x78` 全部改成 `0x7A`。
3. **确认模块本身是好的**：把这块 OLED 拔下来插到另一块板子上试。
   两块板用的引脚（PB6/PB7）和代码完全一样，如果换板就能亮，
   说明模块没问题，要去查不亮那块板的排线；如果换板还是不亮，
   说明是模块或者杜邦线的问题。
4. **软件 I2C 的速度**：这份驱动是纯 GPIO 翻转，没有硬件分频，
   SCL 频率跟着主频走。两块板一个 70MHz 一个 72MHz，不加延时时
   SCL 大约 1.6MHz，已经超出 SSD1306 数据手册的 400kHz 上限。
   驱动里用 `OLED_I2C_DELAY_LOOPS`（默认 8）把它压到 400kHz 以内。
   实测如果模块在别的频率上更稳，可以调这个数字：数字越大 SCL 越慢。
5. **显示很淡**：把 `OLED_Init()` 里的对比度命令 `0xCF` 调大一点试（最大 `0xFF`）。
6. **画面花/闪**：杜邦线太长或者接触不良，软件 I2C 对线长比较敏感，尽量短一些。
7. **两块屏内容一样但一块不动**：说明那块板的 `displayTask` 没跑起来，
   检查 FreeRTOS 堆是否够（`configTOTAL_HEAP_SIZE`）。

## 7. 串口状态行怎么看

两块板子每秒打印一行状态，用来判断到底是程序问题还是总线问题：

```
STATUS: mode=FAULT fault=0x01 tec=0 rec=0 rx=0
```

| 字段 | 含义 | 怎么判断 |
| --- | --- | --- |
| `mode` | 本级节点的工作模式 | `FAULT` 表示本级判定出了问题 |
| `fault` | 本级 FaultFlags | `0x01` = 本级 CAN 超时（收不到对方的帧） |
| `tec` | CAN 发送错误计数 | 一直往上涨 = 总线电气问题 |
| `rec` | CAN 接收错误计数 | 一直往上涨 = 总线电气问题 |
| `rx` | 上电以来收到的对方帧数 | 一直是 0 = 这一侧根本收不到 |

几种典型组合：

- `tec`/`rec` 都是 0，`rx` 在涨：CAN 完全正常。
- `tec`/`rec` 一直涨：总线问题（大概率是终端电阻只有 1 个、
  CANH/CANL 接反、或者两边波特率不一致）。
- `tec`/`rec` 是 0，`rx` 一直是 0：本级收不到。
  如果对方 `tec` 在涨，说明发出去没人应答；如果对方 `tec` 也是 0，
  说明对方根本没在发，或者两边的收发器方向接错了。

另外注意一个设计：VCU 进入 FAULT 以后，会把自己的 `FAULT` 通过
`0x200 Control_Cmd` 一起下发给执行器。所以执行器屏上出现
`MD:FAULT` 有两种可能，看它自己的 `F:` 字段就能区分：

- `MD:FAULT  F:01`：执行器自己超时了，没收到 `0x200`。
- `MD:FAULT  F:00`：执行器收帧正常，是 VCU 告诉它要进故障 ——
  问题在 VCU 那一侧（VCU 收不到 `0x101`）。

## 8. 踩过的坑：CubeMX 里的 CAN 工作模式

做回环自检的时候，CubeMX 里 CAN 的 Mode 会被设成 `Loopback`，
生成出来的 `can.c` 里就是：

```c
hcan1.Init.Mode = CAN_MODE_LOOPBACK;
```

**回环模式下 CAN 外设的接收端不接总线，收发都只在芯片内部绕一圈**，
所以另一块板子什么都收不到。两块板的 `can.c` 都必须是：

```c
hcan1.Init.Mode = CAN_MODE_NORMAL;   /* F103 里句柄叫 hcan */
```

而且 `.ioc` 文件里的 `CAN1.Mode` / `CAN.Mode` 也要一起改成 `CAN_MODE_NORMAL`，
否则下次用 CubeMX 重新生成代码，`can.c` 会被改回 `Loopback`，
问题会莫名其妙地复现。

这个坑的表现和"总线没接好"一模一样：对方收不到、错误计数却是 0。
区分方法就是看 `tec`/`rec`：**两个都是 0 却收不到帧，先去看 CAN 的工作模式**。
