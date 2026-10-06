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

## Notes

The host code is Linux-only because it uses SocketCAN. Build it inside the
Ubuntu VM or another Linux environment, not on Windows.
