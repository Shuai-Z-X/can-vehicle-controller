#pragma once

// can_protocol.hpp
// CAN 应用层协议接口。
//
// 这个头文件只声明“协议模块对外提供什么能力”，不包含具体实现。
// 具体实现放在 can_protocol.cpp 中。
// can_monitor.cpp、can_send.cpp 和测试代码只需要包含本文件，
// 不需要关心字节解析、CRC 和计数器检查的内部细节。

#include <cstddef>
#include <cstdint>
#include <map>

#include <linux/can.h>

namespace canproto {

// CAN 报文 ID，与 protocols/can_matrix.dbc 中的定义保持一致。
constexpr canid_t kIdVcuStatus = 0x100;
constexpr canid_t kIdActuatorStatus = 0x101;
constexpr canid_t kIdControlCmd = 0x200;

// 控制器和执行器的工作模式。
// 数值必须与 DBC 中 VAL_ 的枚举值一致。
enum class Mode : std::uint8_t {
    Off = 0,
    Standby = 1,
    Run = 2,
    Fault = 3,
};

// 0x100 VCU_Status 解析后的结果。
struct VcuStatus {
    Mode mode{Mode::Off};
    double speed_kmh{0.0};
    double battery_voltage_v{0.0};
    bool fault_can_timeout{false};
    bool fault_sensor_range{false};
    bool fault_actuator{false};
    bool fault_over_voltage{false};
    bool fault_under_voltage{false};
    std::uint8_t counter{0};
    std::uint8_t checksum{0};
};

// 0x101 Actuator_Status 解析后的结果。
struct ActuatorStatus {
    Mode mode{Mode::Off};
    double output_pct{0.0};
    double current_a{0.0};
    int temperature_c{0};
    std::uint8_t counter{0};
    std::uint8_t checksum{0};
};

// 0x200 Control_Cmd 编解码使用的结构体。
struct ControlCmd {
    Mode mode{Mode::Off};
    double target_output_pct{0.0};
    std::uint8_t counter{0};
    std::uint8_t checksum{0};
};

// 滚动计数器检查结果。
enum class CounterResult {
    First,      // 第一次收到该 ID
    Ok,         // 计数器连续 +1
    Missed,     // 中间丢了 2~3 帧
    Duplicate,  // 重复帧
    Error,      // 计数器跳变过大或异常
};

// 每条报文独立记录上一次计数器值。
class CounterChecker {
public:
    CounterResult check(canid_t id, std::uint8_t counter);

private:
    std::map<canid_t, std::uint8_t> last_;
};

// CRC-8：多项式 0x07，初值 0x00，不反转，结果异或 0x00。
std::uint8_t crc8(const std::uint8_t* data, std::size_t length);

// 解码函数：成功返回 true，并把结果写入 out。
bool decode_vcu_status(const can_frame& frame, VcuStatus& out);
bool decode_actuator_status(const can_frame& frame, ActuatorStatus& out);
bool decode_control_cmd(const can_frame& frame, ControlCmd& out);

// 编码函数：把 ControlCmd 结构体编码成 0x200 CAN 帧。
// 函数内部自动填充计数器之外的字节，并计算 CRC 写入第 7 字节。
bool encode_control_cmd(const ControlCmd& command, can_frame& out);

// 枚举转字符串，用于终端输出和 CSV。
const char* to_string(Mode mode);
const char* to_string(CounterResult result);

}  // namespace canproto
