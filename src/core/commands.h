#pragma once
#include "core/protocol.h"
#include <string>
#include <string_view>

namespace portwave {
struct CommandStep {
    Bytes data;
    bool pause{};
};
struct CommandResult {
    std::vector<CommandStep> steps;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
// Validate the ENTIRE command before any bytes are sent. UTF-8 is passed through.
CommandResult parseCommand(std::string_view text, bool hex, std::string_view clipboard = {});
std::string normalizePaste(std::string_view text);
} // namespace portwave
