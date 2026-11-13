// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/uplink/uplink_rx.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

struct Pulse {
    double lead = 0, trail = 0;   // fractional sample index in the buffer, at half amplitude
    float amp = 0;
    double width(double rate) const { return (trail - lead) / rate * 1e6; }
};

struct IntState {
    UplinkInterrogator pub;
    std::deque<double> recent;      // hit times, last 10 s
    std::vector<double> burst;      // main-beam hits of the burst in progress
    std::deque<double> centres;     // finished bursts
};

struct AcState {
    UplinkAircraft pub;
    std::vector<int> codes;         // interrogator keys that addressed it
};

float db(float a) { return 20.f * std::log10(std::max(a, 1e-10f)); }

} // namespace

struct UplinkReceiver::Impl {
    std::mutex mu;
    double rate = 0;
    std::vector<cf32> buf;
    std::vector<float> mag;
    int64_t base = 0;               // absolute index of buf[0]
    float noiseRms = 0;
    std::deque<int64_t> used;       // absolute leads (rounded) of pulses already part of a group
    std::function<void(const std::string&)> logCb;
    std::function<void(const UplinkEvent&)> evCb;
    // published state (under mu)
    UplinkTelemetry tel;
    std::map<int, IntState> ints;
    std::map<uint32_t, AcState> acs;
    std::deque<UplinkTcas> tcas;
    std::deque<UplinkLogLine> log;
    double lastDecode = -1e9, lastPulse = -1e9, cfoAcc = 0;
    uint64_t cfoN = 0;

    // mu held: the clock starts again (configure), the kept tables move back with it
    void shiftTimes(double dt) {
        for (auto& kv : ints) {
            IntState& st = kv.second;
            st.pub.lastSec += dt; st.pub.lastBurstSec += dt;
            for (auto& v : st.recent) v += dt;
            for (auto& v : st.burst) v += dt;
            for (auto& v : st.centres) v += dt;
        }
        for (auto& kv : acs) { kv.second.pub.firstSec += dt; kv.second.pub.lastSec += dt; }
        for (auto& t : tcas) t.sec += dt;
        for (auto& l : log) l.sec += dt;
        lastDecode += dt; lastPulse += dt;
        tel.timeSec += dt;
    }

    double us(double samples) const { return samples / rate * 1e6; }
    double smp(double micro) const { return micro * 1e-6 * rate; }
    cf32 at(double i) const {
        if (i < 0) i = 0;
        const size_t k = (size_t)i;
        if (k + 1 >= buf.size()) return buf.empty() ? cf32() : buf.back();
        const float a = (float)(i - k);
        return buf[k] * (1 - a) + buf[k + 1] * a;
    }
    bool isUsed(int64_t a) const {
        for (int64_t u : used) if (std::llabs(u - a) <= 1) return true;
        return false;
    }
    void markUsed(const Pulse& p) { used.push_back(base + (int64_t)std::llround(p.lead)); }

    void process(bool all);
    std::vector<Pulse> pulses(size_t n, float thr);
    void modeS(const Pulse& p1, const Pulse& p2, const Pulse& p6);
    void record(const UplinkEvent& e);
    void hit(int key, double sec, float level, bool mainBeam, int what);
    void closeBurst(IntState& s, double now);
};

