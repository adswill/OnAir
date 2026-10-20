// CIR/FFSK protocol port from railair-terminal. See docs/railway.md.
#include "dect2/railway/railway_proto.h"
#include "dect2/common/bits.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace dect2::railway {
namespace {
unsigned le(const uint8_t* p, size_t n) {
    unsigned v = 0; for (size_t i = 0; i < n; i++) v |= unsigned(p[i]) << (8 * i); return v;
}
std::string hex(const uint8_t* p, size_t n) {
    std::string s; const char* h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 15]; } return s;
}
std::string bcd(const uint8_t* p, size_t n) {
    const char* chars = "0123456789?B -][";
    std::string s; for (size_t i = 0; i < n; i++) { s += chars[p[i] >> 4]; s += chars[p[i] & 15]; } return s;
}
bool digits(const std::string& s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}
std::string padded(unsigned n, int width) {
    char b[32]; snprintf(b, sizeof b, "%0*u", width, n); return b;
}
const char* action(unsigned code) {
    switch (code) {
    case 0x24: return "Vent"; case 0x25: return "Vent response";
    case 0x91: return "Pressure query"; case 0x92: return "Pressure response";
    case 0x83: return "Disconnect"; case 0x84: return "Disconnect response";
    case 0xB5: return "Connect"; case 0xB6: return "Connect response";
    case 0x30: return "Tail pressure alarm"; case 0x31: return "Pressure alarm response";
    case 0xB0: return "Tail low battery alarm"; case 0xB1: return "Low battery response";
    case 0x5B: return "Tail connection request";
    case 0x93: return "Automatic pressure query"; case 0x94: return "Automatic pressure response";
    case 0xA1: return "Clock calibration"; case 0xA2: return "Clock calibration response";
    case 0xA3: return "Pressure calibration"; case 0xA4: return "Pressure calibration response";
    default: return nullptr;
    }
}
bool klw(const uint8_t* p, size_t n, Message& m) {
    std::vector<uint8_t> loco;
    const uint8_t *command = nullptr, *parameter = nullptr;
    if (n == 6) { loco.assign(p, p + 2); command = p + 2; parameter = p + 4; }
    else if (n == 11 || (n == 17 && (p[5] == 0xA1 || p[5] == 0xA2))) {
        loco.assign(p, p + 4); command = p + 4; parameter = p + 9;
        if (p[6] == 0xAA && p[7] == 0xAA && p[8] == 0xA0 && n == 11) {
            loco.resize(2); m.extra = hex(p + 2, 2) + hex(p + 6, 3);
        } else m.tailId = bcd(p + 6, 3);
        if (n == 17) {
            const auto s = bcd(p + 11, 6);
            if (digits(s)) {
                auto number = [&](int at) { return std::atoi(s.substr((size_t)at, 2).c_str()); };
                const std::chrono::year_month_day date{std::chrono::year{2000 + number(0)}, std::chrono::month{unsigned(number(2))}, std::chrono::day{unsigned(number(4))}};
                if (date.ok() && number(6) < 24 && number(8) < 60 && number(10) < 60)
                    m.clockSync = "20" + s.substr(0, 2) + "-" + s.substr(2, 2) + "-" + s.substr(4, 2) + "T" + s.substr(6, 2) + ":" + s.substr(8, 2) + ":" + s.substr(10, 2);
            }
        }
    } else if (n == 15) {
        loco = {p[7], p[8], p[0], p[1]}; command = p + 2; parameter = p + 4;
        const uint8_t id[] = {p[6], p[9], p[10]}; m.tailId = bcd(id, 3); m.extra = hex(p + 11, 4);
    } else return false;
    const std::string raw = bcd(loco.data(), loco.size());
    if (raw.size() == 8 && digits(raw)) {
        m.locoCode = raw.substr(0, 3); m.locoSerial = raw.substr(4);
        auto model = locomotiveModel((unsigned)std::atoi(m.locoCode.c_str()));
        m.locomotive = (model.empty() ? m.locoCode : model) + "-" + m.locoSerial;
    } else m.locomotive = raw;
    m.actionCode = command[1]; m.commandReserved = command[0];
    if (const char* label = action(m.actionCode)) m.action = label;
    else m.action = "Unknown 0x" + hex(command + 1, 1);
    if (!(parameter[0] == 255 && (parameter[1] == 255 || parameter[1] == 0xA5))) m.parameter = bcd(parameter, 2);
    const bool pressure = m.actionCode == 0x91 || m.actionCode == 0x92 || m.actionCode == 0x30 || m.actionCode == 0x31 || m.actionCode == 0x93 || m.actionCode == 0x94 || m.actionCode == 0xA3 || m.actionCode == 0xA4;
    if (pressure && digits(m.parameter)) m.pressureKpa = std::atoi(m.parameter.c_str());
    return true;
}
uint32_t syndrome(uint32_t word) {
    for (int i = 15; i >= 0; i--) if (word & (1u << (i + 10))) word ^= 0x5B9u << i;
    return word;
}
const auto& errors() {
    static const auto table = [] {
        std::array<std::vector<uint32_t>, 1024> t;
        t[0].push_back(0);
        for (int i = 0; i < 26; i++) {
            const uint32_t a = 1u << i; t[syndrome(a)].push_back(a);
            for (int j = 0; j < i; j++) { const uint32_t b = a | (1u << j); t[syndrome(b)].push_back(b); }
        }
        return t;
    }();
    return table;
}
} // namespace
uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= uint16_t(p[i]) << 8;
        for (int k = 0; k < 8; k++) crc = uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
