#include "core/ansi.h"
#include "core/bounded_queue.h"
#include "core/commands.h"
#include "core/protocol.h"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace portwave;
static int checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        const std::string colored = "\x1b[31m中文\x1b[0m\r\n\tplain\x1b]0;title\a!"
                                    "\x1bPpayload\x1b\\\x1b(Bend";
        const std::string plain = "中文\r\n\tplain!end";
        for (std::size_t split = 0; split <= colored.size(); ++split) {
            AnsiTextFilter filter;
            auto text = filter.feed(std::string_view(colored).substr(0, split));
            text += filter.feed(std::string_view(colored).substr(split));
            check(text == plain, "plain log ANSI filter preserves UTF-8 at every split");
        }
        AnsiTextFilter filter;
        std::string bytewise;
        for (const char c : colored)
            bytewise += filter.feed(std::string_view(&c, 1));
        check(bytewise == plain, "plain log bytewise CSI/OSC/DCS/charset filtering");
        check(filter.feed("\x1b[38;2;").empty(), "incomplete control does not leak");
        filter.reset();
        check(filter.feed("new\r\n") == "new\r\n", "new log stream resets partial control");
        check(filter.feed(std::string_view("a\a\b\0", 4)) == "a", "plain logs omit C0 controls");
        const Bytes fixture{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        check(crc32(fixture) == 0xcbf43926, "CRC32 standard vector");
        Frame f{7, 2, 0x81, {1, 2, 3}};
        auto bytes = packFrame(f);
        const Bytes golden{0xaa, 0x55, 0x07, 0x00, 0x02, 0x00, 0x81, 0x03,
                           0x00, 0x01, 0x02, 0x03, 0xd6, 0x13, 0xb5, 0x78};
        check(bytes == golden, "byte-for-byte Python upgrade.pack_frame fixture");
        FrameParser parser;
        std::vector<Frame> frames;
        for (auto byte : bytes) {
            auto got = parser.feed(std::span(&byte, 1));
            frames.insert(frames.end(), got.begin(), got.end());
        }
        check(frames.size() == 1 && frames[0].payload == f.payload && frames[0].sequence == 2,
              "fragmented frame");
        bytes.back() ^= 1;
        check(parser.feed(bytes).empty(), "bad CRC rejected");
        auto valid = packFrame(f);
        Bytes noise{0, 1, 2, 0xaa};
        noise.insert(noise.end(), valid.begin(), valid.end());
        check(parser.feed(noise).size() == 1, "resynchronize after noise");
        auto cmd = parseCommand("login\\r\\p\\v\\025", false, "hello\nworld");
        check(bool(cmd) && cmd.steps.size() == 3, "command segmentation");
        check(cmd.steps[1].pause, "command pause");
        check(cmd.steps[2].data.back() == 21, "octal control");
        check(!parseCommand("AT\\r\\x", false) && parseCommand("AT\\r\\x", false).steps.empty(),
              "atomic command validation");
        check(!parseCommand("a12", true) && !parseCommand("xx", true), "invalid HEX");
        check(parseCommand("AA 55 01", true).steps[0].data.size() == 3, "HEX bytes");
        check(normalizePaste("a\r\nb\nc\rd\n\r") == "a\rb\rc\rd\r", "clipboard normalization");
        AnsiDecoder ansi;
        check(ansi.feed("\x1b[3").empty(), "fragmented CSI");
        auto runs = ansi.feed("2mOK\x1b[0m!");
        check(runs.size() == 2 && runs[0].style.foreground >= 0 && runs[1].style.foreground == -1,
              "SGR reset");
        auto rgb = ansi.feed("\x1b[38;2;1;2;3mRGB");
        check(rgb[0].style.foreground == 0x010203, "true color");
        auto standard = ansi.feed("\x1b[0;31mR\x1b[1mB\x1b[22mN\x1b[32mG");
        check(standard.size() == 4 && standard[0].style.foreground == 0xcd0000 &&
                  standard[1].style.foreground == 0xff0000 && standard[2].style.foreground == 0xcd0000 &&
                  standard[3].style.foreground == 0x00cd00,
              "reference ANSI palette and bold brightness");
        auto utf = ansi.feed("\xe4\xb8");
        check(utf.empty(), "UTF8 partial buffered");
        check(ansi.feed("\xad")[0].utf8 == "\xe4\xb8\xad", "UTF8 continuation");
        BoundedQueue queue(10);
        queue.push("12345678");
        queue.push("abcd");
        check(queue.dropped() == 8 && queue.size() == 4, "bounded overflow");
        check(queue.take(2) == "ab" && queue.take(2) == "cd", "byte budget");
        queue.push("01234567890123");
        check(queue.size() == 10 && queue.take(20) == "4567890123", "oversize chunk bounded");
        bool skipped = false;
        queue.push("old-123");
        queue.push("new");
        const auto lossBefore = queue.dropped();
        check(queue.takeLatest(5, skipped) == "23new" && skipped && queue.size() == 0,
              "latest read skips backlog atomically across chunks");
        check(queue.dropped() == lossBefore + 5, "intentional display discard is observable");
        queue.push("ok");
        check(queue.takeLatest(5, skipped) == "ok" && !skipped, "small latest read preserves all bytes");
        queue.push("discard");
        check(queue.takeLatest(0, skipped).empty() && skipped && queue.size() == 0,
              "zero latest budget safely discards all");
        BoundedQueue threaded(1024);
        std::thread writer([&] {
            for (int i = 0; i < 10000; ++i)
                threaded.push("0123456789");
        });
        for (int i = 0; i < 10000; ++i)
            threaded.take(16);
        writer.join();
        check(threaded.size() <= 1024, "thread-safe queue bounds");
        Bytes log{0x57, 0x62, 2, 0, 'O', 'K'};
        append32(log, crc32(log));
        LogFrameParser logs;
        check(logs.feed(log).at(0) == Bytes({'O', 'K'}), "Wb log frame");
        std::cout << checks << " core checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
