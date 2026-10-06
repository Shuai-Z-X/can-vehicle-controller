"""Day 5 CAN protocol and integration tests.

这些测试覆盖：
1. DBC 是否正确加载。
2. 黄金测试向量的 CRC 是否正确。
3. cantools 解析结果是否与预期一致。
4. Control_Cmd 编码结果是否与已知帧一致。
5. 错误 CRC 是否会被检测出来。
6. 滚动计数器规则是否正确。
7. can_monitor 是否能通过 vcan0 正确解析 VCU_Status。

前面的测试只依赖 Python 和 cantools；最后一个集成测试需要 vcan0
和已经编译好的 can_monitor。条件不满足时会自动跳过。
"""

import json
import subprocess
import time
from pathlib import Path

import can
import cantools
import pytest

# 仓库根目录：host/tests/test_day5.py -> parents[2] 就是仓库根目录。
ROOT = Path(__file__).resolve().parents[2]
DBC_PATH = ROOT / "protocols" / "can_matrix.dbc"
VECTORS_PATH = ROOT / "protocols" / "test_vectors.json"
MONITOR = ROOT / "build" / "host" / "can_monitor"
VCAN = Path("/sys/class/net/vcan0")


def crc8(data: bytes) -> int:
    """CRC-8，多项式 0x07，初值 0x00，不反转，结果异或 0x00。"""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


@pytest.fixture(scope="session")
def db():
    """加载一次 DBC，所有测试共享。"""
    return cantools.database.load_file(DBC_PATH)


@pytest.fixture(scope="session")
def vectors():
    """读取 Day 2 定义的黄金测试向量。"""
    return json.loads(VECTORS_PATH.read_text(encoding="utf-8"))["frames"]


def test_dbc_has_three_messages(db):
    """DBC 应包含三条应用报文，且 ID 与协议文档一致。"""
    names = {message.name for message in db.messages}
    assert names == {"VCU_Status", "Actuator_Status", "Control_Cmd"}
    assert db.get_message_by_name("VCU_Status").frame_id == 0x100
    assert db.get_message_by_name("Actuator_Status").frame_id == 0x101
    assert db.get_message_by_name("Control_Cmd").frame_id == 0x200


def test_all_vectors_have_valid_crc(vectors):
    """每个黄金测试向量的第 7 字节都必须等于前 7 字节的 CRC-8。"""
    for item in vectors:
        data = bytes.fromhex(item["data"].replace(" ", ""))
        assert crc8(data[:7]) == data[7], item["name"]


def test_all_vectors_decode_as_expected(db, vectors):
    """cantools 解析出的每个信号都应等于测试向量中的期望值。"""
    for item in vectors:
        message = db.get_message_by_name(item["name"])
        data = bytes.fromhex(item["data"].replace(" ", ""))
        decoded = message.decode(data, decode_choices=False)
        for key, expected in item["decoded"].items():
            actual = decoded[key]
            if isinstance(expected, float):
                assert actual == pytest.approx(expected, abs=1e-6)
            else:
                assert actual == expected


def test_encode_control_cmd_known_vector(db):
    """Control_Cmd 编码结果应等于已知帧 022C010000000934。"""
    message = db.get_message_by_name("Control_Cmd")
    data = bytearray(
        message.encode(
            {
                "CmdMode": 2,
                "TargetOutput": 30.0,
                "CmdReserved": 0,
                "CmdCounter": 9,
                "CmdChecksum": 0,
            },
            scaling=True,
            padding=True,
        )
    )
    data[7] = crc8(bytes(data[:7]))
    assert bytes(data) == bytes.fromhex("022C010000000934")


def test_invalid_checksum_is_detected():
    """故意破坏第 7 字节后，重新计算的 CRC 不应等于它。"""
    data = bytearray(bytes.fromhex("022C010000000934"))
    data[7] ^= 0xFF
    assert crc8(bytes(data[:7])) != data[7]


def test_counter_checker_rules():
    """验证 FIRST/OK/MISSED/DUPLICATE/ERROR 五种计数器结果。"""

    def result(last, current):
        if last is None:
            return "FIRST"
        diff = (current - last) & 0xFF
        if diff == 1:
            return "OK"
        if 2 <= diff <= 3:
            return "MISSED"
        if diff == 0:
            return "DUPLICATE"
        return "ERROR"

    assert result(None, 5) == "FIRST"
    assert result(5, 6) == "OK"
    assert result(6, 8) == "MISSED"
    assert result(8, 8) == "DUPLICATE"
    assert result(9, 1) == "ERROR"


@pytest.mark.skipif(not VCAN.exists(), reason="vcan0 is not available")
@pytest.mark.skipif(not MONITOR.exists(), reason="can_monitor is not built")
def test_monitor_decodes_vcu_status(tmp_path):
    """集成测试：启动 can_monitor，发送一帧 0x100，检查解析输出。"""
    process = subprocess.Popen(
        [str(MONITOR), "vcan0", str(tmp_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    # 给 can_monitor 一点时间完成 socket 绑定。
    time.sleep(0.5)

    try:
        # python-can 通过 SocketCAN 向 vcan0 发送一帧 VCU_Status。
        bus = can.Bus(interface="socketcan", channel="vcan0")
        bus.send(
            can.Message(
                arbitration_id=0x100,
                data=bytes.fromhex("027B009209000505"),
                is_extended_id=False,
            )
        )
        bus.shutdown()
        time.sleep(0.5)
    finally:
        # 发送 SIGTERM，让 can_monitor 正常退出并刷新 CSV。
        # 如果程序没有及时退出，再强制 kill，避免测试一直卡住。
        process.terminate()
        try:
            output, _ = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()

    assert "VCU_Status mode=RUN" in output
    assert "speed=12.3" in output
    assert "voltage=24.50" in output
