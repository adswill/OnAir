// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME receiver: see same_rx.h.
#include "dect2/same/same_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/packet/packet_demod.h"
#include "dect2/same/same_proto.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <mutex>
#include "../audio/ham_listen.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace same {

const Channel* channels(int& count) {
    static const Channel c[] = {{"WX1", 162.400}, {"WX2", 162.425}, {"WX3", 162.450}, {"WX4", 162.475}, {"WX5", 162.500}, {"WX6", 162.525}, {"WX7", 162.550}};
    count = 7;
    return c;
}

std::string vote(const std::vector<std::string>& bursts, int* fixed, int* agree) {
    if (fixed) *fixed = 0;
    if (agree) *agree = 0;
    // the most common length (the first of equals)
    size_t bestLen = 0; int bestN = 0;
    for (const auto& b : bursts) {
        int n = 0;
        for (const auto& c : bursts) if (c.size() == b.size()) n++;
        if (n > bestN) { bestN = n; bestLen = b.size(); }
    }
    if (bestN < 2 || bestLen == 0) return "";
    std::vector<const std::string*> v;
    for (const auto& b : bursts) if (b.size() == bestLen) v.push_back(&b);
    std::string out(bestLen, '\0');
    for (size_t i = 0; i < bestLen; i++) {
        if (v.size() == 2) {   // no majority: where they differ, the one that is a character the format allows
            const uint8_t a = (uint8_t)(*v[0])[i], b = (uint8_t)(*v[1])[i];
            out[i] = (char)(a == b || validChar(a) || !validChar(b) ? a : b);
            continue;
        }
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            int ones = 0;
            for (const auto* s : v) ones += ((uint8_t)(*s)[i] >> bit) & 1;
            if (2 * ones > (int)v.size() || (2 * ones == (int)v.size() && (((uint8_t)(*v[0])[i] >> bit) & 1))) byte |= (uint8_t)(1u << bit);
        }
        out[i] = (char)byte;
    }
    int fx = 0, ag = 0;
    for (const auto* s : v) {
        int d = 0;
        for (size_t i = 0; i < bestLen; i++) d += __builtin_popcount((unsigned)((uint8_t)(*s)[i] ^ (uint8_t)out[i]));
        fx += d;
        if (!d) ag++;
    }
    if (fixed) *fixed = fx;
    if (agree) *agree = ag;
    return out;
}

} // namespace same

namespace {

using pktdemod::kMidRate;
constexpr double kBaud = 520.0 + 5.0 / 6.0;   // 520.8333
constexpr double kMark = 2083.0 + 1.0 / 3.0, kSpace = 1562.5;
constexpr double kRate = 48000.0;
constexpr int kWin = 92;                      // one bit is 92.16 samples
constexpr double kToneHz = 1050.0;
constexpr int kToneBlock = 2400;              // 50 ms at 48 kHz
constexpr size_t kMaxBurst = 300;
constexpr double kGroupGapSec = 2.2;          // repeats are about one second apart
constexpr int kPreambleMin = 2;               // aligned 0xAB bytes that start a burst

struct BitDemod {
    std::vector<float> mr, mi, sr, si, ext;
    float ph = -0.5f;
    bool lev = false, prevLev = false;
    float inertia = 0.75f, inertiaLocked = 0.85f;
    bool locked = false;
    void init() {
        using dect2::math::kPi;
        mr.resize(kWin); mi.resize(kWin); sr.resize(kWin); si.resize(kWin);
        for (int t = 0; t < kWin; t++) {
            const int k = kWin - 1 - t;
            mr[(size_t)t] = (float)std::cos(2 * kPi * kMark * k / kRate); mi[(size_t)t] = (float)std::sin(2 * kPi * kMark * k / kRate);
            sr[(size_t)t] = (float)std::cos(2 * kPi * kSpace * k / kRate); si[(size_t)t] = (float)std::sin(2 * kPi * kSpace * k / kRate);
        }
        reset();
    }
    void reset() { ext.assign(kWin - 1, 0.f); ph = -0.5f; lev = prevLev = false; locked = false; }
    // calls bit(level, dominance) once per bit
    template <class F> void process(const float* x, size_t n, F&& bit) {
        const size_t h = kWin - 1;
        ext.resize(h + n);
        std::copy(x, x + n, ext.begin() + (ptrdiff_t)h);
        const float step = (float)(kBaud / kRate);
        for (size_t i = 0; i < n; i++) {
            const float* p = &ext[i];
            float a = 0, b = 0, c = 0, d = 0;
            for (int t = 0; t < kWin; t++) {
                const float v = p[t];
                a += v * mr[(size_t)t]; b += v * mi[(size_t)t];
                c += v * sr[(size_t)t]; d += v * si[(size_t)t];
            }
            const float em = a * a + b * b, es = c * c + d * d;
            lev = em > es;
            if (lev != prevLev) ph *= locked ? inertiaLocked : inertia;
            prevLev = lev;
            ph += step;
            if (ph >= 0.5f) {
                ph -= 1.f;
                bit(lev ? 1 : 0, (em + es) > 0 ? std::fabs(em - es) / (em + es) : 0.f);
            }
        }
        std::copy(ext.end() - (ptrdiff_t)h, ext.end(), ext.begin());
        ext.resize(h);
    }
};

struct Burst { std::string bytes; bool eom = false; double startSec = 0, endSec = 0; float snr = 0; double cfo = 0; };

int hamming4(const std::string& s, const char* ref) {   // matching bytes of the first four
    int m = 0;
    for (int i = 0; i < 4 && i < (int)s.size(); i++) if (s[(size_t)i] == ref[i]) m++;
    return m;
}

} // namespace

