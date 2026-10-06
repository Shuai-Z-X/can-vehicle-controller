#include "can_protocol.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <string>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t g_running = 1;

void handle_signal(int) {
    g_running = 0;
}

std::string wall_time_string() {
    using namespace std::chrono;

    const auto now = system_clock::now();

    // 注意：不要把变量命名为 seconds，否则会遮蔽 std::chrono::seconds 类型。
    const auto now_seconds = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - now_seconds).count();
    const std::time_t tt = system_clock::to_time_t(now);

    std::tm tm{};
    if (const std::tm* tmp = std::localtime(&tt)) {
        tm = *tmp;
    }

    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm);

    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lld", base,
                  static_cast<long long>(millis));
    return out;
}

std::string fault_string(const canproto::VcuStatus& status) {
    std::string result;
    const auto add = [&result](const char* name) {
        if (!result.empty()) {
            result += "|";
        }
        result += name;
    };

    if (status.fault_can_timeout) {
        add("CAN_TIMEOUT");
    }
    if (status.fault_sensor_range) {
        add("SENSOR_RANGE");
    }
    if (status.fault_actuator) {
        add("ACTUATOR");
    }
    if (status.fault_over_voltage) {
        add("OVERVOLTAGE");
    }
    if (status.fault_under_voltage) {
        add("UNDERVOLTAGE");
    }

    return result.empty() ? "NONE" : result;
}

void print_vcu_status(const canproto::VcuStatus& status,
                      canproto::CounterResult counter) {
    const std::string ts = wall_time_string();
    const std::string faults = fault_string(status);
    std::printf("[%s] VCU_Status mode=%s speed=%.1f km/h voltage=%.2f V "
                "faults=%s counter=%u/%s crc=OK\n",
                ts.c_str(),
                canproto::to_string(status.mode),
                status.speed_kmh,
                status.battery_voltage_v,
                faults.c_str(),
                static_cast<unsigned>(status.counter),
                canproto::to_string(counter));
}

void print_actuator_status(const canproto::ActuatorStatus& status,
                           canproto::CounterResult counter) {
    const std::string ts = wall_time_string();
    std::printf("[%s] Actuator_Status mode=%s output=%.1f %% current=%.2f A "
                "temp=%d C counter=%u/%s crc=OK\n",
                ts.c_str(),
                canproto::to_string(status.mode),
                status.output_pct,
                status.current_a,
                status.temperature_c,
                static_cast<unsigned>(status.counter),
                canproto::to_string(counter));
}

void print_control_cmd(const canproto::ControlCmd& command,
                       canproto::CounterResult counter) {
    const std::string ts = wall_time_string();
    std::printf("[%s] Control_Cmd mode=%s target=%.1f %% counter=%u/%s "
                "crc=OK\n",
                ts.c_str(),
                canproto::to_string(command.mode),
                command.target_output_pct,
                static_cast<unsigned>(command.counter),
                canproto::to_string(counter));
}

void write_vcu_csv(std::ofstream& out, const canproto::VcuStatus& status) {
    out << wall_time_string() << ",0x100,"
        << canproto::to_string(status.mode) << ','
        << status.speed_kmh << ',' << status.battery_voltage_v << ','
        << status.fault_can_timeout << ',' << status.fault_sensor_range << ','
        << status.fault_actuator << ',' << status.fault_over_voltage << ','
        << status.fault_under_voltage << ','
        << static_cast<unsigned>(status.counter) << ','
        << static_cast<unsigned>(status.checksum) << '\n';
    out.flush();
}

void write_actuator_csv(std::ofstream& out,
                        const canproto::ActuatorStatus& status) {
    out << wall_time_string() << ",0x101,"
        << canproto::to_string(status.mode) << ','
        << status.output_pct << ',' << status.current_a << ','
        << status.temperature_c << ','
        << static_cast<unsigned>(status.counter) << ','
        << static_cast<unsigned>(status.checksum) << '\n';
    out.flush();
}

