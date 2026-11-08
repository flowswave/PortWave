#include "core/ansi.h"
#include "core/protocol.h"
#include <chrono>
#include <iostream>

int main() {
    portwave::Bytes bytes(1024 * 1024, 42);
    auto start = std::chrono::steady_clock::now();
    std::uint32_t checksum = 0;
    for (int i = 0; i < 100; ++i)
        checksum ^= portwave::crc32(bytes);
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "crc32,100_MiB," << seconds << "_seconds," << (100 / seconds) << "_MiB_per_second,"
              << checksum << "\n";
}
