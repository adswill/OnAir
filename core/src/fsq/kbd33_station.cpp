// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ / IFKP engine side (see kbd33_station.h).
#include "dect2/fsq/kbd33_station.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2 {

namespace {
constexpr double kBase = marine::kBaseRate;      // 24 kHz complex from the front end

// Complex samples through a real low-pass and down by an integer
struct Decim {
    int dec = 1, n = 0, pos = 0, phase = 0;
    std::vector<float> taps;
    std::vector<cf32> hist;
    void configure(int d, double cutoff, int ntaps) {   // cutoff in cycles per input sample
        dec = d; n = ntaps;
        const auto lp = hfdig::lowpass(n, cutoff, 6.0);
        taps.assign(lp.begin(), lp.end());
        hist.assign((size_t)(2 * n), cf32(0.f, 0.f));
        pos = 0; phase = 0;
    }
    bool push(cf32 x, cf32& out) {
        hist[(size_t)pos] = x; hist[(size_t)(pos + n)] = x;
        if (++pos >= n) pos = 0;
        if (++phase < dec) return false;
        phase = 0;
        const cf32* w = &hist[(size_t)pos];
        float re = 0, im = 0;
        for (int k = 0; k < n; k++) { re += taps[(size_t)k] * w[k].real(); im += taps[(size_t)k] * w[k].imag(); }
        out = cf32(re, im);
        return true;
    }
};

const char kTriggers[] = " !#$%&'()*+,-.;<=>?@[\\]^_{|}~";

std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }
bool isCall(const std::string& w) { return w.size() >= 3 && w.size() <= 20 && cw::isCallsign(w); }
}

const char* fsqTriggerMeaning(char c) {
    switch (c) {
    case ' ': return "text";
    case '?': return "asks for the signal report";
    case '*': return "asks for an acknowledgement";
    case '+': return "asks for a file";
    case ';': return "relay this message";
    case '!': return "repeat now";
    case '~': return "repeat in 15 s";
    case '#': return "store this text";
    case '$': return "asks for the heard list";
    case '@': return "asks for the location";
    case '&': return "asks for the message of the day";
    case '^': return "asks for the software version";
    case '%': return "picture follows";
    case '|': return "alert";
    case '>': return "speed up";
    case '<': return "speed down";
    case '[': return "relayed message";
    default: return "";
    }
}

Kbd33Msg fsqParseLine(const std::string& line, const std::string& myCall) {
    Kbd33Msg m;
    std::string s = line;
    // "CALL:" at the start, the sender; two hex digits after the colon are the CRC of the callsign
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    std::string body = s.substr(i);
    const size_t colon = body.find(':');
    if (colon != std::string::npos && colon >= 3 && colon <= 20) {
        const std::string call = body.substr(0, colon);
        bool word = true;
        for (char c : call) if (!(isalnum((unsigned char)c) || c == '/')) word = false;
        if (word && isCall(call)) {
            m.from = upper(call);
            body = body.substr(colon + 1);
            if (body.size() >= 2 && isxdigit((unsigned char)body[0]) && isxdigit((unsigned char)body[1])) {
                std::string h = body.substr(0, 2);
                for (char& c : h) c = (char)tolower((unsigned char)c);
                m.crcOk = h == kbd33::crc8hex(call) || h == kbd33::crc8hex(m.from);
                body = body.substr(2);   // a wrong CRC is still a CRC field
            }
        }
    }
    size_t b = 0;
    while (b < body.size() && body[b] == ' ') b++;
    body = body.substr(b);
    // the addressee: a callsign (or allcall / cqcqcq) followed by a command character
    size_t e = 0;
    while (e < body.size() && body[e] != ' ' && !strchr(kTriggers, body[e])) e++;
    const std::string w = body.substr(0, e);
    std::string lw = w;
    for (char& c : lw) c = (char)tolower((unsigned char)c);
    if (!w.empty() && (lw == "allcall" || lw == "cqcqcq" || isCall(w))) {
        m.to = lw == "allcall" ? "allcall" : lw == "cqcqcq" ? "cqcqcq" : upper(w);
        m.trigger = (e < body.size() && strchr(kTriggers, body[e])) ? body[e] : ' ';
        m.text = body.substr(std::min(body.size(), e + 1));
    } else {
        m.text = body;
    }
    while (!m.text.empty() && m.text.front() == ' ') m.text.erase(0, 1);
    while (!m.text.empty() && m.text.back() == ' ') m.text.pop_back();
    const std::string me = upper(myCall);
    m.toMe = !me.empty() && (m.to == me || m.to == "allcall" || m.to == "cqcqcq");
    return m;
}

