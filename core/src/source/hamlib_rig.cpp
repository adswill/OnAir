// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/source/hamlib_rig.h"
#include "dect2/source/hamlib_link.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace dect2 {

namespace {
std::mutex gMu;
std::vector<HamlibRig> gRigs;
std::mutex gStateMu;
HamlibRigState gState;

std::string esc(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        if (c == ',' || c == ';' || c == '%' || c < 32) {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            r += b;
        } else
            r += (char)c;
    }
    return r;
}
std::string unesc(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            r += s[i];
    }
    return r;
}
std::vector<std::string> splitFields(const std::string& item) {
    std::vector<std::string> f;
    std::string cur;
    for (size_t i = 0; i < item.size(); i++) {
        if (item[i] == '%' && i + 2 < item.size()) {
            cur += (char)strtol(item.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
            continue;
        }
        if (item[i] == ',') {
            f.push_back(cur);
            cur.clear();
        } else
            cur += item[i];
    }
    f.push_back(cur);
    return f;
}
} // namespace

std::string hamlibRigsToString(const std::vector<HamlibRig>& v) {
    std::string s;
    for (const auto& r : v) {
        if (r.model <= 0 || r.path.empty()) continue;
        if (!s.empty()) s += ';';
        s += std::to_string(r.model) + ',' + esc(r.path) + ',' + esc(r.label) + ',' + esc(r.audioId) + ',' + (r.baud > 0 ? std::to_string(r.baud) : std::string());
        const bool tx = r.pttType != 0 || !r.pttPath.empty() || !r.dataMode.empty() || !r.audioOutId.empty() || r.split;
        if (tx)   // only written when set, so a plain receive rig keeps the short form older versions read
            s += ',' + std::to_string(std::clamp(r.pttType, 0, 3)) + ',' + esc(r.pttPath) + ',' + esc(r.dataMode) + ',' + esc(r.audioOutId) + ',' + (r.split ? "1" : "0");
    }
    return s;
}

std::vector<HamlibRig> hamlibRigsFromString(const std::string& s) {
    std::vector<HamlibRig> out;
    size_t b = 0;
    while (b < s.size()) {
        size_t e = s.find(';', b);
        const std::string item = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
        b = e == std::string::npos ? s.size() : e + 1;
        if (item.empty()) continue;
        const auto f = splitFields(item);
        if (f.empty()) continue;
        HamlibRig r;
        r.model = atoi(f[0].c_str());
        if (r.model <= 0) continue;
        r.path = f.size() > 1 ? unesc(f[1]) : "";
        if (r.path.empty()) continue;
        r.label = f.size() > 2 ? unesc(f[2]) : "";
        r.audioId = f.size() > 3 ? unesc(f[3]) : "";
        r.baud = f.size() > 4 ? std::max(0, atoi(f[4].c_str())) : 0;
        r.pttType = f.size() > 5 ? std::clamp(atoi(f[5].c_str()), 0, 3) : 0;
        r.pttPath = f.size() > 6 ? unesc(f[6]) : "";
        r.dataMode = f.size() > 7 ? unesc(f[7]) : "";
        r.audioOutId = f.size() > 8 ? unesc(f[8]) : "";
        r.split = f.size() > 9 && f[9] == "1";
        out.push_back(std::move(r));
    }
    return out;
}

void setHamlibRigState(const HamlibRigState& st) {
    std::lock_guard<std::mutex> lk(gStateMu);
    gState = st;
}

HamlibRigState hamlibRigState() {
    std::lock_guard<std::mutex> lk(gStateMu);
    return gState;
}

void setHamlibRigs(const std::vector<HamlibRig>& v) {
    std::lock_guard<std::mutex> lk(gMu);
    gRigs = v;
}

std::vector<HamlibRig> hamlibRigs() {
    std::lock_guard<std::mutex> lk(gMu);
    return gRigs;
}

std::string hamlibRigSerial(int model, const std::string& path, int baud) {
    return std::to_string(model) + (baud > 0 ? "@" + std::to_string(baud) : std::string()) + ":" + path;
}

bool parseHamlibSerial(const std::string& serial, int& model, std::string& path, int& baud) {
    const size_t c = serial.find(':');
    if (c == std::string::npos || c == 0) return false;
    std::string head = serial.substr(0, c);
    baud = 0;
    const size_t at = head.find('@');
    if (at != std::string::npos) {
        baud = std::max(0, atoi(head.substr(at + 1).c_str()));
        head.resize(at);
    }
    model = atoi(head.c_str());
    path = serial.substr(c + 1);
    return model > 0 && !path.empty();
}

