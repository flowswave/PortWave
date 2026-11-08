#include "core/ansi.h"
#include <array>
#include <charconv>

namespace portwave {
std::string AnsiTextFilter::feed(std::string_view bytes) {
    std::string output;
    output.reserve(bytes.size());
    for (const unsigned char c : bytes) {
        if (state_ == State::String || state_ == State::StringEscape) {
            if ((osc_ && c == 7) || (state_ == State::StringEscape && c == '\\'))
                state_ = State::Text;
            else
                state_ = c == 27 ? State::StringEscape : State::String;
            continue;
        }
        if (c == 27) {
            state_ = State::Escape;
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            output += static_cast<char>(c);
            state_ = State::Text;
            continue;
        }
        switch (state_) {
        case State::Text:
            if (c >= 0x20 && c != 0x7f)
                output += static_cast<char>(c);
            break;
        case State::Escape:
            if (c == '[')
                state_ = State::Csi;
            else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
                osc_ = c == ']';
                state_ = State::String;
            } else
                state_ = c >= 0x20 && c <= 0x2f ? State::Intermediate : State::Text;
            break;
        case State::Intermediate:
            if (c < 0x20 || c > 0x2f)
                state_ = State::Text;
            break;
        case State::Csi:
            if (c >= 0x40 && c <= 0x7e)
                state_ = State::Text;
            break;
        default:
            break;
        }
    }
    return output;
}
namespace {
int palette(int n) {
    constexpr std::array<int, 16> base{0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
                                       0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
                                       0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
    if (n < 0 || n > 255)
        return -1;
    if (n < 16)
        return base[static_cast<std::size_t>(n)];
    if (n >= 232) {
        int v = 8 + (n - 232) * 10;
        return v * 65536 + v * 256 + v;
    }
    n -= 16;
    auto level = [](int x) { return x == 0 ? 0 : 55 + x * 40; };
    return level(n / 36) * 65536 + level(n / 6 % 6) * 256 + level(n % 6);
}
} // namespace
void AnsiDecoder::sgr(std::string_view text) {
    std::vector<int> p;
    std::size_t pos = 0;
    do {
        auto end = text.find(';', pos);
        if (end == text.npos)
            end = text.size();
        int n = 0;
        std::from_chars(text.data() + pos, text.data() + end, n);
        p.push_back(n);
        pos = end + 1;
    } while (pos <= text.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        int n = p[i];
        if (n == 0)
            style_ = {};
        else if (n == 1) {
            style_.bold = true;
            if (style_.foregroundIndex >= 0 && style_.foregroundIndex < 8)
                style_.foreground = palette(style_.foregroundIndex + 8);
        } else if (n == 22) {
            style_.bold = false;
            if (style_.foregroundIndex >= 0 && style_.foregroundIndex < 8)
                style_.foreground = palette(style_.foregroundIndex);
        } else if (n == 39) {
            style_.foreground = -1;
            style_.foregroundIndex = -1;
        } else if (n == 49)
            style_.background = -1;
        else if (n >= 30 && n <= 37) {
            style_.foregroundIndex = n - 30;
            style_.foreground = palette(n - 30 + (style_.bold ? 8 : 0));
        } else if (n >= 90 && n <= 97) {
            style_.foregroundIndex = n - 90 + 8;
            style_.foreground = palette(n - 90 + 8);
        } else if (n >= 40 && n <= 47)
            style_.background = palette(n - 40);
        else if (n >= 100 && n <= 107)
            style_.background = palette(n - 100 + 8);
        else if ((n == 38 || n == 48) && i + 2 < p.size()) {
            int value = -1;
            if (p[i + 1] == 5) {
                const int index = p[i + 2];
                if (n == 38)
                    style_.foregroundIndex = index >= 0 && index <= 255 ? index : -1;
                value = palette(index + (n == 38 && style_.bold && index >= 0 && index < 8 ? 8 : 0));
                i += 2;
            } else if (p[i + 1] == 2 && i + 4 < p.size()) {
                if (n == 38)
                    style_.foregroundIndex = -1;
                if (p[i + 2] >= 0 && p[i + 2] <= 255 && p[i + 3] >= 0 && p[i + 3] <= 255 && p[i + 4] >= 0 &&
                    p[i + 4] <= 255)
                    value = p[i + 2] * 65536 + p[i + 3] * 256 + p[i + 4];
                i += 4;
            }
            if (n == 38)
                style_.foreground = value;
            else
                style_.background = value;
        }
    }
}
std::vector<TerminalRun> AnsiDecoder::feed(std::string_view bytes) {
    pending_.append(bytes);
    std::vector<TerminalRun> out;
    std::size_t i = 0, start = 0;
    auto flush = [&](std::size_t end) {
        if (end > start)
            out.push_back({TerminalRun::Kind::Text, pending_.substr(start, end - start), style_, 0});
    };
    while (i < pending_.size()) {
        const auto c = static_cast<unsigned char>(pending_[i]);
        if (c == 27) {
            flush(i);
            if (i + 1 >= pending_.size())
                break;
            if (pending_[i + 1] == '[') {
                auto end = i + 2;
                while (end < pending_.size() && !(pending_[end] >= '@' && pending_[end] <= '~'))
                    ++end;
                if (end == pending_.size()) {
                    if (end - i > 128) {
                        i = end;
                        start = i;
                    }
                    break;
                }
                const auto args = std::string_view(pending_).substr(i + 2, end - i - 2);
                if (pending_[end] == 'm')
                    sgr(args);
                else if (pending_[end] == 'K') {
                    int n = 0;
                    std::from_chars(args.data(), args.data() + args.size(), n);
                    out.push_back({TerminalRun::Kind::EraseLine, {}, style_, n});
                }
                i = end + 1;
                start = i;
                continue;
            }
            if (pending_[i + 1] == ']') {
                auto end = pending_.find('\a', i + 2);
                auto st = pending_.find("\x1b\\", i + 2);
                if (st != pending_.npos && (end == pending_.npos || st < end)) {
                    i = st + 2;
                    start = i;
                    continue;
                }
                if (end == pending_.npos) {
                    if (pending_.size() - i > 4096) {
                        i = pending_.size();
                        start = i;
                    }
                    break;
                }
                i = end + 1;
                start = i;
                continue;
            }
            i += 2;
            start = i;
            continue;
        }
        if (c == '\r' || c == '\n' || c == '\b') {
            flush(i);
            auto kind = c == '\r'   ? TerminalRun::Kind::CarriageReturn
                        : c == '\n' ? TerminalRun::Kind::Newline
                                    : TerminalRun::Kind::Backspace;
            out.push_back({kind, {}, style_, 0});
            ++i;
            start = i;
            continue;
        }
        if (c >= 0xc0) {
            std::size_t n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (i + n > pending_.size()) {
                flush(i);
                start = i;
                break;
            }
            i += n;
        } else
            ++i;
    }
    if (i == pending_.size())
        flush(i);
    pending_.erase(0, i);
    return out;
}
} // namespace portwave