bool parseFrame(const std::vector<uint8_t>& f, Message& out) {
    out = Message();
    if (f.size() < 13 || f[0] != 0x0C || f.size() != size_t(f[1]) + 2 || f.size() != size_t(f[10]) + 13 || f[7] != 0x1F || f[8] != 0x8C) return false;
    if (crc16(f.data(), f.size() - 2) != (unsigned(f[f.size() - 2]) << 8 | f.back())) return false;
    Message m;
    const uint8_t* p = f.data() + 11;
    if (f[2] == 0 && f[3] == 0x3F && f[4] == 0x1F && f[5] == 0 && f[6] == 0 && f[9] == 0x30 && f[10] == 32) {
        m.protocol = Protocol::Tdcs;
        m.signalType = p[0]; m.semaphoreType = p[1]; m.locomotiveRole = p[2] & 3;
        for (int i = 3; i < 7; i++) { if (p[i] && p[i] != ' ') { if (p[i] < 32 || p[i] > 126) return false; m.train += char(p[i]); } }
        m.train += std::to_string(le(p + 7, 3)); m.locoCode = padded(le(p + 10, 2), 3); m.locoSerial = padded(le(p + 12, 2), 4);
        auto model = locomotiveModel(le(p + 10, 2)); m.locomotive = (model.empty() ? m.locoCode : model) + "-" + m.locoSerial;
        const unsigned mileage = le(p + 14, 3);
        m.testFrame = mileage == 9999888 || mileage == 8888888;
        m.kilometerValid = !m.testFrame && mileage != 0xFFFFFF && mileage != 9999999;
        if (m.kilometerValid) { m.kilometer = (mileage & 0x3FFFFF) / 1000.0 * ((mileage & 0x800000) ? -1 : 1); m.increasingMileage = (mileage & 0x400000) != 0; }
        m.speedKmh = le(p + 17, 3) & 0x3FF; m.weightTonnes = le(p + 20, 2); m.length = le(p + 22, 2) / 10.0;
        m.carCount = p[24]; m.station = le(p + 25, 2); m.track = p[27]; m.actualTrack = p[28]; m.driver = le(p + 29, 3);
    } else if (f[2] == 0x41 && f[3] == 0x3F && f[4] == 0x1F && f[5] == 0 && f[6] <= 1 && f[9] == 0xA5) {
        if (!klw(p, f[10], m)) return false;
    } else return false;
    m.raw = f; out = std::move(m); return true;
}
void FrameFinder::feed(bool bit, std::vector<Message>& out) {
    shift_ = (shift_ << 1) | unsigned(bit); seen_ = std::min(32u, seen_ + 1);
    if (!active_) {
        if (seen_ < 32) return;
        const int e = dect2::bits::popcount(shift_ ^ 0x0DD4259Fu);
        if (e > 2 && e < 30) return;
        active_ = true; inverted_ = e >= 30; word_ = bits_ = 0; candidates_ = {Candidate{}}; syncs++; return;
    }
    word_ = (word_ << 1) | unsigned(bit ^ inverted_);
    if (++bits_ < 26) return;
    std::map<std::vector<uint8_t>, int> pending;
    std::map<std::vector<uint8_t>, Message> valid;
    for (const auto& previous : candidates_) for (uint32_t mask : errors()[syndrome(word_)]) {
        const unsigned value = (word_ ^ mask) >> 10;
        auto data = previous.bytes; data.push_back(uint8_t(value >> 8)); data.push_back(uint8_t(value));
        const size_t expected = size_t(data[1]) + 2;
        if (data[0] != 0x0C || expected < 13 || expected > 257) continue;
        if (data.size() >= 12 && (data[1] != data[10] + 11 || data[7] != 0x1F || data[8] != 0x8C)) continue;
        const int corrected = previous.corrected + dect2::bits::popcount(mask);
        if (data.size() < expected) {
            auto [it, inserted] = pending.emplace(data, corrected); if (!inserted) it->second = std::min(it->second, corrected);
        } else {
            data.resize(expected); Message m;
            if (parseFrame(data, m)) {
                m.correctedBits = corrected;
                auto it = valid.find(data); if (it == valid.end() || corrected < it->second.correctedBits) valid[data] = std::move(m);
            } else crcFailures++;
        }
    }
    word_ = bits_ = 0;
    if (!valid.empty()) {
        if (valid.size() == 1) out.push_back(std::move(valid.begin()->second)); else ambiguous++;
        active_ = false; candidates_.clear();
    } else if (pending.empty() || pending.size() > 64) { rejected++; active_ = false; candidates_.clear(); }
    else { candidates_.clear(); for (auto& [data, corrected] : pending) candidates_.push_back({data, corrected}); }
}
} // namespace dect2::railway