std::vector<Pulse> UplinkReceiver::Impl::pulses(size_t n, float thr) {
    std::vector<Pulse> out;
    const size_t gap = (size_t)std::max(1.0, smp(0.2)), minLen = (size_t)std::max(2.0, smp(0.3));
    size_t i = 0;
    std::vector<float> tmp;
    while (i < n) {
        if (mag[i] <= thr) { i++; continue; }
        const size_t s = i;
        size_t e = i, quiet = 0;
        float peak = 0;
        while (i < n) {
            peak = std::max(peak, mag[i]);
            if (mag[i] > std::max(thr * 0.7f, peak * 0.3f)) { e = i + 1; quiet = 0; } else if (++quiet > gap) break;   // hysteresis: the phase reversals dip P6
            i++;
        }
        if (e - s < minLen) continue;
        size_t strong = 0;
        for (size_t k = s; k < e; k++) strong += mag[k] > thr;
        if (strong * 2 < e - s) continue;   // mostly under the threshold: noise strung together by the hysteresis
        tmp.assign(mag.begin() + s, mag.begin() + e);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() * 6 / 10, tmp.end());
        Pulse p;
        p.amp = tmp[tmp.size() * 6 / 10];
        const float half = p.amp * 0.5f;
        size_t a = s > 3 ? s - 3 : 1;
        while (a < e && mag[a] < half) a++;
        p.lead = a == 0 ? 0 : (a - 1) + (half - mag[a - 1]) / std::max(1e-12f, mag[a] - mag[a - 1]);
        size_t b = std::min(n - 2, e + 2);
        while (b > a && mag[b] < half) b--;
        p.trail = b + (mag[b] - half) / std::max(1e-12f, mag[b] - mag[b + 1]);
        if (p.trail <= p.lead) continue;
        out.push_back(p);
    }
    return out;
}

void UplinkReceiver::Impl::process(bool all) {
    const size_t n = buf.size();
    if (n < 64 || rate <= 0) return;
    const size_t tail = (size_t)smp(40);
    if (!all && n < tail * 2) return;
    mag.resize(n);
    for (size_t i = 0; i < n; i++) mag[i] = std::abs(buf[i]);
    {   // noise floor: the mean power of the envelope samples under its median (Rayleigh: 0.614 sigma^2), which an 8-bit radio's
        // quantised noise (mostly 0 and 1 LSB) does not fool the way a low percentile would
        std::vector<float> s;
        s.reserve(n / 7 + 1);
        for (size_t i = 0; i < n; i += 7) s.push_back(mag[i]);
        const size_t h = s.size() / 2;
        std::nth_element(s.begin(), s.begin() + h, s.end());
        double p2 = 0;
        for (size_t i = 0; i < h; i++) p2 += (double)s[i] * s[i];
        const float rms = (float)std::sqrt(std::max(p2 / std::max<size_t>(1, h) / 0.6137, 1e-14));   // sigma of one component
        noiseRms = noiseRms <= 0 ? rms : 0.8f * noiseRms + 0.2f * rms;
    }
    const float thr = std::max(3.5f * noiseRms, 1e-6f);
    size_t limit = all ? n : n - tail;
    while (!all && limit > 0 && mag[limit] > thr) limit--;   // do not cut a pulse in two
    const std::vector<Pulse> ps = pulses(n, thr);
    auto widthOk = [&](const Pulse& p, double lo, double hi) { const double w = p.width(rate); return w >= lo && w <= hi; };
    auto nearBy = [&](const Pulse& p, double t, double tol) { return std::fabs(us(p.lead) - t) <= tol; };
    for (size_t i = 0; i < ps.size(); i++) {
        const Pulse& p1 = ps[i];
        if (p1.lead >= limit) break;
        if (!widthOk(p1, 0.5, 1.15) || isUsed(base + (int64_t)std::llround(p1.lead))) continue;
        { std::lock_guard<std::mutex> lk(mu); tel.pulses++; lastPulse = (base + p1.lead) / rate; }
        const double t1 = us(p1.lead);
        const Pulse *p2 = nullptr, *p6 = nullptr, *p3 = nullptr, *p4 = nullptr;
        Pulse v6;                      // P6 put together again where noise or a P5 cut it at the phase reversals
        std::vector<const Pulse*> parts;
        char acMode = 0;
        for (size_t j = i + 1; j < ps.size(); j++) {
            const Pulse& q = ps[j];
            const double dt = us(q.lead) - t1;
            if (dt > 24) break;
            if (isUsed(base + (int64_t)std::llround(q.lead))) continue;
            const float rel = db(q.amp) - db(p1.amp);
            if (!p2 && nearBy(q, t1 + 2.0, 0.2) && widthOk(q, 0.5, 1.15)) p2 = &q;
            else if (!p6 && nearBy(q, t1 + 3.5, 0.25) && q.width(rate) >= 0.2) {
                v6 = q;
                parts.assign(1, &q);
                for (size_t k = j + 1; k < ps.size() && us(ps[k].lead - v6.trail) < 0.6; k++) { v6.trail = ps[k].trail; parts.push_back(&ps[k]); }
                if (v6.width(rate) >= 14.5 && v6.width(rate) <= 32) p6 = &v6;
            }
            else if (!p3 && std::fabs(rel) <= 4 && widthOk(q, 0.5, 1.15) && (nearBy(q, t1 + 8, 0.2) || nearBy(q, t1 + 21, 0.2))) { p3 = &q; acMode = dt < 14 ? 'A' : 'C'; }
        }
        if (p2 && p6 && std::fabs(db(p2->amp) - db(p1.amp)) <= 4) {
            markUsed(p1); markUsed(*p2);
            for (const Pulse* q : parts) markUsed(*q);
            modeS(p1, *p2, *p6);
            continue;
        }
        if (!p3) continue;
        const double t3 = us(p3->lead);
        for (size_t j = i + 1; j < ps.size(); j++) {
            const Pulse& q = ps[j];
            if (us(q.lead) - t3 > 3) break;
            if (nearBy(q, t3 + 2.0, 0.2) && widthOk(q, 0.5, 2.0) && std::fabs(db(q.amp) - db(p1.amp)) <= 4 && !isUsed(base + (int64_t)std::llround(q.lead))) { p4 = &q; break; }
        }
        markUsed(p1); markUsed(*p3);
        if (p2) markUsed(*p2);
        if (p4) markUsed(*p4);
        UplinkEvent e;
        e.sec = (base + p1.lead) / rate;
        e.acMode = acMode;
        e.levelDb = db(p1.amp);
        e.p2Db = p2 ? db(p2->amp) - db(p1.amp) : -99.f;
        e.sidelobe = e.p2Db >= -1.f;
        if (p4) { e.kind = 1; e.p4 = p4->width(rate) > 1.2 ? 2 : 1; }
        record(e);
    }
    if (limit > 0) {
        buf.erase(buf.begin(), buf.begin() + limit);
        base += (int64_t)limit;
        while (!used.empty() && used.front() < base - (int64_t)smp(5)) used.pop_front();
    }
}

