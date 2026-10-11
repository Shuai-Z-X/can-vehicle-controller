# can-vehicle-controller

Automotive controller prototype: STM32 + FreeRTOS + CAN + mini-UDS + Linux
SocketCAN host.

## Repository layout

```text
docs/          Protocol documents for humans
protocols/     CAN matrix, DID/DTC tables, golden test vectors
tools/         Validation and code-generation helpers
host/          Linux C++17 SocketCAN monitor and sender
scripts/       Helper scripts for vcan, test traffic, and test runs
```

## Day 2 validation

Create a Python virtual environment and install `cantools`:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple cantools
python tools/check_can_db.py
```

Expected result: `all 4 vectors passed`.

## Day 3: bring up vcan0

```bash
chmod +x scripts/setup_vcan.sh scripts/test_vcan.sh
./scripts/setup_vcan.sh
ip -details link show vcan0
```

In one terminal, listen:

```bash
candump vcan0
```

In another terminal, send the four golden test vectors:

```bash
./scripts/test_vcan.sh vcan0
```

## Day 3/4: build the Linux host

```bash
cmake -S host -B build/host
cmake --build build/host -j
./build/host/can_monitor vcan0 logs
```

Then run `./scripts/test_vcan.sh vcan0` from another terminal. The monitor
decodes `VCU_Status`, `Actuator_Status`, and `Control_Cmd`, checks the CRC-8
and rolling counter, prints human-readable values, and writes CSV logs under
`logs/`.

Expected log files:

```text
logs/vcu_status.csv
logs/actuator_status.csv
logs/control_cmd.csv
```

## Day 5: send Control_Cmd and run tests

Build the sender and monitor:

```bash
cmake -S host -B build/host
cmake --build build/host -j
```

Start the monitor in one terminal:

```bash
./scripts/setup_vcan.sh
./build/host/can_monitor vcan0 logs
```

Send five Control_Cmd frames in another terminal:

```bash
./build/host/can_send vcan0 2 30 5
```

Install Python test dependencies and run the test suite:

```bash
source .venv/bin/activate
python -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple pytest cantools python-can
python -m pytest host/tests -v
```

Or run the one-command helper:

```bash
chmod +x scripts/run_tests.sh
./scripts/run_tests.sh
```

The integration test needs `vcan0` and a built `can_monitor`; otherwise it is
skipped automatically.

## Day 8: CANable + 真实总线端到端验收

前面几步都是在 `vcan0` 上做的纯软件验证。到了这一步，把 **CANable**
接到两块板子的真实 CAN 总线上，就可以跑完整的端到端验收。

> **没有 CANable 也不用买**：F407 已经把 USART2 实现成 SLCAN 适配器，
> 用一根 USB 转 TTL 就能让 Linux 挂出标准 `can0`，操作步骤见
> [`docs/slcan_gateway.md`](docs/slcan_gateway.md)。

硬件接线、驱动加载（gs_usb / slcan）、波特率配置和排错方法都在
[`docs/canable_linux.md`](docs/canable_linux.md) 里，这里只列最短路径：

```bash
# 方案 A：有 CANable
# 1. 拉起接口（CANable 2.0 / candlelight 固件）
sudo modprobe gs_usb
sudo ip link set can0 up type can bitrate 500000

# 方案 B：没有 CANable，用 F407 自制适配器（USB 转 TTL 接 PA2/PA3）
sudo modprobe slcan
sudo slcand -o -c -s6 -S 115200 /dev/ttyUSB0 can0
sudo ip link set can0 up

# 两种方案到这里就一样了
ip -details link show can0

# 2. 确认总线上有数据（0x100 每 20ms、0x101 每 50ms）
candump -tz can0

# 3. 一键端到端验收（PASS/FAIL 汇总）
chmod +x scripts/acceptance_test.sh
sudo ./scripts/acceptance_test.sh can0 30
```

验收脚本会自动检查这几件事：

| 检查项 | 说明 |
| --- | --- |
| 接口存在且 UP | `ip link` + `state UP` |
| 收到 0x100 / 0x101 / 0x200 | 三个报文都在总线上，说明两个节点都在正常工作 |
| VCU 处于 RUN | F407 收得到执行器的 0x101 才会进 RUN，等价于验了反向链路 |
| 执行器输出跟随命令 | 0x101 的 `output` 应该等于 0x200 的目标值（默认 30.0%） |
| 上位机 can_send 可发帧 | 证明 Linux 侧能写入总线 |
| 无 CRC 错误帧 / 错误帧 | 总线电气质量 |

抓到的原始报文会存到 `logs/acceptance/` 下的 CSV，可以直接拿来做报告截图。

## Notes

The host code is Linux-only because it uses SocketCAN. Build it inside the
Ubuntu VM or another Linux environment, not on Windows.
