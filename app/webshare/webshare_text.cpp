// Text helpers of the OnAir Web easy setup (no UI), tested in tests/webshare/test_webshare.cpp
#include "webshare.h"
#include <cctype>

namespace webshare {

std::string parseTunnelUrl(const std::string& log) {
    const std::string tail = ".trycloudflare.com";
    for (size_t pos = 0; (pos = log.find("https://", pos)) != std::string::npos; pos += 8) {
        size_t e = pos + 8;
        while (e < log.size() && (isalnum((unsigned char)log[e]) || log[e] == '-' || log[e] == '.')) e++;
        const std::string u = log.substr(pos, e - pos);
        if (u.size() > 8 + tail.size() && u.compare(u.size() - tail.size(), tail.size(), tail) == 0 && u.find("api.") == std::string::npos) return u;
    }
    return {};
}

std::string parseAckCode(const std::string& log) {
    const std::string k = "web_directory_ack=";
    const size_t p = log.rfind(k);
    if (p == std::string::npos) return {};
    size_t e = p + k.size();
    while (e < log.size() && (isalnum((unsigned char)log[e]) || log[e] == '-')) e++;
    return log.substr(p + k.size(), e - p - k.size());
}

std::string parsePreview(const std::string& log) {
    // The server prints "onair-server: <preview>" and then the error line that carries the code; keep everything between.
    const size_t a = log.find("onair-server: ");
    const size_t b = log.find("web_directory is on, but nothing is sent yet");
    if (a == std::string::npos || b == std::string::npos || b <= a) return {};
    std::string s = log.substr(a + 14, b - a - 14);
    const size_t last = s.find_last_not_of(" \n\r\t");
    return last == std::string::npos ? std::string() : s.substr(0, last + 1);
}

bool cleanLine(const std::string& s, size_t maxLen) {
    if (s.size() > maxLen) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return false;
    return true;
}

} // namespace webshare
