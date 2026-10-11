# CANable 接入 Linux 上位机

把 CANable（USB 转 CAN 适配器）接到双节点总线上，Linux 侧就能用
`candump` / `candump -tz` / 本仓库的 `can_monitor`、`can_send` 直接读写报文，
完成"上位机 → CAN → 节点 → 反馈回上位机"的整条闭环。

## 1. 硬件接线

```
   F407 (VCU)            F103 (执行器)
  CANH ──┬──────────────┬── CANH
  CANL ──┴──────────────┴── CANL
         │              │
       120Ω           120Ω          ← 两端各一个终端电阻
         │              │
         └──── CANable ─┘
              CANH / CANL / GND
```

要点：

- CANable 的 **CANH 接总线 CANH，CANL 接总线 CANL**，不要交叉；
- **GND 可以不接**（CAN 是差分信号），但接上更稳，尤其是两块板分别供电时；
- 总线两端各一个 **120Ω** 终端电阻，所以在总线任意两点之间量 CANH-CANL 应该是 **约 60Ω**；
- 波特率必须和固件一致：本项目是 **500 kbps**（两边都是）。

## 2. 识别你的 CANable 是哪一代

插上 USB 后，`ip link` 或者 `dmesg` 会告诉你：

```bash
lsusb
ip -br link show
ls /dev/ttyACM*
```

| 现象 | 型号 / 固件 | 用哪种方式驱动 |
| --- | --- | --- |
| `ip link` 里直接多出一个 `can0` | CANable 2.0 / candlelight（gs_usb） | **内核原生驱动，最省事** |
| 只多出 `/dev/ttyACM0` | CANable 1.0 / slcan 固件 | 需要 `slcand` 起一个虚拟 CAN 口 |

## 3. 拉起接口

### 3.1 gs_usb（candlelight，推荐）

```bash
sudo modprobe gs_usb
ip -br link show                 # 应该能看到 can0
sudo ip link set can0 up type can bitrate 500000
ip -details link show can0       # 确认 state UP、bitrate 500000
```

### 3.2 slcan（老版固件）

```bash
sudo modprobe slcan
# -s6 就是 500kbps（s0=10k s1=20k s2=50k s3=100k s4=125k s5=250k s6=500k s7=800k s8=1M）
sudo slcand -o -c -s6 -S 3000000 /dev/ttyACM0 can0
sudo ip link set can0 up
ip -details link show can0
```

想关掉：

```bash
sudo ip link set can0 down
sudo pkill slcand
```

## 4. 先确认总线上有东西

```bash
sudo apt install -y can-utils      # 装一次就行

candump -tz can0
```

正常情况下应该看到两块板子的数据在刷：

```
(2026-10-11 10:00:00.123456)  can0  RX - -  100   [8]  02 7B 00 92 09 00 05 05
(2026-10-11 10:00:00.143456)  can0  RX - -  100   [8]  02 7B 00 92 09 01 05 2F
(2026-10-11 10:00:00.153456)  can0  RX - -  101   [8]  02 F4 01 7D 00 55 07 D4
(2026-10-11 10:00:00.173456)  can0  RX - -  200   [8]  02 2C 01 00 00 00 09 34
```

判读：

- `100` 大约每 **20ms** 一帧，`101` 大约每 **50ms** 一帧，`200` 大约每 **20ms** 一帧；
- **完全没有输出** = 总线不通（接线、波特率、终端电阻）；
- 有输出但频率忽快忽慢 = 某一侧在重传，检查终端电阻和线长。

## 5. 用本仓库的上位机工具

```bash
cmake -S host -B build/host
cmake --build build/host -j
```

**监视 + 记 CSV**：

```bash
./build/host/can_monitor can0 logs
```

输出长这样（CRC 和滚动计数器都会自动校验）：

```
[10:00:00.123] VCU_Status  mode=RUN speed=12.3 km/h voltage=24.50 V ...
[10:00:00.153] Actuator_Status mode=RUN output=30.0 % current=0.50 A ...
[10:00:00.173] Control_Cmd   mode=RUN target=30.0 % counter=9/OK ...
```

CSV 会写到 `logs/vcu_status.csv`、`logs/actuator_status.csv`、`logs/control_cmd.csv`。

**下发控制命令**：

```bash
# 用法: can_send <接口> <模式 0=OFF 1=STANDBY 2=RUN 3=FAULT> <目标输出%> [帧数]
./build/host/can_send can0 2 30 20     # RUN 模式，目标输出 30%，连发 20 帧
./build/host/can_send can0 1 0 10      # 切回 STANDBY
```

注意：VCU（F407）自己也在 20ms 周期地发 `0x200`，所以真实系统里你会看到
`0x200` 有**两个来源**（VCU 和上位机）。做上位机接管演示时，
可以先让 VCU 停机，或者只看 `can_monitor` 打印的 counter 区分。

## 6. 一键端到端验收

接好线、拉好接口之后，直接跑：

```bash
./scripts/acceptance_test.sh can0 30
```

脚本会自动：

1. 检查 `can0` 是否已 up；
2. 启动 `can_monitor` 抓包；
3. 确认总线上有 `0x100` 和 `0x101`；
4. 发一条 `0x200`（RUN / 30%），确认 `0x101` 的 `output` 跟着变成 30.0%；
5. 统计 CRC 错误帧、未知帧；
6. 打印 PASS / FAIL 汇总。

## 7. 常见问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `ip link` 里没有 `can0` | 固件不是 candlelight，或者驱动没加载 | `sudo modprobe gs_usb`；老固件用 slcand 方案 |
| `RTNETLINK answers: Device or resource busy` | 接口已经 up 了 | 先 `sudo ip link set can0 down` |
| `candump` 一条都没有 | 接线 / 波特率 / 终端电阻 | 量 CANH-CANL 是不是 60Ω；确认两边都是 500k |
| 大量 `CAN error frame` | 波特率不一致、终端电阻缺失、线太长 | 同上，优先查终端电阻 |
| 只有一侧能发 | 收发器方向接反，或者 STB/S 脚没接地 | 见固件侧的接线说明 |
| `ip -details link show` 一直是 `state DOWN` | 接口没 up，或者 USB 掉线 | 重新 `ip link set can0 up ...` |
| 发送报 `No buffer space available` | 总线没有应答（没有第二个节点） | 至少要有一个节点在总线上，CAN 需要 ACK |