void UplinkReceiver::Impl::modeS(const Pulse& p1, const Pulse&, const Pulse& p6) {
    UplinkEvent e;
    e.sec = (base + p1.lead) / rate;
    e.levelDb = db(p1.amp);
    const double step = smp(0.05);
    auto metric = [&](double ts) {
        cf32 A, B;
        for (int m = 1; m <= 8; m++) { A += at(ts - m * step); B += at(ts + m * step); }
        const float d = std::abs(A) * std::abs(B);
        return d > 0 ? -(A * std::conj(B)).real() / d : 0.f;
    };
    const double nom = p6.lead + smp(1.25);
    float best = -2;
    std::vector<std::pair<double, float>> m;
    for (double d = -0.3; d <= 0.3001; d += 0.02) { const double ts = nom + smp(d); const float v = metric(ts); m.push_back({ts, v}); best = std::max(best, v); }
    if (best < 0.6f) { e.kind = 3; record(e); return; }
    double sum = 0; int cnt = 0;
    for (const auto& x : m) if (x.second >= best - 0.03f) { sum += x.first; cnt++; }
    const double ts0 = sum / cnt;
    const int nbits = p6.width(rate) > 23 ? 112 : 56;
    const double chip = smp(0.25);
    std::vector<cf32> d(nbits), bestD;
    float phi = 0, bestE = -1;
    // fine timing: the chip instants that give the clearest decisions, within 0.08 us of the sync estimate
    for (double off = -0.15; off <= 0.1501; off += 0.01) {
        const double ts = ts0 + smp(off);
        cf32 sq;
        const double w = smp(0.05);
        auto chipAt = [&](double c) { return at(c - w) + at(c) + at(c + w); };   // the middle 0.1 us of the chip
        cf32 prev = chipAt(ts + chip + chip / 2);
        for (int k = 1; k <= nbits; k++) {
            const cf32 cur = chipAt(ts + chip * (k + 1) + chip / 2);
            d[k - 1] = cur * std::conj(prev);
            sq += d[k - 1] * d[k - 1] / std::max(1e-20f, std::abs(d[k - 1]));
            prev = cur;
        }
        const float ph = std::arg(sq) / 2;
        const cf32 r = std::polar(1.f, -ph);
        float en = 0;
        for (const cf32& x : d) en += std::fabs((x * r).real());
        if (en > bestE) { bestE = en; bestD = d; phi = ph; }
    }
    d = bestD;
    const cf32 rot = std::polar(1.f, -phi);
    uint8_t b[14] = {};
    for (int k = 0; k < nbits; k++) if ((d[k] * rot).real() < 0) b[k / 8] |= (uint8_t)(0x80 >> (k % 8));
    e.cfoHz = phi / (2 * 3.14159265358979 * 0.25e-6);
    if (!uplink::decode(b, nbits, e.msg) || (e.msg.uf == 11 && !e.msg.allCallOk)) { e.kind = 4; record(e); return; }
    e.kind = 2;
    record(e);
}

