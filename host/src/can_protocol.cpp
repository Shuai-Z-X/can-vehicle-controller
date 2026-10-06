#include "can_protocol.hpp"

namespace canproto {

namespace {

// 所有应用报文在 v0.1 中都固定为 8 字节。
bool is_dlc8(const can_frame& frame) {
    return frame.can_dlc == 8;
}

// 把两个字节按小端序组合成 16 位整数。
// 例如 2C 01 -> 0x012C。
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

// 0=OFF, 1=STANDBY, 2=RUN, 3=FAULT。
// 其他值视为非法，防止把错误字节当作有效模式。
bool valid_mode(std::uint8_t raw) {
    return raw <= 3;
}

}  // namespace

// CRC-8 实现。
// 输入只覆盖字节 0~6，不包含第 7 字节的校验和本身。
std::uint8_t crc8(const std::uint8_t* data, std::size_t length) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80) {
                crc = static_cast<std::uint8_t>((crc << 1) ^ 0x07);
            } else {
                crc = static_cast<std::uint8_t>(crc << 1);
            }
        }
    }
    return crc;
}

// 解析 0x100 VCU_Status。
// 字节布局：
//   [0]    VehMode
//   [1-2]  VehSpeed，小端，因子 0.1
//   [3-4]  BatteryVoltage，小端，因子 0.01
//   [5]    故障位
//   [6]    滚动计数器
//   [7]    CRC-8
bool decode_vcu_status(const can_frame& frame, VcuStatus& out) {
    // 先检查 ID 和 DLC，避免把别的报文误当成本报文。
    if ((frame.can_id & CAN_SFF_MASK) != kIdVcuStatus || !is_dlc8(frame)) {
        return false;
    }

    const std::uint8_t* d = frame.data;

    // CRC 错误或模式非法时拒绝这一帧。
    if (crc8(d, 7) != d[7] || !valid_mode(d[0])) {
        return false;
    }

    out.mode = static_cast<Mode>(d[0]);
    out.speed_kmh = static_cast<double>(le16(&d[1])) * 0.1;
    out.battery_voltage_v = static_cast<double>(le16(&d[3])) * 0.01;

    // 第 5 字节的每个 bit 对应一种故障。
    out.fault_can_timeout = (d[5] & 0x01U) != 0;
    out.fault_sensor_range = (d[5] & 0x02U) != 0;
    out.fault_actuator = (d[5] & 0x04U) != 0;
    out.fault_over_voltage = (d[5] & 0x08U) != 0;
    out.fault_under_voltage = (d[5] & 0x10U) != 0;

    out.counter = d[6];
    out.checksum = d[7];
    return true;
}

// 解析 0x101 Actuator_Status。
// 字节布局：
//   [0]    ActuatorMode
//   [1-2]  ActuatorOutput，小端，因子 0.1
//   [3-4]  MotorCurrent，小端，因子 0.01
//   [5]    Temperature，偏移 -40
//   [6]    滚动计数器
//   [7]    CRC-8
bool decode_actuator_status(const can_frame& frame, ActuatorStatus& out) {
    if ((frame.can_id & CAN_SFF_MASK) != kIdActuatorStatus || !is_dlc8(frame)) {
        return false;
    }

    const std::uint8_t* d = frame.data;

    if (crc8(d, 7) != d[7] || !valid_mode(d[0])) {
        return false;
    }

    out.mode = static_cast<Mode>(d[0]);
    out.output_pct = static_cast<double>(le16(&d[1])) * 0.1;
    out.current_a = static_cast<double>(le16(&d[3])) * 0.01;

    // 温度使用偏移 -40：原始值 85 表示 45 摄氏度。
    out.temperature_c = static_cast<int>(d[5]) - 40;

    out.counter = d[6];
    out.checksum = d[7];
    return true;
}

// 解析 0x200 Control_Cmd。
// 字节布局：
//   [0]    CmdMode
//   [1-2]  TargetOutput，小端，因子 0.1
//   [3-5]  CmdReserved
//   [6]    CmdCounter
//   [7]    CmdChecksum
bool decode_control_cmd(const can_frame& frame, ControlCmd& out) {
    if ((frame.can_id & CAN_SFF_MASK) != kIdControlCmd || !is_dlc8(frame)) {
        return false;
    }

    const std::uint8_t* d = frame.data;

    if (crc8(d, 7) != d[7] || !valid_mode(d[0])) {
        return false;
    }

    out.mode = static_cast<Mode>(d[0]);
    out.target_output_pct = static_cast<double>(le16(&d[1])) * 0.1;
    out.counter = d[6];
    out.checksum = d[7];
    return true;
}

// 编码 0x200 Control_Cmd。
// 生成 8 字节 CAN 帧，并自动计算第 7 字节的 CRC。
bool encode_control_cmd(const ControlCmd& command, can_frame& out) {
    const std::uint8_t mode = static_cast<std::uint8_t>(command.mode);

    // 协议规定目标输出范围是 0.0% ~ 100.0%。
    if (mode > 3 || command.target_output_pct < 0.0 ||
        command.target_output_pct > 100.0) {
        return false;
    }

    // 缩放因子 0.1：30.0% -> 300 -> 0x012C。
    const std::uint16_t raw_target =
        static_cast<std::uint16_t>(command.target_output_pct / 0.1 + 0.5);

    out = can_frame{};
    out.can_id = kIdControlCmd;
    out.can_dlc = 8;

    out.data[0] = mode;                                          // CmdMode
    out.data[1] = static_cast<std::uint8_t>(raw_target & 0xFF);  // TargetOutput 低字节
    out.data[2] = static_cast<std::uint8_t>((raw_target >> 8) & 0xFF);  // 高字节
    out.data[3] = 0;  // CmdReserved 低字节
    out.data[4] = 0;  // CmdReserved 中字节
    out.data[5] = 0;  // CmdReserved 高字节
    out.data[6] = command.counter;  // CmdCounter
    out.data[7] = 0;  // 先清零，再计算 CRC
    out.data[7] = crc8(out.data, 7);
    return true;
}

// 把模式枚举转换成人类可读字符串。
const char* to_string(Mode mode) {
    switch (mode) {
        case Mode::Off:
            return "OFF";
        case Mode::Standby:
            return "STANDBY";
        case Mode::Run:
            return "RUN";
        case Mode::Fault:
            return "FAULT";
    }
    return "UNKNOWN";
}

// 把计数器检查结果转换成字符串。
const char* to_string(CounterResult result) {
    switch (result) {
        case CounterResult::First:
            return "FIRST";
        case CounterResult::Ok:
            return "OK";
        case CounterResult::Missed:
            return "MISSED";
        case CounterResult::Duplicate:
            return "DUPLICATE";
        case CounterResult::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

// 检查 8 位滚动计数器。
// diff 的含义：
//   1      -> 正常连续
//   2~3    -> 中间可能丢帧
//   0      -> 重复帧
//   其他   -> 计数器异常
CounterResult CounterChecker::check(canid_t id, std::uint8_t counter) {
    auto it = last_.find(id);
    if (it == last_.end()) {
        last_[id] = counter;
        return CounterResult::First;
    }

    const std::uint8_t diff = static_cast<std::uint8_t>(counter - it->second);
    it->second = counter;

    if (diff == 1) {
        return CounterResult::Ok;
    }
    if (diff >= 2 && diff <= 3) {
        return CounterResult::Missed;
    }
    if (diff == 0) {
        return CounterResult::Duplicate;
    }
    return CounterResult::Error;
}

}  // namespace canproto
