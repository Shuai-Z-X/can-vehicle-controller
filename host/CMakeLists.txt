cmake_minimum_required(VERSION 3.16)
project(can_host LANGUAGES CXX)

# 使用 C++17：需要结构化绑定、std::filesystem 和泛型 lambda 等特性。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# can_send 使用 std::this_thread::sleep_for，需要链接线程库。
find_package(Threads REQUIRED)

# 协议层静态库：CRC、编解码、计数器检查。
add_library(can_protocol STATIC src/can_protocol.cpp)
target_include_directories(can_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(can_protocol PRIVATE -Wall -Wextra -Wpedantic)

# 接收端：监听 SocketCAN，解析信号并写 CSV。
add_executable(can_monitor src/can_monitor.cpp)
target_link_libraries(can_monitor PRIVATE can_protocol)
target_compile_options(can_monitor PRIVATE -Wall -Wextra -Wpedantic)

# 发送端：发送 0x200 Control_Cmd。
add_executable(can_send src/can_send.cpp)
target_link_libraries(can_send PRIVATE can_protocol Threads::Threads)
target_compile_options(can_send PRIVATE -Wall -Wextra -Wpedantic)