struct SameReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0;
    bool isReady = false;
    pktdemod::FmFront fe;
    hfdig::Listen listen;
    dmr::Fir<float> audio48, audio8;
    std::vector<float> a48, a8;
    BitDemod dem;
    int country = 0;

    // bit / byte layer
    enum { Idle, Pre, Msg } st = Idle;
    uint8_t reg = 0;
    int sinceAb = 100, preRun = 0, bitsInByte = 0, nPre = 0, invalidRun = 0;
    std::string cur;
    int64_t n48 = 0;
    double burstStart = 0;
    // vote groups
    std::vector<Burst> group;
    // tone
    double tbuf = 0; int tn = 0; double ts1 = 0, ts2 = 0, tsum = 0;
    int tonePresent = 0, toneAbsent = 0;
    bool toneOn = false; double toneStart = 0, toneLast = 0; float toneLevel = -200;
    double lastToneEnd = -1e9;
    // results
    std::deque<SameAlert> alerts;     // oldest first
    std::deque<SameTone> tones;
    SameTelemetry t, pub;
    uint64_t seq = 0;
    int64_t nextReport = 0;
    double lastNow = 0;
    int64_t lastState = -1;
    double lastAlertSec = -1e9;
    uint64_t okCount = 0, badCount = 0, burstCount = 0;

    double now() const { return (double)n48 / kRate; }
    void say(const std::string& s) { if (log) log(s); }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        fe.chanPass2 = 4500.0; fe.chanStop2 = fe.chanPass2 * 1.8;   // 4.0: the 520 baud tones need much less than the 9 kHz channel (docs/SENSITIVITY-4.0-A3.md)
        if (!fe.configure(fs, sameTuning().minSampleRate)) return;
        audio48.design(dmr::lowpassTaps(3300.0, 8000.0, kMidRate, 50, 9), 2);
        audio8.design(dmr::lowpassTaps(3200.0, 800.0, 48000.0, 50, 9), 6);
        dem.init();
        isReady = true;
        resetState();
    }

    void shiftTimes(double dt) {
        for (auto& a : alerts) a.timeSec += dt;
        for (auto& x : tones) x.timeSec += dt;
    }

    void resetState() {
        const double off = fe.offsetHz;
        fe.reset(); fe.offsetHz = off;
        audio48.reset(); audio8.reset(); listen.reset();
        dem.reset();
        st = Idle; reg = 0; sinceAb = 100; preRun = 0; bitsInByte = 0; nPre = 0; invalidRun = 0; cur.clear();
        group.clear();
        tn = 0; ts1 = ts2 = tsum = 0; tonePresent = toneAbsent = 0; toneOn = false; lastToneEnd = -1e9;
        shiftTimes(-lastNow);
        lastNow = 0;
        n48 = 0; nextReport = 0;
        lastAlertSec = -1e9;
        const uint64_t s = seq;
        t = SameTelemetry();
        t.inputRate = fs;
        lastState = -1;
        std::lock_guard<std::mutex> lk(telMu);
        const uint64_t ps = pub.seq;
        pub = SameTelemetry();
        pub.seq = std::max(s, ps);
        seq = pub.seq;
        pub.inputRate = fs;
        fillLists(pub);
        pub.seq = ++seq;
        say("SAME: listening for weather radio alerts");
    }

    void fillLists(SameTelemetry& o) const {
        o.alerts.assign(alerts.rbegin(), alerts.rend());
        o.tones.assign(tones.rbegin(), tones.rend());
        o.country = country;
        o.blocksOk = okCount; o.blocksBad = badCount; o.bursts = burstCount;
    }
    void republish() {
        std::lock_guard<std::mutex> lk(telMu);
        fillLists(pub);
        pub.seq = ++seq;
    }

    // ---- bursts
    void endBurst() {
        const std::string raw = cur;
        st = Idle; preRun = 0; sinceAb = 100; bitsInByte = 0; nPre = 0; invalidRun = 0; cur.clear();
        dem.locked = false;
        if (raw.size() < 4) return;
        Burst b;
        const int mz = hamming4(raw, "ZCZC"), mn = hamming4(raw, "NNNN");
        if (mn >= 2 && mn > mz) { b.eom = true; b.bytes = raw.substr(0, 4); }
        else if (mz >= 2) {
            b.bytes = same::trimMessage(raw);
            if (b.bytes.empty() || b.bytes.compare(0, 4, "ZCZC") != 0) {   // damaged start or structure: the raw bytes without the noise at the end
                b.bytes = raw;
                while (!b.bytes.empty() && !same::validChar((uint8_t)b.bytes.back())) b.bytes.pop_back();
            }
        } else return;                                          // noise that happened to look like a preamble
        b.startSec = burstStart; b.endSec = now();
        b.snr = (float)fe.snrEma; b.cfo = fe.cfoHz();
        burstCount++;
        if (!group.empty() && (group.back().eom != b.eom || b.startSec - group.back().endSec > kGroupGapSec)) finishGroup();
        group.push_back(b);
        if (group.size() >= 3) finishGroup();
    }

    void onByte(uint8_t c) {
        if (st == Pre) {
            if (__builtin_popcount((unsigned)(c ^ 0xAB)) <= 1 && nPre < 40) { nPre++; return; }
            st = Msg; cur.clear(); invalidRun = 0;
            dem.locked = true;
        }
        cur += (char)c;
        if (same::validChar(c)) invalidRun = 0; else invalidRun++;
        if (invalidRun >= 4 || cur.size() >= kMaxBurst) {
            while (!cur.empty() && !same::validChar((uint8_t)cur.back())) cur.pop_back();
            endBurst();
        }
    }

    void onBit(int bit, float dom) {
        (void)dom;
        reg = (uint8_t)((reg >> 1) | (bit << 7));
        if (st == Idle) {
            sinceAb++;
            if (reg == 0xAB) {
                preRun = sinceAb == 8 ? preRun + 1 : 1;
                sinceAb = 0;
                if (preRun >= kPreambleMin) { st = Pre; bitsInByte = 0; nPre = preRun; burstStart = now() - preRun * 8.0 / kBaud; }
            }
            return;
        }
        if (++bitsInByte == 8) { bitsInByte = 0; onByte(reg); }
    }

    // ---- messages
    void addAlert(SameAlert a) {
        a.wall = (int64_t)std::time(nullptr);
        std::tm g {};
        const time_t tt = (time_t)a.wall;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        a.year = g.tm_year + 1900;
        a.timeSec = now();
        if (a.valid) okCount++; else badCount++;
        if (!a.eom && a.valid) {
            say("SAME: " + a.msg.event + (a.msg.eventName.empty() ? "" : " (" + a.msg.eventName + ")") + " from " + a.msg.org + ", " + std::to_string(a.msg.locs.size()) + " location(s), " +
                std::to_string(a.agreeing) + " of " + std::to_string(a.bursts) + " repeats agree");
        } else if (a.eom) say("SAME: end of message");
        else say("SAME: a damaged message (" + a.err + ")");
        lastAlertSec = a.timeSec;
        alerts.push_back(std::move(a));
        while (alerts.size() > kSameAlertMax) alerts.pop_front();
        republish();
    }

    void finishGroup() {
        if (group.empty()) return;
        std::vector<Burst> g;
        g.swap(group);
        SameAlert a;
        a.bursts = (int)g.size();
        a.snrDb = g.back().snr; a.cfoHz = g.back().cfo;
        std::vector<std::string> v;
        for (const auto& b : g) v.push_back(b.bytes);
        if (g.front().eom) {
            int ok = 0;
            for (const auto& b : g) if (b.bytes == "NNNN") ok++;
            a.eom = true;
            a.raw = "NNNN";
            a.valid = ok >= 1 || g.size() >= 2;
            a.agreeing = ok;
            a.msg = same::parse("NNNN");
            if (!a.valid) a.err = "damaged end of message";
            addAlert(std::move(a));
            return;
        }
        int fixed = 0, agree = 0;
        const std::string voted = same::vote(v, &fixed, &agree);
        same::Message m = voted.empty() ? same::Message() : same::parse(voted, (same::Country)country);
        if (m.valid) { a.agreeing = agree; a.bitsFixed = fixed; a.raw = voted; }
        else {
            // the vote did not give a message: a repeat that is whole by itself (every field checked) is better than nothing
            bool found = false;
            for (const auto& b : g) {
                same::Message x = same::parse(b.bytes, (same::Country)country);
                if (x.valid) { m = x; a.raw = b.bytes; a.agreeing = 0; for (const auto& c : g) if (c.bytes == b.bytes) a.agreeing++; found = true; break; }
            }
            if (!found) {
                a.raw = !voted.empty() ? voted : v.front();
                for (const auto& s : v) if (voted.empty() && s.size() > a.raw.size()) a.raw = s;
                m = same::parse(a.raw, (same::Country)country);
            }
        }
        a.valid = m.valid;
        a.err = m.err;
        a.msg = std::move(m);
        addAlert(std::move(a));
    }

    // ---- the attention tone
    void toneBlock() {
        const double N = kToneBlock;
        const double p = ts1 * ts1 + ts2 * ts2 - 2 * std::cos(2 * dect2::math::kPi * kToneHz / kRate) * ts1 * ts2;
        const double msq = tsum / N;
        const double rho = tsum > 0 ? 2.0 * p / (N * tsum) : 0;
        const bool present = rho > 0.5 && msq > 1e5;
        const double tnow = now();
        if (present) {
            toneAbsent = 0;
            if (++tonePresent >= 3 && !toneOn) { toneOn = true; toneStart = tnow - 3 * 0.05; }
            if (toneOn) { toneLast = tnow; toneLevel = (float)(10 * std::log10(msq / 1e12 + 1e-20)); }
        } else {
            tonePresent = 0;
            if (toneOn && ++toneAbsent >= 3) endTone();
        }
        tn = 0; ts1 = ts2 = tsum = 0;
    }
    void endTone() {
        toneOn = false; toneAbsent = 0;
        const double dur = toneLast - toneStart;
        if (dur < 0.5) return;
        SameTone x;
        x.wall = (int64_t)std::time(nullptr); x.timeSec = toneStart; x.durSec = dur; x.levelDb = toneLevel;
        tones.push_back(x);
        while (tones.size() > kSameToneMax) tones.pop_front();
        lastToneEnd = toneLast;
        // the tone follows the header within about twenty seconds: it belongs to the newest alert
        if (!alerts.empty() && !alerts.back().eom && alerts.back().toneSec == 0 && toneStart - alerts.back().timeSec < 25.0 && toneStart >= alerts.back().timeSec - 3.0) alerts.back().toneSec = dur;
        say("SAME: 1050 Hz alert tone, " + std::to_string((int)std::lround(dur * 10) / 10.0).substr(0, 4) + " s");
        republish();
    }
    void tone(const float* x, size_t n) {
        const double coeff = 2 * std::cos(2 * dect2::math::kPi * kToneHz / kRate);
        for (size_t i = 0; i < n; i++) {
            const double s0 = x[i] + coeff * ts1 - ts2;
            ts2 = ts1; ts1 = s0;
            tsum += (double)x[i] * x[i];
            if (++tn == kToneBlock) toneBlock();
        }
    }

    void publish() {
        const double tnow = now();
        lastNow = tnow;
        if (!group.empty() && st == Idle && tnow - group.back().endSec > kGroupGapSec) finishGroup();   // not while the next repeat is arriving
        t.timeSec = tnow;
        t.snrDb = (float)fe.snrEma;
        t.cfoHz = fe.cfoHz();
        t.levelDb = fe.takeLevelDb();
        t.toneNow = toneOn;
        t.lastToneSec = toneOn ? tnow : lastToneEnd;
        t.dataValid = tnow - lastAlertSec < 120.0;
        const bool carrier = fe.carrierRecent(5.0);
        t.state = t.dataValid ? 2 : (carrier || toneOn || st != Idle ? 1 : 0);
        if (t.state != lastState) {
            say(t.state == 2 ? "SAME: message decoded" : t.state == 1 ? "SAME: signal, no message" : "SAME: no signal");
            lastState = t.state;
        }
        SameTelemetry o = t;
        fillLists(o);
        std::lock_guard<std::mutex> lk(telMu);
        o.seq = ++seq;
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        fe.process(x, n);
        const std::vector<float>& disc = fe.narrowDisc();
        a48.clear();
        audio48.process(disc.data(), disc.size(), a48);
        if (listen.on.load() && !a48.empty()) { a8.clear(); audio8.process(a48.data(), a48.size(), a8); listen.play(a8.data(), a8.size()); }
        if (!a48.empty()) {
            // the tone first (its clock is the sample count before this block)
            tone(a48.data(), a48.size());
            dem.process(a48.data(), a48.size(), [this](int b, float d) { onBit(b, d); });
            n48 += (int64_t)a48.size();
        }
        fe.commit();
        while (n48 >= nextReport) {
            nextReport += (int64_t)(0.25 * kRate);
            publish();
        }
    }
};

