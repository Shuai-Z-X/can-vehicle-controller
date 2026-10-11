#!/usr/bin/env bash
#
# 端到端验收脚本（在 Linux 上、接好真实 CAN 总线之后运行）
#
# 它把整条链路自动跑一遍并给出 PASS / FAIL 汇总：
#   上位机/总线  →  F407 VCU  →  CAN  →  F103 执行器  →  状态回传
#
# 用法:
#   sudo ./scripts/acceptance_test.sh [接口名] [期望输出%]
#   sudo ./scripts/acceptance_test.sh can0 30
#
# 为什么要 sudo：SocketCAN 的原始套接字需要 CAP_NET_RAW 权限，
# 不然 can_monitor / can_send 会报 "Operation not permitted"。
#
set -euo pipefail

# 切到仓库根目录，保证相对路径都对。
cd "$(dirname "$0")/.."

IFACE="${1:-can0}"
EXPECT_OUTPUT="${2:-30}"
LOGDIR="logs/acceptance"
MONITOR="build/host/can_monitor"
SENDER="build/host/can_send"
RX_LOG=""

PASS=0
FAIL=0

ok()  { printf '  [PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }
info(){ printf '  [INFO] %s\n' "$1"; }

MON_PID=""
cleanup() {
    if [ -n "$MON_PID" ] && kill -0 "$MON_PID" 2>/dev/null; then
        kill "$MON_PID" 2>/dev/null || true
        wait "$MON_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

printf '=== 端到端验收: 接口=%s 期望输出=%s%% ===\n' "$IFACE" "$EXPECT_OUTPUT"

# ---------------------------------------------------------------- 1. 接口
if ! ip link show "$IFACE" >/dev/null 2>&1; then
    bad "接口 $IFACE 不存在"
    printf '\n提示：先按 docs/canable_linux.md 拉起接口，例如\n'
    printf '      sudo modprobe gs_usb\n'
    printf '      sudo ip link set %s up type can bitrate 500000\n' "$IFACE"
    exit 1
fi
ok "接口 $IFACE 存在"

if ip -details link show "$IFACE" | grep -q 'state UP'; then
    ok "$IFACE 已经 UP"
else
    bad "$IFACE 没有 UP（sudo ip link set $IFACE up ...）"
fi

# ---------------------------------------------------------------- 2. 工具
if [ ! -x "$MONITOR" ] || [ ! -x "$SENDER" ]; then
    info "host 工具还没编译，先编译"
    cmake -S host -B build/host >/dev/null
    cmake --build build/host -j >/dev/null
fi
ok "can_monitor / can_send 已就绪"

# ---------------------------------------------------------------- 3. 抓包
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR"

# 抓包日志和 CSV 一起放在 logs/acceptance/ 里，跑完可以直接拿来写报告。
RX_LOG="$LOGDIR/can_monitor.log"

"$MONITOR" "$IFACE" "$LOGDIR" >"$RX_LOG" 2>&1 &
MON_PID=$!
info "can_monitor 已启动 (pid=$MON_PID)，先抓 3 秒"
sleep 3

if ! kill -0 "$MON_PID" 2>/dev/null; then
    bad "can_monitor 启动后立刻退出了"
    sed 's/^/         /' "$RX_LOG" | head -5
    printf '\n如果提示 Operation not permitted，请用 sudo 运行本脚本。\n'
    exit 1
fi

# ---------------------------------------------------------------- 4. 报文
if grep -q 'VCU_Status' "$RX_LOG"; then
    ok "收到 0x100 VCU_Status"
else
    bad "没有收到 0x100（F407 没在发，或总线不通）"
fi

if grep -q 'Actuator_Status' "$RX_LOG"; then
    ok "收到 0x101 Actuator_Status"
else
    bad "没有收到 0x101（F103 没在发，或总线不通）"
fi

if grep -q 'Control_Cmd' "$RX_LOG"; then
    ok "收到 0x200 Control_Cmd"
else
    bad "没有收到 0x200"
fi

# VCU 应该已经进入 RUN（它收得到执行器的 0x101 才会进 RUN）
if grep -q 'VCU_Status mode=RUN' "$RX_LOG"; then
    ok "VCU 处于 RUN（说明它收到了执行器的 0x101）"
else
    bad "VCU 不是 RUN 状态（F407 收不到 0x101？看它串口的 rx 计数）"
fi

# 执行器应该跟着 0x200 的目标输出走
if grep -q "Actuator_Status mode=RUN output=${EXPECT_OUTPUT}\.0 %" "$RX_LOG"; then
    ok "执行器输出跟随命令 = ${EXPECT_OUTPUT}.0%"
else
    bad "执行器输出没有跟上 ${EXPECT_OUTPUT}.0%（检查 F103 的 STATUS 行）"
fi

# ---------------------------------------------------------------- 5. 上位机发送
if "$SENDER" "$IFACE" 2 "$EXPECT_OUTPUT" 5 >"$RX_LOG.send" 2>&1; then
    ok "上位机 can_send 发送 0x200 成功（$(grep -c 'sent 0x200' "$RX_LOG.send") 帧）"
else
    bad "上位机 can_send 发送失败"
    sed 's/^/         /' "$RX_LOG.send" | head -5
fi
rm -f "$RX_LOG.send"

# ---------------------------------------------------------------- 6. 质量
if grep -q 'invalid frame' "$RX_LOG"; then
    bad "存在 CRC / 长度校验失败的帧（CRC 或协议不匹配）"
else
    ok "没有 CRC / 长度校验失败的帧"
fi

if grep -q 'unknown CAN ID' "$RX_LOG"; then
    info "总线上有协议外的 ID："
    grep 'unknown CAN ID' "$RX_LOG" | sort -u | head -5 | sed 's/^/         /'
else
    ok "总线上只有协议里的三个 ID"
fi

if grep -q 'CAN error frame' "$RX_LOG"; then
    bad "总线出现过错误帧（终端电阻 / 波特率 / 线长）"
    grep -c 'CAN error frame' "$RX_LOG" | sed 's/^/         错误帧数量: /'
else
    ok "没有 CAN 错误帧"
fi

# ---------------------------------------------------------------- 汇总
printf '\n=== 结果：PASS=%d FAIL=%d ===\n' "$PASS" "$FAIL"
printf '抓包日志: %s\n' "$RX_LOG"
printf 'CSV 日志: %s/\n' "$LOGDIR"

if [ "$FAIL" -eq 0 ]; then
    printf '端到端验收通过。\n'
else
    printf '还有 %d 项没通过，按上面的 [FAIL] 逐条排查。\n' "$FAIL"
    exit 1
fi
