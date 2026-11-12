// The newest part of a log, at most maxBytes, cut at a line start: the debug report keeps at most 1 MB of logs (newest lines last).
#pragma once
#include <cstddef>
#include <string>

namespace dect2 {

constexpr size_t kReportLogBytes = 1u << 20;

// text: the whole log, oldest first. The result ends with the same last line and starts with a whole line; when lines were left out,
// a first line "[N earlier lines left out]" says how many (that line is counted in maxBytes too).
inline std::string logTail(const std::string& text, size_t maxBytes = kReportLogBytes) {
    if (text.size() <= maxBytes) return text;
    const size_t room = maxBytes > 64 ? maxBytes - 64 : 0;   // space for the note line
    size_t from = text.size() - room;
    while (from < text.size() && from > 0 && text[from - 1] != '\n') from++;   // the next line start
    size_t dropped = 0;
    for (size_t i = 0; i < from; i++) if (text[i] == '\n') dropped++;
    return "[" + std::to_string(dropped) + " earlier lines left out]\n" + text.substr(from);
}

}  // namespace dect2
