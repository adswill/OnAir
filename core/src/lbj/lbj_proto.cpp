// LBJ fixed fields and single-use, five-second basic/detail pairing.
// Ported from frank-1225/railair-terminal (GPL-3.0-or-later); docs/railway.md.
#include "dect2/lbj/lbj_proto.h"
#include "dect2/railway/railway_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#ifdef _WIN32
#include "dect2/common/platform.h"
#else
#include <iconv.h>
#endif
namespace dect2::lbj {
namespace {
bool digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string trim(std::string s) {
    const auto a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool blank(const std::string& s) { return s.find_first_not_of(" -*?") == std::string::npos; }
std::string unhex(const std::string& numeric) {
    const std::string codes = "0123456789*U -)(";
    std::string out;
    if (numeric.size() % 2) return {};
    for (size_t i = 0; i < numeric.size(); i += 2) {
        auto nibble = [&](char c) { return c == '.' ? size_t(10) : (c == ']' ? size_t(14) : (c == '[' ? size_t(15) : codes.find(c))); };
        const size_t hi = nibble(numeric[i]), lo = nibble(numeric[i + 1]);
        if (hi > 15 || lo > 15) return {};
        out.push_back(char(hi * 16 + lo));
    }
    return out;
}
std::string gbk(std::string bytes) {
    while (!bytes.empty() && (bytes.back() == 0 || (unsigned char)bytes.back() == 255)) bytes.pop_back();
    if (bytes.empty()) return {};
#ifdef _WIN32
    const int n = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (!n) return {};
    std::wstring wide((size_t)n, 0);
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), wide.data(), n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)m, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, out.data(), m, nullptr, nullptr);
#else
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) return {};
    size_t left = bytes.size(), available = bytes.size() * 4 + 4;
    std::string out(available, 0);
    char* in = bytes.data(); char* target = out.data();
    const size_t result = iconv(cd, &in, &left, &target, &available);
    iconv_close(cd);
    if (result == (size_t)-1 || left) return {};
    out.resize(out.size() - available);