void UplinkReceiver::Impl::hit(int key, double sec, float level, bool mainBeam, int what) {
    IntState& s = ints[key];
    UplinkInterrogator& p = s.pub;
    if (p.count == 0) {
        if (key >= 0 && key < 100) p.ii = key; else if (key >= 100) p.si = key - 100;
        p.kind = key == -1 ? 1 : key == -3 ? 2 : 0;
        if (logCb) {
            char b[80];
            if (key < 0) snprintf(b, sizeof b, "Mode S uplink: %s", key == -1 ? "Mode A/C interrogations" : "intermode all-calls"); else snprintf(b, sizeof b, "Mode S uplink: interrogator %s %d", key < 100 ? "II" : "SI", key < 100 ? key : key - 100);
            logCb(b);
        }
    }
    p.count++;
    if (what == 0) p.allCalls++; else if (what == 1) p.rollCalls++; else if (what == 2) p.modeA++; else if (what == 3) p.modeC++; else if (what == 4) p.intermode++;
    p.lastSec = sec;
    s.recent.push_back(sec);
    while (!s.recent.empty() && s.recent.front() < sec - 10) s.recent.pop_front();
    if (!mainBeam) { p.sidelobe++; return; }
    if (level < p.levelDb - 6) return;   // a roll-call to an aircraft elsewhere, a sidelobe: not our main beam
    p.levelDb = std::max(p.levelDb, level);
    closeBurst(s, sec);
    s.burst.push_back(sec);
}

void UplinkReceiver::Impl::closeBurst(IntState& s, double now) {
    UplinkInterrogator& p = s.pub;
    if (!s.burst.empty() && now - s.burst.back() > 0.3) {
        double c = 0;
        for (double t : s.burst) c += t;
        s.centres.push_back(c / s.burst.size());
        if (s.centres.size() > 24) s.centres.pop_front();
        p.bursts++;
        p.lastBurstSec = s.centres.back();
        s.burst.clear();
        std::vector<double> dif;
        for (size_t i = 1; i < s.centres.size(); i++) { const double x = s.centres[i] - s.centres[i - 1]; if (x > 0.8 && x < 30) dif.push_back(x); }
        if (!dif.empty()) { std::nth_element(dif.begin(), dif.begin() + dif.size() / 2, dif.end()); p.periodSec = (float)dif[dif.size() / 2]; }
    }
}

