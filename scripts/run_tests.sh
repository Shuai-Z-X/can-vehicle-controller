#!/usr/bin/env bash
#
# Day 5 一键构建和测试脚本。
#
# 作用：
#   1. 用 CMake 编译 can_monitor 和 can_send。
#   2. 如果存在 .venv，就用虚拟环境跑 pytest。
#   3. 否则用系统 python3 跑 pytest。
#
set -euo pipefail

# 切换到仓库根目录，保证相对路径正确。
cd "$(dirname "$0")/.."

# 构建 host 工程。
cmake -S host -B build/host
cmake --build build/host -j

# 运行 Day 5 测试。
if [ -x .venv/bin/python ]; then
    .venv/bin/python -m pytest host/tests -v
else
    python3 -m pytest host/tests -v
fi