struct Kbd33Station::Impl {
    bool ifkp;
    double minRate;
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    std::string myCall;
    Kbd33Telemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    std::atomic<int> speedReq{-1};
    // receiver thread
    Kbd33Telemetry tel;
    double curRate = 0;
    bool running = false;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0; int64_t nPower = 0;
    marine::Front front;
    hfdig::Listen listen;
    std::vector<cf32> base;
    Decim dec24;
    kbd33::Rx rx;
    std::string text;
    std::vector<Kbd33Line> lines;
    std::vector<std::string> calls;
    std::vector<Kbd33Msg> msgs;
    bool lineOpen = false;
    std::string cur;
    double curTime = 0;
    int lastCh = 0;
    uint64_t chars = 0;
    float levelFloorDb = -200;

    Impl(bool i, double mr) : ifkp(i), minRate(mr), rx(i) {
        dec24.configure(3, 3900.0 / kBase, 127);
        rx.onChar = [this](int ch, double t) { putChar(ch, t); };
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        const uint64_t s = tel.seq, ss = tel.specSeq;
        tel = Kbd33Telemetry();
        tel.seq = s; tel.specSeq = ss;
        tel.inputRate = curRate;
        tel.loHz = kbd33::Rx::kLoHz; tel.hiHz = kbd33::Rx::kHiHz;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        running = curRate >= minRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        listen.reset();
        base.clear();
        dec24.configure(3, 3900.0 / kBase, 127);
        rx.reset();
        cur.clear(); lineOpen = false;
    }

    void closeLine() {
        if (!cur.empty() && !ifkp) {
            std::string mc;
            { std::lock_guard<std::mutex> lk(mu); mc = myCall; }
            Kbd33Msg m = fsqParseLine(cur, mc);
            m.timeSec = curTime;
            if (!m.from.empty() || !m.to.empty()) {
                msgs.push_back(m);
                if (msgs.size() > kKbd33MsgMax) msgs.erase(msgs.begin());
            }
        }
        cur.clear();
        lineOpen = false;
    }