void UplinkReceiver::Impl::record(const UplinkEvent& e) {
    if (evCb) evCb(e);
    std::lock_guard<std::mutex> lk(mu);
    UplinkTelemetry& t = tel;
    UplinkLogLine l;
    l.sec = e.sec; l.levelDb = e.levelDb;
    char b[160];
    if (e.kind == 3) { t.modeSSidelobe++; t.blocksBad++; t.seq++; return; }
    if (e.kind == 4) { t.blocksBad++; t.seq++; return; }
    t.blocksOk++;
    t.snrDb = e.levelDb - t.noiseDb;
    lastDecode = e.sec;
    if (e.kind == 0 || e.kind == 1) {
        if (e.kind == 1) {
            if (e.p4 == 2) t.intermodeAcs++; else t.intermodeAc++;
            snprintf(b, sizeof b, "intermode all-call Mode %c, %s%s", e.acMode, e.p4 == 2 ? "A/C/S (long P4)" : "A/C only (short P4)", e.sidelobe ? ", sidelobe" : "");
        } else {
            if (e.acMode == 'A') t.modeA++; else t.modeC++;
            if (e.sidelobe) t.modeAcSidelobe++;
            if (e.p2Db > -90) snprintf(b, sizeof b, "Mode %c%s, P2 %+.0f dB", e.acMode, e.sidelobe ? " sidelobe (suppressed)" : "", e.p2Db);
            else snprintf(b, sizeof b, "Mode %c", e.acMode);
        }
        hit(e.kind == 1 ? -3 : -1, e.sec, e.levelDb, !e.sidelobe, e.kind == 1 ? 4 : e.acMode == 'A' ? 2 : 3);
        l.text = b;
    } else {
        const uplink::Uplink& u = e.msg;
        t.modeS++;
        t.uf[u.uf & 31]++;
        cfoAcc += e.cfoHz; cfoN++;
        t.cfoHz = cfoAcc / cfoN;
        l.uf = u.uf;
        l.text = uplink::describe(u);
        const int key = u.ii >= 0 ? u.ii : u.si >= 0 ? 100 + u.si : -2;
        if (key != -2) hit(key, e.sec, e.levelDb, true, u.uf == 11 ? 0 : 1);
        if (u.uf != 11) {
            l.addr = u.addr;
            AcState& a = acs[u.addr];
            UplinkAircraft& p = a.pub;
            if (p.count == 0) { p.addr = u.addr; p.firstSec = e.sec; }
            p.count++;
            p.confirmed = p.count >= 2;
            p.uf[u.uf & 31]++;
            p.lastSec = e.sec;
            p.levelDb = e.levelDb;
            if (u.bds1 >= 0) {
                const std::string r = u.bds1 == 0 ? "AICB" : uplink::bdsText(u.bds1, u.bds2);
                if (std::find(p.registers.begin(), p.registers.end(), r) == p.registers.end()) p.registers.push_back(r);
            }
            if (key != -2 && std::find(a.codes.begin(), a.codes.end(), key) == a.codes.end()) {
                a.codes.push_back(key);
                p.interrogators.clear();
                for (int c : a.codes) { char s[16]; snprintf(s, sizeof s, "%s%s %d", p.interrogators.empty() ? "" : ", ", c < 100 ? "II" : "SI", c < 100 ? c : c - 100); p.interrogators += s; }
            }
            if (u.uf == 0 || u.uf == 16) {
                UplinkTcas c;
                c.sec = e.sec; c.uf = u.uf; c.to = u.addr; c.from = u.ra ? u.mid : 0;
                if (u.uf == 0) { snprintf(b, sizeof b, "ACAS surveillance of %06X%s", u.addr, u.aq ? " (acquisition)" : ""); c.text = b; }
                else if (u.ra) { snprintf(b, sizeof b, "RA coordination %06X -> %06X: %s, %s", u.mid, u.addr, uplink::vrcText(u.vrc), uplink::cvcText(u.cvc)); c.text = b; }
                else c.text = l.text;
                tcas.push_back(c);
                if (tcas.size() > kUplinkTcasKeep) tcas.pop_front();
                if (u.ra && logCb) logCb("Mode S uplink: " + c.text);
            }
        }
    }
    log.push_back(l);
    if (log.size() > kUplinkLogKeep) log.pop_front();
    t.seq++;
}

UplinkReceiver::UplinkReceiver() : p_(std::make_unique<Impl>()) {}
UplinkReceiver::~UplinkReceiver() = default;

void UplinkReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->shiftTimes(-p_->tel.timeSec);   // the tables stay (clearResults): the clock starts again at 0
    p_->buf.clear(); p_->base = 0; p_->used.clear(); p_->noiseRms = 0;
    p_->tel.inputRate = inputRateHz;
    p_->tel.seq++;
}
bool UplinkReceiver::ready() const { return p_->rate >= uplinkTuning().minSampleRate - 1; }
void UplinkReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->noiseRms = 0;
    p_->tel.seq++;
}
void UplinkReceiver::feed(const cf32* x, size_t n) {
    Impl& p = *p_;
    if (!ready()) return;
    p.buf.insert(p.buf.end(), x, x + n);
    if (p.buf.size() >= (size_t)p.smp(6000)) p.process(false);   // every 6 ms of signal
    std::lock_guard<std::mutex> lk(p.mu);
    p.tel.timeSec = (p.base + (double)p.buf.size()) / p.rate;
}
void UplinkReceiver::flush() { if (ready()) p_->process(true); }

