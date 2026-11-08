#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace portwave {
using Bytes = std::vector<std::uint8_t>;
std::uint32_t crc32(std::span<const std::uint8_t> bytes);
struct Frame {
    std::uint16_t total{}, sequence{};
    std::uint8_t opcode{};
    Bytes payload;
};
Bytes packFrame(const Frame& frame); // Throws invalid_argument for payload > 65535.
// Single worker thread only. Incomplete frames remain bounded to 65548 bytes.
class FrameParser {
  public:
    std::vector<Frame> feed(std::span<const std::uint8_t> bytes);
    void reset() { buffer_.clear(); }

  private:
    Bytes buffer_;
};
class LogFrameParser {
  public:
    std::vector<Bytes> feed(std::span<const std::uint8_t> bytes);

  private:
    Bytes buffer_;
};
void append16(Bytes& bytes, std::uint16_t value);
void append32(Bytes& bytes, std::uint32_t value);
} // namespace portwave
