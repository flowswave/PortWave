#include "core/protocol.h"
#include <algorithm>
#include <stdexcept>

namespace portwave {
namespace {
std::uint16_t read16(const Bytes& b, std::size_t i) {
    return static_cast<std::uint16_t>(b[i] | (static_cast<unsigned>(b[i + 1]) << 8U));
}
std::uint32_t read32(const Bytes& b, std::size_t i) {
    return b[i] | (std::uint32_t(b[i + 1]) << 8U) | (std::uint32_t(b[i + 2]) << 16U) |
           (std::uint32_t(b[i + 3]) << 24U);
}
} // namespace
void append16(Bytes& b, std::uint16_t v) {
    b.push_back(v & 255U);
    b.push_back(static_cast<std::uint32_t>(v) >> 8U);
}
void append32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back((v >> (8U * static_cast<unsigned>(i))) & 255U);
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffff;
    for (auto b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffff;
}
Bytes packFrame(const Frame& f) {
    if (f.payload.size() > 65535)
        throw std::invalid_argument("IAP payload too large");
    Bytes result{0xaa, 0x55};
    append16(result, f.total);
    append16(result, f.sequence);
    result.push_back(f.opcode);
    append16(result, static_cast<std::uint16_t>(f.payload.size()));
    result.insert(result.end(), f.payload.begin(), f.payload.end());
    append32(result, crc32(result));
    return result;
}
std::vector<Frame> FrameParser::feed(std::span<const std::uint8_t> bytes) {
    std::vector<Frame> frames;
    // Feed bounded slices so even a hostile input cannot create an unbounded buffer.
    for (auto byte : bytes) {
        buffer_.push_back(byte);
        while (buffer_.size() >= 2) {
            if (buffer_[0] != 0xaa || buffer_[1] != 0x55) {
                buffer_.erase(buffer_.begin());
                continue;
            }
            if (buffer_.size() < 9)
                break;
            const std::size_t size = 13 + read16(buffer_, 7);
            if (buffer_.size() < size)
                break;
            if (read32(buffer_, size - 4) == crc32(std::span(buffer_).first(size - 4))) {
                frames.push_back(
                    {read16(buffer_, 2), read16(buffer_, 4), buffer_[6],
                     Bytes(buffer_.begin() + 9, buffer_.begin() + static_cast<std::ptrdiff_t>(size) - 4)});
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size));
            } else
                buffer_.erase(buffer_.begin());
        }
    }
    return frames;
}
std::vector<Bytes> LogFrameParser::feed(std::span<const std::uint8_t> bytes) {
    std::vector<Bytes> result;
    for (auto b : bytes) {
        buffer_.push_back(b);
        while (buffer_.size() >= 2) {
            const bool current = buffer_[0] == 0x57 && buffer_[1] == 0x62;
            const bool legacy = buffer_[0] == 0x43 && buffer_[1] == 0x4c;
            if (!current && !legacy) {
                buffer_.erase(buffer_.begin());
                continue;
            }
            if (buffer_.size() < 4)
                break;
            const std::size_t size = 8 + read16(buffer_, 2);
            if (buffer_.size() < size)
                break;
            // Legacy CL has zero CRC; modern Wb validates the complete frame.
            if (legacy || read32(buffer_, size - 4) == crc32(std::span(buffer_).first(size - 4)))
                result.emplace_back(buffer_.begin() + 4,
                                    buffer_.begin() + static_cast<std::ptrdiff_t>(size) - 4);
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size));
        }
    }
    return result;
}
} // namespace portwave
