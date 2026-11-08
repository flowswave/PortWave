#include "core/commands.h"
#include <cctype>

namespace portwave {
std::string normalizePaste(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' || text[i] == '\n') {
            if (i + 1 < text.size() && text[i + 1] != text[i] && (text[i + 1] == '\r' || text[i + 1] == '\n'))
                ++i;
            out += '\r';
        } else
            out += text[i];
    }
    return out;
}
CommandResult parseCommand(std::string_view text, bool hex, std::string_view clipboard) {
    CommandResult result;
    Bytes data;
    auto fail = [&](std::string msg) { return CommandResult{{}, std::move(msg)}; };
    if (hex) {
        int high = -1;
        for (unsigned char c : text) {
            if (std::isspace(c))
                continue;
            int n = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
            if (n < 0)
                return fail("HEX contains an invalid digit");
            if (high < 0)
                high = n;
            else {
                data.push_back(static_cast<std::uint8_t>(high * 16 + n));
                high = -1;
            }
        }
        if (high >= 0)
            return fail("HEX requires pairs of digits");
    } else {
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto c = static_cast<std::uint8_t>(text[i]);
            if (c != '\\') {
                data.push_back(c);
                continue;
            }
            if (++i == text.size())
                return fail("Incomplete escape");
            c = static_cast<std::uint8_t>(text[i]);
            switch (c) {
            case 'r':
                data.push_back(13);
                break;
            case 'n':
                data.push_back(10);
                break;
            case 't':
                data.push_back(9);
                break;
            case 'b':
                data.push_back(8);
                break;
            case 'e':
                data.push_back(27);
                break;
            case '\\':
                data.push_back('\\');
                break;
            case 'p':
                result.steps.push_back({std::move(data), false});
                data.clear();
                result.steps.push_back({{}, true});
                break;
            case 'v': {
                const auto paste = normalizePaste(clipboard);
                data.insert(data.end(), paste.begin(), paste.end());
                break;
            }
            default:
                if (c < '0' || c > '7' || i + 2 >= text.size())
                    return fail("Unsupported or incomplete escape");
                {
                    unsigned value = 0;
                    for (int j = 0; j < 3; ++j) {
                        auto d = text[i + static_cast<std::size_t>(j)];
                        if (d < '0' || d > '7')
                            return fail("Invalid octal escape");
                        value = value * 8 + static_cast<unsigned>(d - '0');
                    }
                    if (value > 255)
                        return fail("Octal byte exceeds 255");
                    data.push_back(static_cast<std::uint8_t>(value));
                    i += 2;
                }
            }
        }
    }
    if (!data.empty())
        result.steps.push_back({std::move(data), false});
    return result;
}
} // namespace portwave
