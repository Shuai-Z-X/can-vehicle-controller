# 没有 CANable？把 F407 变成一块自制 CAN 适配器

做 Linux 上位机联调时，通常需要一块 CANable / PCAN 之类的 USB 转 CAN。
但**这个项目不一定需要买**：F407 本来就有一路空闲串口，让它兼职当适配器，
Linux 侧用内核自带的 `slcan` 驱动挂成标准 `can0`，
`candump` / `can_monitor` / `can_send` / `scripts/acceptance_test.sh` **全部原样可用**。

## 1. 原理

```
   F103 执行器 ── CAN ── F407 (VCU + SLCAN 网关) ── USB转TTL ── Linux (can0)
                                                          │
                                                    slcand / can-utils
```

F407 里实现的是一套 **SLCAN（LAWICEL）ASCII 协议**——就是 CANable 1.0
用的那套。Linux 内核有一个 `slcan` 线路规程专门会说这个协议，
所以对内核来说，你的 F407 和一块真的 CANable 没有区别。

协议本身很简单，所有命令以 `\r` 结尾：

| 方向 | 内容 | 含义 |
| --- | --- | --- |
| 主机→适配器 | `S6\r` | 设 CAN 波特率（S6 = 500k） |
| 主机→适配器 | `O\r` / `C\r` | 打开 / 关闭 CAN 通道 |
| 主机→适配器 | `V\r` / `N\r` / `F\r` | 读版本 / 序列号 / 状态 |
| 主机→适配器 | `t III D DDDD...\r` | 发送标准帧（III=11 位 ID，D=DLC） |
| 适配器→主机 | `\r` / `\a` | 命令成功 / 出错 |
| 适配器→主机 | `t III D DDDD...\r` | 收到一帧 CAN 就主动上报 |

**一个细节**：CAN 节点收不到自己发出去的报文。所以网关除了转发
"从总线上收到的帧"，还会通过 `AppCan_SetTxHook()` 把 VCU **自己发出去的**
`0x100` / `0x200` 也转发给上位机，这样上位机能看到完整的三个报文。

## 2. 接线

```
        F407                          USB 转 TTL                    Linux
   ┌────────────┐                  ┌──────────────┐            ┌──────────┐
   │  PA2 (TX)  ├──────────────────┤ RX           │            │          │
   │  PA3 (RX)  ├──────────────────┤ TX           │── USB ─────┤ /dev/ttyUSB0
   │  GND       ├──────────────────┤ GND          │            │          │
   │ PA11/PA12 ─┼── CAN 总线        └──────────────┘            └──────────┘
   └────────────┘
```

- PA2/PA3 是 F407 的 **USART2**，波特率固定 **115200 8N1**；
- **必须共地**（USB 转 TTL 的 GND 接 F407 的 GND）；
- PA9/PA10（USART1）仍然是调试串口，接另一个 USB 转 TTL 看打印，两者互不影响。

## 3. 烧录

直接编译下载 F407 工程即可，`app_slcan.c` 已经加进 Keil 工程，
上电后 `slcanTask` 会和 CAN、OLED 任务一起跑。

## 4. Linux 侧操作

```bash
# 1. 认一下是哪个串口
ls /dev/ttyUSB* /dev/ttyACM*
dmesg | tail -5

# 2. 用 slcand 把 F407 挂成一个 can0 接口
sudo modprobe slcan
sudo slcand -o -c -s6 -S 115200 /dev/ttyUSB0 can0

# 3. 打开接口
sudo ip link set can0 up
ip -details link show can0

# 4. 看数据
candump -tz can0
```

- `-s6` 是 **CAN 波特率索引**（s6 = 500 kbps，和固件一致）；
- `-S 115200` 是 **串口波特率**，和 F407 的 USART2 一致；
- 关掉的时候：`sudo ip link set can0 down` 然后 `sudo pkill slcand`。

正常应该看到三路报文在刷：

```
can0  100   [8]  02 7B 00 92 09 00 05 05
can0  101   [8]  02 F4 01 7D 00 55 07 D4
can0  200   [8]  02 2C 01 00 00 00 09 34
```

## 5. 然后就能用现成的工具了

```bash
sudo apt install -y can-utils

# 监视 + 记 CSV
./build/host/can_monitor can0 logs

# 下发控制命令（RUN 模式，目标输出 30%）
./build/host/can_send can0 2 30 20

# 一键端到端验收
sudo ./scripts/acceptance_test.sh can0 30
```

## 6. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `slcand: tcsetattr: Invalid argument` | 串口设备名不对 | 用 `ls /dev/ttyUSB*` 确认，改成实际的设备 |
| `slcand` 起来了但 `candump` 没数据 | 没执行 `ip link set can0 up` | 补上；`ip -details link show can0` 看 state |
| `ip link` 里没有 can0 | slcand 没起成功 | 先 `sudo pkill slcand`，再按上面重来 |
| `candump` 全是 error frame | CAN 侧接线/终端电阻 | 这跟网关无关，按 `docs/canable_linux.md` 查总线 |
| 只有 0x101，没有 0x100/0x200 | Tx hook 没生效 | 确认 `app_slcan.c` 里 `AppCan_SetTxHook()` 已调用 |
| 数据时有时无 | USB 转 TTL 接触不良 | 换线、插紧；和之前电机那种"偶发不通"是同一类问题 |