bool UplinkReceiver::telemetry(UplinkTelemetry& out, uint64_t lastSeq) {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lk(p.mu);
    UplinkTelemetry& t = p.tel;
    const uint64_t prevSeq = t.seq;
    (void)prevSeq;
    t.noiseDb = db(p.noiseRms);
    t.levelDb = t.noiseDb;
    const double now = t.timeSec;
    t.state = now - p.lastDecode < 10 ? 2 : now - p.lastPulse < 10 ? 1 : 0;
    t.dataValid = t.state == 2;
    if (t.seq == lastSeq) return false;
    out = t;
    out.interrogators.clear();
    for (auto& kv : p.ints) {
        p.closeBurst(kv.second, now);
        UplinkInterrogator i = kv.second.pub;
        size_t r = 0;
        for (double s : kv.second.recent) r += s >= now - 10;
        i.ratePerSec = r / 10.f;
        out.interrogators.push_back(i);
    }
    out.aircraft.clear();
    for (auto& kv : p.acs) out.aircraft.push_back(kv.second.pub);
    std::sort(out.aircraft.begin(), out.aircraft.end(), [](const UplinkAircraft& a, const UplinkAircraft& b) { return a.lastSec > b.lastSec; });
    if (out.aircraft.size() > 400) out.aircraft.resize(400);
    out.tcas.assign(p.tcas.begin(), p.tcas.end());
    out.log.assign(p.log.begin(), p.log.end());
    return true;
}

void UplinkReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void UplinkReceiver::setEventCallback(std::function<void(const UplinkEvent&)> cb) { p_->evCb = std::move(cb); }
void UplinkReceiver::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& p = *p_;
    p.ints.clear(); p.acs.clear(); p.tcas.clear(); p.log.clear();
    const uint64_t s = p.tel.seq;
    const double r = p.tel.inputRate, ts = p.tel.timeSec;
    p.tel = UplinkTelemetry();
    p.tel.seq = s + 1; p.tel.inputRate = r; p.tel.timeSec = ts;
    p.cfoAcc = 0; p.cfoN = 0;
}

void UplinkReceiver::clearResults() { clear(); }
std::string UplinkReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->ints, p_->acs, p_->tcas, p_->log);
}
bool UplinkReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::map<int, IntState> ints;
    std::map<uint32_t, AcState> acs;
    std::deque<UplinkTcas> tcas;
    std::deque<UplinkLogLine> log;
    if (!results::unpack(blob, t0, ints, acs, tcas, log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& p = *p_;
    p.ints = std::move(ints); p.acs = std::move(acs); p.tcas = std::move(tcas); p.log = std::move(log);
    const double now = p.tel.timeSec;
    p.tel.timeSec = t0;
    const double ld = p.lastDecode, lp = p.lastPulse;
    p.shiftTimes(now - t0);
    p.lastDecode = ld; p.lastPulse = lp;
    p.tel.seq++;
    return true;
}

ModeTuning uplinkTuning() {
    ModeTuning t;
    t.stdMode = 45; t.id = "uplink"; t.name = "Mode S uplink";
    t.minMhz = 1025; t.maxMhz = 1035; t.defMhz = 1030.0;
    // the 4 Mchip/s phase reversals of P6 need about 8 MHz; 10 Msps with the 7 MHz filter keeps them sharp
    t.sampleRate = 10000000.0; t.basebandHz = 7000000.0; t.bandwidthMhz = 8;
    t.minSampleRate = 8000000.0;
    t.carrierAtCentre = true;   // an interrogator on the tuned frequency is a carrier on the centre: the DC removal would eat P6
    t.notCircular = true;       // pulses of one phase are not circular: the blind IQ estimate would bend them
    return t;
}

} // namespace dect2
