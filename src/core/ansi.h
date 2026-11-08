#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace portwave {
// Streaming byte filter for plain-text exports. Confined to its owning worker;
// retains only finite parser state across chunks (no buffered control strings).
// Strips ESC sequences (CSI/OSC/DCS/SOS/PM/APC) and other ASCII controls;
// preserves UTF-8 bytes and CR/LF/TAB without emulating terminal cursor edits.
// Incomplete controls are discarded at stream end; reset before a new stream.
class AnsiTextFilter {
  public:
    std::string feed(std::string_view bytes);
    void reset() { state_ = State::Text; }

  private:
    enum class State { Text, Escape, Intermediate, Csi, String, StringEscape };
    State state_ = State::Text;
    bool osc_{};
};
struct AnsiStyle {
    int foreground = -1, background = -1;
    bool bold = false;
    int foregroundIndex = -1;
}; // RGB or -1 default.
struct TerminalRun {
    enum class Kind { Text, CarriageReturn, Newline, Backspace, EraseLine } kind = Kind::Text;
    std::string utf8;
    AnsiStyle style;
    int argument{};
};
// One decoder per connection; keeps incomplete CSI/UTF-8 sequences across reads.
class AnsiDecoder {
  public:
    std::vector<TerminalRun> feed(std::string_view bytes);
    void reset() {
        pending_.clear();
        style_ = {};
    }

  private:
    void sgr(std::string_view parameters);
    std::string pending_;
    AnsiStyle style_;
};
} // namespace portwave
