// can_send.cpp
// 命令行 CAN 发送工具。
//
// 用法：
//   ./can_send <can_interface> <mode 0-3> <target_pct 0-100> [count]
//
// 示例：
//   ./can_send vcan0 2 30 5
//
// 作用：
//   向指定 CAN 接口发送 count 帧 0x200 Control_Cmd。
//   每帧的计数器自动加一，CRC 由 can_protocol.cpp 计算。

#include "can_protocol.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    // 参数个数：程序名 + 接口名 + 模式 + 目标输出 + 可选次数。
    if (argc < 4 || argc > 5) {
        std::fprintf(stderr,
                     "usage: %s <can_interface> <mode 0-3> "
                     "<target_pct 0-100> [count]\n",
                     argv[0]);
        return EXIT_FAILURE;
    }

    const char* ifname = argv[1];
    const int mode = std::atoi(argv[2]);
    const double target = std::atof(argv[3]);
    const int count = (argc > 4) ? std::atoi(argv[4]) : 1;

    // 在发送前检查参数，避免生成非法 CAN 帧。
    if (mode < 0 || mode > 3 || target < 0.0 || target > 100.0 ||
        count < 1) {
        std::fprintf(stderr, "invalid mode/target/count\n");
        return EXIT_FAILURE;
    }

    // 创建 SocketCAN 原始套接字。
    // 返回的是一个文件描述符，后续用 read/write 操作。
    const int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) {
        std::perror("socket");
        return EXIT_FAILURE;
    }

    // 通过接口名找到内核接口索引，例如 vcan0 -> ifindex。
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        std::perror("ioctl SIOCGIFINDEX");
        close(sock);
        return EXIT_FAILURE;
    }

    // 把套接字绑定到指定 CAN 接口。
    struct sockaddr_can addr {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        close(sock);
        return EXIT_FAILURE;
    }

    // 每帧的滚动计数器从 0 开始，发送后自动加一。
    std::uint8_t counter = 0;

    for (int i = 0; i < count; ++i) {
        canproto::ControlCmd command{};
        command.mode = static_cast<canproto::Mode>(mode);
        command.target_output_pct = target;
        command.counter = counter++;

        // 调用协议层编码函数：填充 8 字节并计算 CRC。
        can_frame frame{};
        if (!canproto::encode_control_cmd(command, frame)) {
            std::fprintf(stderr, "encode failed\n");
            close(sock);
            return EXIT_FAILURE;
        }

        // write() 把一个完整的 can_frame 写到 SocketCAN。
        const ssize_t written = write(sock, &frame, sizeof(frame));
        if (written != static_cast<ssize_t>(sizeof(frame))) {
            std::perror("write");
            close(sock);
            return EXIT_FAILURE;
        }

        std::printf("sent 0x200 mode=%d target=%.1f counter=%u crc=0x%02X\n",
                    mode,
                    target,
                    static_cast<unsigned>(command.counter),
                    static_cast<unsigned>(frame.data[7]));
        std::fflush(stdout);

        // 以 20 ms 为周期发送，模拟真实控制报文。
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    close(sock);
    return EXIT_SUCCESS;
}