    void putChar(int ch, double t) {
        chars++;
        if (ch == 10 && lastCh == 13) { lastCh = ch; return; }
        lastCh = ch;
        if (ch == 13 || ch == 10 || ch == 8) {
            closeLine();
            if (!text.empty() && text.back() != '\n') text += '\n';
        } else {
            std::string u;
            if (ch == 9) ch = ' ';
            if (ch >= 32 && ch <= 126) u = std::string(1, (char)ch);
            else if (ch == 163) u = "\xC2\xA3";
            else if (ch == 176) u = "\xC2\xB0";
            else if (ch == 177) u = "\xC2\xB1";
            else if (ch == 215) u = "\xC3\x97";
            else if (ch == 247) u = "\xC3\xB7";
            else return;
            if (!lineOpen) {
                Kbd33Line l; l.timeSec = t;
                lines.push_back(l);
                if (lines.size() > kKbd33LinesMax) lines.erase(lines.begin());
                lineOpen = true;
                curTime = t;
            }
            lines.back().text += u;
            cur += u;
            text += u;
        }
        if (text.size() > kKbd33TextMax) text.erase(0, text.size() - kKbd33TextMax);
        if (ch == ' ' || ch == 13 || ch == 10) {
            std::string flat = text;
            std::replace(flat.begin(), flat.end(), '\n', ' ');
            calls = cw::callsigns(flat);
        }
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        std::vector<float> sp;
        float fl = 0;
        if (rx.takeSpectrum(sp, fl)) { tel.spectrum = std::move(sp); tel.floorDb = fl; tel.specSeq++; tel.loHz = rx.spectrumLoHz(); tel.hiHz = tel.loHz + (double)tel.spectrum.size() * (kbd33::Rx::kFs / 4096); }
        tel.autoSpeed = speedReq.load() < 0;
        if (rx.locked()) {
            const kbd33::Spec sp2 = kbd33::specOf(ifkp, rx.speed());
            tel.speed = rx.speed();
            tel.state = 2;
            tel.baud = sp2.baud(); tel.spacingHz = sp2.spacingHz; tel.bandHz = sp2.bandHz();
            tel.gridHz = rx.gridHz();
            rx.occupied(tel.occLoHz, tel.occHiHz);
            tel.cfoHz = rx.afcHz();
            tel.snrDb = std::max(-5.f, (float)rx.snrDb());
            tel.quality = (float)rx.quality();
            tel.dataValid = rx.quality() > 0.2;
            tel.toneUse = rx.toneUse();
        } else {
            tel.state = 0; tel.speed = -1; tel.dataValid = false; tel.toneUse.clear(); tel.snrDb = 0; tel.quality = 0;
        }
        tel.chars = chars; tel.blocksOk = chars; tel.blocksBad = rx.droppedSymbols();
        tel.calls = calls;
        for (const Kbd33Msg& m : msgs)   // the sender and the addressee of FSQ lines (the text has them glued to the CRC and the command)
            for (const std::string* c : {&m.from, &m.to})
                if (!c->empty() && *c != "allcall" && *c != "cqcqcq" && std::find(tel.calls.begin(), tel.calls.end(), *c) == tel.calls.end()) tel.calls.push_back(*c);
        tel.text = text; tel.lines = lines; tel.msgs = msgs; tel.syncs = rx.syncs();
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

Kbd33Station::Kbd33Station(bool ifkp, double minRate) : p_(std::make_unique<Impl>(ifkp, minRate)) {}
Kbd33Station::~Kbd33Station() = default;

void Kbd33Station::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void Kbd33Station::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool Kbd33Station::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= p_->minRate - 1; }
void Kbd33Station::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    const std::string t = p_->pub.text;   // the text stays (clearResults)
    auto l = std::move(p_->pub.lines);
    auto m = std::move(p_->pub.msgs);
    p_->pub = Kbd33Telemetry();
    p_->pub.seq = s; p_->pub.text = t; p_->pub.lines = std::move(l); p_->pub.msgs = std::move(m);
}

void Kbd33Station::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.front.setOffsetHz(m.offsetHz);
    }
    if (m.clearReq.exchange(false)) { m.text.clear(); m.lines.clear(); m.calls.clear(); m.msgs.clear(); m.cur.clear(); m.lineOpen = false; m.chars = 0; }
    m.rx.setSpeed(m.speedReq.load());
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.listen.playBase(m.base.data(), m.base.size());
        std::vector<cf32> y8;
        y8.reserve(m.base.size() / 3 + 1);
        for (const cf32& v : m.base) { cf32 o; if (m.dec24.push(v, o)) y8.push_back(o); }
        m.rx.feed(y8.data(), y8.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

void Kbd33Station::clearResults() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.text.clear(); p_->pub.lines.clear(); p_->pub.calls.clear(); p_->pub.msgs.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}
bool Kbd33Station::telemetry(Kbd33Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void Kbd33Station::setSpeed(int s) { p_->speedReq = s; }
void Kbd33Station::setMyCall(const std::string& c) { std::lock_guard<std::mutex> lk(p_->mu); p_->myCall = c; }
void Kbd33Station::setListen(bool on) { p_->listen.on = on; }
bool Kbd33Station::listening() const { return p_->listen.on.load(); }
void Kbd33Station::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void Kbd33Station::setMuted(bool m) { p_->listen.muted = m; }
void Kbd33Station::setSilent(bool s) { p_->listen.silent = s; }
void Kbd33Station::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

} // namespace dect2
