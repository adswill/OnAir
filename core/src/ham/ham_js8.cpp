// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham JS8: messages to frames, frames to slot audio, the sender queue and the gated send (ham_js8.h).
#include "dect2/ham/ham_js8.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2::ham {

namespace {
constexpr double kPi = 3.14159265358979323846;
std::string up(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
bool groupOk(const std::string& s) {   // "@ALLCALL", "@HB" ...
    if (s.size() < 2 || s[0] != '@') return false;
    for (size_t i = 1; i < s.size(); i++) if (!isalnum((unsigned char)s[i])) return false;
    return s.size() <= 9;
}
}  // namespace

bool js8Heartbeat(const std::string& call, const std::string& grid, std::vector<js8::TxFrame>& out, std::string& err) {
    out.clear();
    if (!hamCallValid(call)) { err = "enter a valid callsign"; return false; }
    js8::TxFrame f{};
    if (!js8::packHeartbeat(up(call), up(grid).substr(0, 4), f.bits)) { err = "this callsign or grid cannot be sent in a heartbeat"; return false; }
    f.flags = js8::kFirst | js8::kLast;
    out.push_back(f);
    err.clear();
    return true;
}

bool js8Cq(const std::string& call, const std::string& grid, std::vector<js8::TxFrame>& out, std::string& err) {
    out.clear();
    if (!hamCallValid(call)) { err = "enter a valid callsign"; return false; }
    js8::TxFrame f{};
    if (!js8::packCq(up(call), up(grid).substr(0, 4), 0, f.bits)) { err = "this callsign or grid cannot be sent in a CQ"; return false; }
    f.flags = js8::kFirst | js8::kLast;
    out.push_back(f);
    err.clear();
    return true;
}

bool js8Directed(const std::string& from, const std::string& toIn, const std::string& cmd, int num, const std::string& text,
                 std::vector<js8::TxFrame>& out, std::string& err) {
    out.clear();
    const std::string to = up(toIn);
    if (!hamCallValid(from)) { err = "enter a valid callsign"; return false; }
    if (!hamCallValid(to) && !groupOk(to)) { err = "\"" + toIn + "\" is not a callsign or @GROUP"; return false; }
    int code = js8::commandCode(cmd.empty() ? " " : cmd);
    if (code < 0) { err = "unknown JS8 command \"" + cmd + "\""; return false; }
    out = js8::buildMessage(up(from), to, code, num, up(text));
    if (out.empty()) { err = "the message cannot be sent in JS8"; return false; }
    if ((int)out.size() > kJs8MaxFrames) { err = "the message is too long (" + std::to_string(out.size()) + " frames, " + std::to_string(kJs8MaxFrames) + " at most)"; out.clear(); return false; }
    err.clear();
    return true;
}

double js8BwHz(int submode) { return 8 * js8::spec(submode).baud(); }

std::vector<float> js8FrameAudio(const js8::TxFrame& f, int submode, double hz, double rate) {
    const js8::Spec& sp = js8::spec(submode);
    uint8_t b87[87];
    js8::frameBits(f.bits, f.flags, b87);
    const std::vector<int> t = js8::tones(submode, b87);
    const double baud = sp.baud(), dur = sp.durSec();
    const size_t n = (size_t)std::llround(dur * rate);
    std::vector<float> a(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double tt = (double)i / rate;
        const int sym = std::min(js8::kSymbols - 1, (int)(tt * baud));
        ph += 2 * kPi * (hz + t[(size_t)sym] * baud) / rate;
        if (ph > 2 * kPi) ph -= 2 * kPi;
        const double edge = std::min(1.0, std::min(tt, dur - tt) / 0.01);   // 10 ms ramps, as the generator and JS8Call
        a[i] = (float)((0.5 - 0.5 * std::cos(kPi * edge)) * std::sin(ph));
    }
    return a;
}

bool Js8Sender::frameForSlot(double, js8::TxFrame& out) {
    if (q_.empty()) return false;
    out = q_.front();
    q_.pop_front();
    return true;
}

bool js8Send(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gateIn, const js8::TxFrame& f, int submode, double hz, double level, double now, std::string& err) {
    std::vector<float> a = js8FrameAudio(f, submode, hz);
    const double dur = (double)a.size() / kKbdRate;
    HamTxCheck g = gateIn;
    const double bw = js8BwHz(submode);
    g.audioHz = hz;
    g.bwHz = bw;
    if (!tx.sendAudio(g, std::move(a), bw, level, err)) return false;
    duty.add(now, dur);
    return true;
}

}  // namespace dect2::ham