#endif
    for (unsigned char c : out) if (c < 32 || c == 127) return {};
    return trim(out);
}
bool recent(double now, double before) { return now >= before && now - before <= 5.0; }
void mergeBasic(const LbjMessage& basic, LbjMessage& detail) {
    detail.trainBase = basic.trainBase; detail.train = detail.prefix + basic.trainBase;
    detail.speedKmh = basic.speedKmh; detail.kmMarker = basic.kmMarker;
    detail.direction = basic.direction; detail.paired = true;
    detail.crcOk = detail.crcOk && basic.crcOk;
    detail.correctedBits += basic.correctedBits;
}
bool basic(const Page& p, LbjSessionState& st, LbjMessage& out) {
    const std::string text = p.digits.substr(0, p.digits.size() >= 65 ? 15 : std::string::npos);
    if (blank(text)) { st.emptyFrames++; return false; }
    std::string train, speed, km;
    if (text.size() == 15) {
        train = trim(text.substr(0, 5)); speed = trim(text.substr(5, 5)); km = trim(text.substr(10, 5));
    } else {
        std::istringstream in(text); std::string extra;
        if (!(in >> train >> speed >> km) || (in >> extra)) return false;
    }
    if (!digits(train) || train.size() > 5) return false;
    out.trainBase = std::to_string(std::atoi(train.c_str())); out.train = out.trainBase;
    out.direction = std::atoi(train.c_str()) % 2 ? "down" : "up";
    if (digits(speed) && speed.size() <= 3 && std::atoi(speed.c_str()) <= 400) out.speedKmh = std::to_string(std::atoi(speed.c_str()));
    else if (!blank(speed)) return false;
    const std::string magnitude = !km.empty() && km[0] == '-' ? km.substr(1) : km;
    if (digits(magnitude) && magnitude.size() <= 5) {
        char b[32]; snprintf(b, sizeof b, "%.1f", std::atoi(km.c_str()) / 10.0); out.kmMarker = b;
    } else if (!blank(km)) return false;
    return true;
}
bool detail(const Page& p, LbjMessage& out) {
    if (p.missing || p.digits.size() < 47) return false;
    const std::string& s = p.digits;
    std::string prefix = unhex(s.substr(0, 4));
    for (unsigned char c : prefix) if (c && (c < 32 || c > 126)) { prefix.clear(); break; }
    prefix.erase(std::remove(prefix.begin(), prefix.end(), '\0'), prefix.end()); out.prefix = trim(prefix);
    const std::string code = s.substr(4, 4), serial = s.substr(8, 4);
    if (digits(code)) out.locoCode = code;
    if (digits(serial)) out.locoSerial = serial;
    if (!out.locoCode.empty() && !out.locoSerial.empty()) out.loco = railway::lbjLocomotive(code + serial);
    out.cabEnd = s.substr(12, 2); out.route = gbk(unhex(s.substr(14, 16))); out.unknownTail = s.substr(47, 3);
    if (digits(s.substr(30, 17))) {
        const double lonMin = std::atoi(s.substr(33, 6).c_str()) / 10000.0, latMin = std::atoi(s.substr(41, 6).c_str()) / 10000.0;
        const double lon = std::atoi(s.substr(30, 3).c_str()) + lonMin / 60, lat = std::atoi(s.substr(39, 2).c_str()) + latMin / 60;
        if (lonMin < 60 && latMin < 60 && lon >= 70 && lon <= 140 && lat > 0 && lat <= 60) {
            out.longitude = lon; out.latitude = lat; out.positionValid = true;
        }
    }
    out.detailed = true; return true;
}
} // namespace
bool isLbjAddress(uint32_t ric) {
    return ric == kAddrBasic || ric == kAddrBasicAlt || ric == kAddrDetail || ric == kAddrDetailAlt || ric == kAddrTime;
}
bool parsePage(const Page& p, double t, LbjSessionState& st, LbjMessage& out) {
    out = LbjMessage(); out.timeSec = t; out.address = p.ric; out.correctedBits = p.fixed;
    out.crcOk = !p.missing && p.digits.find('?') == std::string::npos && p.fixed <= 2;
    if (p.ric == kAddrTime) {
        out.timeSync = true;
        if (p.digits.size() >= 5 && digits(p.digits.substr(1, 4))) {
            const int hour = std::atoi(p.digits.substr(1, 2).c_str()), minute = std::atoi(p.digits.substr(3, 2).c_str());
            if (hour < 24 && minute < 60) out.networkTime = p.digits.substr(1, 2) + ":" + p.digits.substr(3, 2);
        }
        return true;
    }
    if (p.ric == kAddrBasic || p.ric == kAddrBasicAlt) {
        const auto emptyBefore = st.emptyFrames;
        if (!basic(p, st, out)) { st.basicSec = st.detailSec = -1e9; if (st.emptyFrames == emptyBefore) st.rejected++; return false; }
        if (p.digits.size() < 65 && recent(t, st.detailSec)) {
            LbjMessage merged = st.pendingDetail; mergeBasic(out, merged); merged.timeSec = t; out = std::move(merged); st.basicSec = -1e9;
        } else { st.pendingBasic = out; st.basicSec = t; }
        st.detailSec = -1e9;
        if (p.digits.size() >= 65) {
            Page q = p; q.ric = kAddrDetail; q.digits = p.digits.substr(p.digits.size() - 50);
            LbjMessage extended; if (parsePage(q, t, st, extended)) out = std::move(extended);
        }
        return true;
    }
    if (p.ric == kAddrDetail || p.ric == kAddrDetailAlt) {
        if (!detail(p, out)) { st.basicSec = st.detailSec = -1e9; st.rejected++; return false; }
        if (recent(t, st.basicSec)) { mergeBasic(st.pendingBasic, out); st.detailSec = -1e9; }
        else { st.pendingDetail = out; st.detailSec = t; }
        st.basicSec = -1e9; return true;
    }
    st.basicSec = st.detailSec = -1e9; return false;
}
std::string formatBasic(const std::string& train, int speedKmh, int kmTenths) {
    char b[48]; snprintf(b, sizeof b, "%-5s %3d %5d", train.c_str(), speedKmh, kmTenths); return b;
}
} // namespace dect2::lbj