void write_control_csv(std::ofstream& out,
                       const canproto::ControlCmd& command) {
    out << wall_time_string() << ",0x200,"
        << canproto::to_string(command.mode) << ','
        << command.target_output_pct << ','
        << static_cast<unsigned>(command.counter) << ','
        << static_cast<unsigned>(command.checksum) << '\n';
    out.flush();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 3) {
        std::fprintf(stderr, "usage: %s [can_interface] [log_dir]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* ifname = (argc > 1) ? argv[1] : "vcan0";
    const std::filesystem::path log_dir = (argc > 2) ? argv[2] : "logs";

    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n",
                     log_dir.string().c_str(), ec.message().c_str());
        return EXIT_FAILURE;
    }

    std::ofstream vcu_csv(log_dir / "vcu_status.csv");
    std::ofstream actuator_csv(log_dir / "actuator_status.csv");
    std::ofstream command_csv(log_dir / "control_cmd.csv");

    if (!vcu_csv || !actuator_csv || !command_csv) {
        std::fprintf(stderr, "cannot open CSV files in %s\n",
                     log_dir.string().c_str());
        return EXIT_FAILURE;
    }

    vcu_csv << "timestamp,id,mode,speed_kmh,voltage_v,fault_can_timeout,"
               "fault_sensor_range,fault_actuator,fault_over_voltage,"
               "fault_under_voltage,counter,checksum\n";
    actuator_csv << "timestamp,id,mode,output_pct,current_a,temperature_c,"
                    "counter,checksum\n";
    command_csv << "timestamp,id,mode,target_output_pct,counter,checksum\n";

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    const int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) {
        std::perror("socket");
        return EXIT_FAILURE;
    }

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        std::perror("ioctl SIOCGIFINDEX");
        close(sock);
        return EXIT_FAILURE;
    }

    struct sockaddr_can addr {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        close(sock);
        return EXIT_FAILURE;
    }

    std::printf("listening on %s, logging to %s (Ctrl+C to stop)\n",
                ifname, log_dir.string().c_str());

    canproto::CounterChecker counter_checker;

    while (g_running) {
        // 用 poll 等待最多 100 ms。
        // 即使总线上没有报文，也会定期回到 while 检查 g_running，
        // 这样收到 SIGTERM 后最多 100 ms 就能退出。
        struct pollfd pfd {};
        pfd.fd = sock;
        pfd.events = POLLIN;

        const int poll_result = poll(&pfd, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            break;
        }

        if (poll_result == 0) {
            continue;
        }

        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        struct can_frame frame {};
        const ssize_t nbytes = read(sock, &frame, sizeof(frame));

        if (nbytes < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            std::perror("read");
            break;
        }

        if (nbytes != static_cast<ssize_t>(sizeof(frame))) {
            std::fprintf(stderr, "short read: %zd bytes\n", nbytes);
            continue;
        }

        if (frame.can_id & CAN_ERR_FLAG) {
            std::printf("[%s] CAN error frame id=0x%03X\n",
                        wall_time_string().c_str(),
                        static_cast<unsigned>(frame.can_id & CAN_ERR_MASK));
            continue;
        }

        const canid_t id = frame.can_id & CAN_SFF_MASK;

        switch (id) {
            case canproto::kIdVcuStatus: {
                canproto::VcuStatus status{};
                if (!canproto::decode_vcu_status(frame, status)) {
                    std::printf("[%s] VCU_Status invalid frame\n",
                                wall_time_string().c_str());
                    break;
                }
                const auto result = counter_checker.check(id, status.counter);
                print_vcu_status(status, result);
                write_vcu_csv(vcu_csv, status);
                break;
            }

            case canproto::kIdActuatorStatus: {
                canproto::ActuatorStatus status{};
                if (!canproto::decode_actuator_status(frame, status)) {
                    std::printf("[%s] Actuator_Status invalid frame\n",
                                wall_time_string().c_str());
                    break;
                }
                const auto result = counter_checker.check(id, status.counter);
                print_actuator_status(status, result);
                write_actuator_csv(actuator_csv, status);
                break;
            }

            case canproto::kIdControlCmd: {
                canproto::ControlCmd command{};
                if (!canproto::decode_control_cmd(frame, command)) {
                    std::printf("[%s] Control_Cmd invalid frame\n",
                                wall_time_string().c_str());
                    break;
                }
                const auto result = counter_checker.check(id, command.counter);
                print_control_cmd(command, result);
                write_control_csv(command_csv, command);
                break;
            }

            default:
                std::printf("[%s] unknown CAN ID 0x%03X dlc=%u\n",
                            wall_time_string().c_str(),
                            static_cast<unsigned>(id),
                            static_cast<unsigned>(frame.can_dlc));
                break;
        }
    }

    close(sock);
    std::printf("stopped\n");
    return EXIT_SUCCESS;
}