SameReceiver::SameReceiver() : p_(std::make_unique<Impl>()) {}
SameReceiver::~SameReceiver() = default;

void SameReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->configure(inputRateHz);
}
void SameReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->fe.setOffset(hz);
}
bool SameReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return p_->isReady;
}
void SameReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->isReady) p_->resetState();
}
void SameReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->feed(x, n);
}
bool SameReceiver::telemetry(SameTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void SameReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->alerts.clear(); p_->tones.clear();
    p_->okCount = p_->badCount = p_->burstCount = 0;
    p_->t.blocksOk = p_->t.blocksBad = 0;
    p_->republish();
}
std::string SameReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return results::pack(p_->lastNow, p_->alerts, p_->tones, p_->okCount, p_->badCount, p_->burstCount);
}
bool SameReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::deque<SameAlert> alerts;
    std::deque<SameTone> tones;
    uint64_t ok = 0, bad = 0, bursts = 0;
    if (!results::unpack(blob, t0, alerts, tones, ok, bad, bursts)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->alerts = std::move(alerts); p_->tones = std::move(tones);
    p_->okCount = ok; p_->badCount = bad; p_->burstCount = bursts;
    p_->shiftTimes(p_->lastNow - t0);
    p_->republish();
    return true;
}
void SameReceiver::setCountry(int c) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->country = c == 1 ? 1 : 0;
    p_->republish();
}
void SameReceiver::setListen(bool on) { p_->listen.on = on; }
bool SameReceiver::listening() const { return p_->listen.on.load(); }
void SameReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void SameReceiver::setMuted(bool m) { p_->listen.muted = m; }
void SameReceiver::setSilent(bool s) { p_->listen.silent = s; }
void SameReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void SameReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}

ModeTuning sameTuning() {
    ModeTuning t;
    t.stdMode = 78; t.id = "same"; t.name = "NOAA Weather Radio SAME";
    t.minMhz = 162.4; t.maxMhz = 162.55; t.defMhz = 162.550;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as APRS / packet)
    return t;
}

} // namespace dect2