bool parseHamlibSerial(const std::string& serial, int& model, std::string& path) {
    int baud = 0;
    return parseHamlibSerial(serial, model, path, baud);
}

const std::vector<std::string>& hamlibModeChoices() {
    static const std::vector<std::string> v = {"USB", "LSB", "AM", "FM", "WFM", "CW", "CWR", "PKTUSB", "PKTLSB", "PKTFM", "RTTY"};
    return v;
}

std::string hamlibRigDeviceName(const HamlibRig& r) {
    if (!r.label.empty()) return "Hamlib: " + r.label;
    const std::string tail = r.path.size() > 16 ? r.path.substr(r.path.size() - 16) : r.path;
    char b[120];
    snprintf(b, sizeof b, "Hamlib rig %d (%s)", r.model, tail.c_str());
    return b;
}

std::vector<DeviceInfo> hamlibRigDevices(const std::vector<HamlibRig>& v) {
    std::vector<DeviceInfo> out;
    for (const auto& r : v) {
        DeviceInfo d;
        d.kind = DeviceInfo::Hamlib;
        d.board = "hamlib";
        d.serial = hamlibRigSerial(r.model, r.path, r.baud);
        d.nativeArgs = r.audioId;
        d.name = hamlibRigDeviceName(r);
        d.minRateHz = 8000;
        d.maxRateHz = 48000;
        d.steadyRateHz = 48000;
        d.minFreqHz = 10e3;
        d.maxFreqHz = 30e9;
        d.canTransmit = hamlibSupported();
        d.txMinRateHz = 48000;
        d.txMaxRateHz = 48000;
        d.txGainNote = "Hamlib: PTT and frequency only; RF audio is not sent through Hamlib";
        d.settings = {
            choiceSetting("hamlib_mode", "Mode", "The rig mode sent over CAT when tuning or transmitting. A mode you turn on the rig itself shows here.",
                          hamlibModeChoices(), {"USB", "LSB", "AM", "FM (narrow)", "FM (wide)", "CW", "CW reverse", "packet USB", "packet LSB", "packet FM", "RTTY"}, "USB", false),
            choiceSetting("audio_iq", "Audio IQ", "How the sound card connected for IQ or speaker audio is interpreted (same as a sound-card source).",
                          {"0", "1", "2", "3"}, {"I/Q stereo", "USB audio", "LSB audio", "Mono real"}, "1", false),
        };
        out.push_back(std::move(d));
    }
    return out;
}

#ifndef DECT2_HAVE_HAMLIB
bool hamlibSupported() { return false; }
std::unique_ptr<IqSource> makeHamlibSource(const DeviceInfo&) { return nullptr; }
std::unique_ptr<TxSink> makeHamlibTxSink(const DeviceInfo&) { return nullptr; }
struct HamlibLink::Impl {};
HamlibLink::HamlibLink() : p_(new Impl) {}
HamlibLink::~HamlibLink() = default;
bool HamlibLink::open(const HamlibConn&, std::string& err) { err = "this build has no Hamlib"; return false; }
void HamlibLink::close() {}
bool HamlibLink::isOpen() const { return false; }
bool HamlibLink::setFreq(double, std::string& err) { err = "this build has no Hamlib"; return false; }
bool HamlibLink::getFreq(double&, std::string& err) { err = "this build has no Hamlib"; return false; }
bool HamlibLink::setMode(const std::string&, int, std::string& err) { err = "this build has no Hamlib"; return false; }
bool HamlibLink::getMode(std::string&, int&, std::string& err) { err = "this build has no Hamlib"; return false; }
bool HamlibLink::setPtt(bool, std::string& err) { err = "this build has no Hamlib"; return false; }
bool HamlibLink::setSplit(bool, double, std::string& err) { err = "this build has no Hamlib"; return false; }
bool HamlibLink::getSplitFreq(double&, std::string& err) { err = "this build has no Hamlib"; return false; }
std::string HamlibLink::info() const { return {}; }
bool hamlibModeKnown(const std::string&) { return false; }
std::string hamlibCanonicalMode(const std::string&) { return {}; }
bool hamlibSameMode(const std::string&, const std::string&) { return false; }
#endif

} // namespace dect2
