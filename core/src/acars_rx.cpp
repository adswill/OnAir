#include "dect2/acars_rx.h"
#include "dect2/acars_proto.h"
#include "dect2/aero_pos.h"
#include "dect2/fftutil.h"
#include "acars_chan.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxActive = 8;                 // carriers decoded at the same time
constexpr int kMaxChanInfo = 24;
constexpr double kBlockSec = 0.008;           // spectrum look every 8 ms
constexpr double kLookbackSec = 0.060;        // a new carrier is decoded from this long before it was noticed (the pre-key is 53 ms)
constexpr double kHoldSec = 0.4;              // a channel stays on this long after its carrier drops
constexpr size_t kMaxTrack = 24;
constexpr double kHalfBandHz = 3400;          // channel power is summed over +-3.4 kHz

struct Cand {
    double freq = 0, off = 0;                 // absolute Hz; Hz from 0 Hz of the input
    int binLo = 0, nb = 1;
    double p = 0, ratio = 0;                  // smoothed power in the band (A^2 units), over the noise in the same band
    double cfo = 0;
    bool active = false;
    int slot = -1;
    int64_t pos = 0, lastAbove = 0, activeSince = 0, coolUntil = 0;
    uint64_t framesAtStart = 0, framesLast = 0;
    bool seen = false;
    uint32_t msgs = 0, bad = 0;
    double lastHeard = 0;
    float levelDb = -120, snrDb = 0;
};

} // namespace

struct AcarsReceiver::Impl {
    std::mutex procMu, telMu;
    double fs = 0, offsetHz = 0, centerHz = 131.5e6, thrDb = 8;
    std::vector<double> userCh;
    bool dirty = true;

    std::vector<cf32> ring;
    int64_t rsz = 0, wr = 0, blk = 0, nextDet = 0, nextPub = 0;
    int nf = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win, pw, pwTmp;
    std::vector<cf32> fbuf;
    float winNorm = 1;
    double noiseBin = 0;
    bool noiseInit = false;
    int warm = 0;

    std::vector<Cand> cands;
    std::vector<std::unique_ptr<AcarsChannelRx>> pool;
    std::vector<bool> slotUsed;
    std::vector<AcarsRawBlock> rawTmp;

    // published state (telMu)
    std::deque<AcarsMessage> msgs;
    std::unordered_map<std::string, AcarsAircraft> aircraft;
    uint64_t okCount = 0, badCount = 0, fixedCount = 0, serial = 0, framesTotal = 0, posTotal = 0;
    std::atomic<bool> rdy{false};
    uint64_t seq = 0;
    double lastMsgSec = -1e9;
    AcarsTelemetry pub;
    std::function<void(const std::string&)> log;

    void say(const std::string& s) { if (log) { char t[24]; snprintf(t, sizeof t, "[%.3f] ", (double)wr / fs); log(t + s); } }

    void rebuild() {
        dirty = false;
        std::fill(slotUsed.begin(), slotUsed.end(), false);
        cands.clear();
        if (fs <= 0) return;
        const double binW = fs / nf;
        const int half = std::max(2, (int)std::ceil(kHalfBandHz / binW));
        auto add = [&](double f, bool user) {
            Cand c;
            c.freq = f;
            c.off = f - centerHz + offsetHz;
            if (std::fabs(c.off) > 0.45 * fs) return;
            if (!user && std::fabs(f - centerHz) < 2500) return;          // the radio's own DC spike
            const double cb = c.off / binW;
            c.binLo = (int)std::lround(cb) - half;
            c.nb = 2 * half + 1;
            cands.push_back(c);
        };
        if (!userCh.empty()) {
            for (double f : userCh) add(f, true);
        } else {
            const double span = std::min(0.45 * fs, 2.5e6);
            const double lo = std::ceil((centerHz - span) / 25e3) * 25e3;
            for (double f = lo; f <= centerHz + span; f += 25e3)
                if (f >= 118e6 && f <= 137e6) add(f, false);
        }
    }

    void init() {
        nf = 1024;
        while (nf < fs / 1500.0 && nf < 16384) nf *= 2;
        blk = (int64_t)std::lround(kBlockSec * fs);
        rsz = (int64_t)(0.25 * fs) + nf;
        ring.assign((size_t)rsz, cf32(0, 0));
        fft = std::make_unique<Fft>(nf);
        win.resize((size_t)nf);
        double sw2 = 0;
        for (int i = 0; i < nf; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / nf)); sw2 += (double)win[(size_t)i] * win[(size_t)i]; }
        winNorm = (float)(1.0 / (nf * sw2));
        fbuf.assign((size_t)nf, cf32(0, 0));
        pw.assign((size_t)nf, 0.f);
        pwTmp.assign((size_t)nf, 0.f);
        pool.clear();
        for (int i = 0; i < kMaxActive; i++) {
            pool.push_back(std::make_unique<AcarsChannelRx>());
            pool.back()->configure(fs, 0);
        }
        slotUsed.assign(kMaxActive, false);
        dirty = true;
        clearRun();
    }

    void clearRun() {
        wr = 0; nextDet = blk; nextPub = (int64_t)(0.25 * fs);
        noiseInit = false; noiseBin = 0; warm = 0;
        std::fill(slotUsed.begin(), slotUsed.end(), false);
        for (auto& c : cands) { c.active = false; c.slot = -1; c.p = 0; c.ratio = 0; c.seen = false; c.msgs = c.bad = 0; c.lastHeard = 0; c.levelDb = -120; c.coolUntil = 0; }
        std::lock_guard<std::mutex> lk(telMu);
        msgs.clear(); aircraft.clear();
        okCount = badCount = fixedCount = 0; framesTotal = 0; posTotal = 0;
        lastMsgSec = -1e9;
    }

    const cf32* at(int64_t abs) const { return &ring[(size_t)(abs % rsz)]; }

    void detect() {
        // the spectrum of the last nf samples
        const int64_t s0 = wr - nf;
        if (s0 < 0) return;
        for (int i = 0; i < nf; i++) fbuf[(size_t)i] = ring[(size_t)((s0 + i) % rsz)] * win[(size_t)i];
        fft->forward(fbuf.data());
        for (int i = 0; i < nf; i++) pw[(size_t)i] = std::norm(fbuf[(size_t)i]) * winNorm;
        pwTmp = pw;
        std::nth_element(pwTmp.begin(), pwTmp.begin() + nf / 2, pwTmp.end());
        const double nb = pwTmp[(size_t)(nf / 2)] / 0.693;      // the median of an exponential is ln 2 of its mean
        noiseBin = noiseInit ? 0.9 * noiseBin + 0.1 * nb : nb;
        noiseInit = true;
        if (++warm < 3) return;
        if (dirty) rebuild();
        const double binW = fs / nf;
        const double on = std::pow(10.0, thrDb / 10.0), off = std::pow(10.0, (thrDb - 3.0) / 10.0);
        // strongest first, so that with more carriers than decoders the strongest are taken
        std::vector<Cand*> order;
        for (auto& c : cands) {
            double sum = 0, cen = 0, ex = 0;
            for (int k = 0; k < c.nb; k++) {
                const int b = ((c.binLo + k) % nf + nf) % nf;
                const double v = pw[(size_t)b];
                sum += v;
                const double e = std::max(v - noiseBin, 0.0);       // noise-free weights keep the centroid on the carrier
                ex += e; cen += (double)(c.binLo + k) * e;
            }
            c.p = c.p > 0 && c.ratio > 0 ? 0.5 * c.p + 0.5 * sum : sum;
            const double noise = noiseBin * c.nb;
            c.ratio = c.p / std::max(noise, 1e-30);
            if (ex > 0 && sum >= 10.0 * noiseBin * c.nb) {          // only while this look has a clear carrier
                const double mean = cen / ex - c.off / binW;         // bins from the channel centre
                c.cfo = c.active ? 0.7 * c.cfo + 0.3 * mean * binW : mean * binW;
            }
            const double carrier = std::max(c.p - noise, 0.0);
            c.snrDb = (float)(10 * std::log10(std::max(carrier / std::max(noise, 1e-30), 1e-3)));
            c.levelDb = c.ratio > off ? (float)(10 * std::log10(std::max(carrier, 1e-12))) : -120.f;
            order.push_back(&c);
        }
        std::sort(order.begin(), order.end(), [](const Cand* a, const Cand* b) { return a->ratio > b->ratio; });
        int nAct = 0;
        for (auto& c : cands) nAct += c.active ? 1 : 0;
        const double tNow = (double)wr / fs;
        for (Cand* c : order) {
            if (c->active) {
                if (c->ratio >= off) c->lastAbove = wr;
                else if (wr - c->lastAbove > (int64_t)(kHoldSec * fs)) { deactivate(*c, "carrier gone"); nAct--; continue; }
                const uint64_t fr = pool[(size_t)c->slot]->framesStarted();
                if (wr - c->activeSince > (int64_t)(8 * fs) && fr == c->framesAtStart) {     // a carrier that is not ACARS: leave it alone for a while
                    c->coolUntil = wr + (int64_t)(30 * fs);
                    deactivate(*c, "no ACARS data");
                    nAct--;
                }
            } else if (c->ratio >= on && wr >= c->coolUntil && nAct < kMaxActive) {
                activate(*c);
                nAct++;
            }
        }
        (void)tNow;
    }

    void activate(Cand& c) {
        int slot = -1;
        for (int i = 0; i < kMaxActive; i++) if (!slotUsed[(size_t)i]) { slot = i; break; }
        if (slot < 0) return;
        slotUsed[(size_t)slot] = true;
        c.slot = slot; c.active = true; c.seen = true;
        pool[(size_t)slot]->setOffset(c.off);
        c.pos = std::max<int64_t>(0, wr - (int64_t)(kLookbackSec * fs));
        c.lastAbove = wr; c.activeSince = wr;
        c.framesAtStart = c.framesLast = pool[(size_t)slot]->framesStarted();
        char b[96];
        snprintf(b, sizeof b, "ACARS: carrier on %.3f MHz (%.0f dB over the noise)", c.freq / 1e6, 10 * std::log10(std::max(c.ratio, 1e-3)));
        say(b);
    }

    void deactivate(Cand& c, const char* why) {
        if (c.slot >= 0) slotUsed[(size_t)c.slot] = false;
        c.slot = -1; c.active = false;
        char b[96];
        snprintf(b, sizeof b, "ACARS: %.3f MHz idle (%s)", c.freq / 1e6, why);
        say(b);
    }

    void advance() {
        for (auto& c : cands) {
            if (!c.active) continue;
            AcarsChannelRx& ch = *pool[(size_t)c.slot];
            while (c.pos < wr) {
                int64_t n = std::min<int64_t>(wr - c.pos, 16384);
                const int64_t a = c.pos % rsz;
                n = std::min<int64_t>(n, rsz - a);
                ch.process(&ring[(size_t)a], (size_t)n);
                c.pos += n;
                const uint64_t fr = ch.framesStarted();
                framesTotal += fr - c.framesLast; c.framesLast = fr;
                rawTmp.clear();
                ch.takeBlocks(rawTmp);
                for (auto& rb : rawTmp) handleBlock(c, rb, (double)c.pos / fs);
            }
        }
    }

    void handleBlock(Cand& c, const AcarsRawBlock& rb, double tSec) {
        const int fixedBits = rb.fixedBits;
        AcarsMessage m;
        bool parsed = false;
        if (rb.ok) {
            uint8_t t7[256];
            for (int i = 0; i < rb.len; i++) t7[i] = rb.txt[i] & 0x7f;
            parsed = acarsParseBlock(t7, rb.len, m);
        }
        std::lock_guard<std::mutex> lk(telMu);
        if (!parsed) { badCount++; c.bad++; return; }
        m.serial = ++serial;
        m.timeSec = tSec;
        m.wallTime = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        m.freqHz = c.freq;
        m.levelDb = rb.levelDb;
        m.crcOk = true;
        m.parityFixed = fixedBits;
        okCount++; c.msgs++; c.lastHeard = tSec;
        if (fixedBits) fixedCount++;
        lastMsgSec = tSec;
        // positions: the ARINC 622 ADS-C reports and the plain-text position reports (the code the Aero mode uses); only from aircraft
        AeroPosition pos;
        aeroPositionFromMessage(m.label, m.text, !m.downlink, pos, &m.adsc);
        if (!m.downlink) pos = AeroPosition();       // an uplink that names a position is not the aircraft's own
        if (pos.valid) {
            m.hasPos = true; m.lat = pos.lat; m.lon = pos.lon;
            if (m.decoded.empty()) {
                char b[96];
                snprintf(b, sizeof b, "%s %.4f %.4f", pos.kind.c_str(), pos.lat, pos.lon);
                m.decoded = b;
                if (pos.hasAlt) m.decoded += " " + std::to_string(pos.altFt) + " ft";
            }
        }
        if (!m.reg.empty()) {
            auto it = aircraft.find(m.reg);
            if (it == aircraft.end()) {
                AcarsAircraft a; a.reg = m.reg;
                it = aircraft.emplace(m.reg, a).first;
                char b[96];
                snprintf(b, sizeof b, "ACARS: new aircraft %s on %.3f MHz", m.reg.c_str(), c.freq / 1e6);
                say(b);
            }
            AcarsAircraft& a = it->second;
            a.messages++; a.freqHz = c.freq; a.lastHeardSec = tSec; a.lastLabel = m.label;
            if (!m.flightId.empty()) a.flight = m.flightId;
            if (m.downlink) a.levelDb = m.levelDb;
            if (pos.valid) addPosition(a, pos, tSec, m.wallTime);
            if (aircraft.size() > 100) {                      // drop the one heard longest ago
                auto old = aircraft.begin();
                for (auto j = aircraft.begin(); j != aircraft.end(); ++j) if (j->second.lastHeardSec < old->second.lastHeardSec) old = j;
                aircraft.erase(old);
            }
        }
        msgs.push_front(std::move(m));
        if (msgs.size() > 200) msgs.pop_back();
    }

    void addPosition(AcarsAircraft& a, const AeroPosition& p, double tSec, int64_t wall) {
        posTotal++;
        a.positions++;
        if (a.flight.empty() && !p.flightId.empty()) a.flight = p.flightId;
        if (p.icao) a.icao = p.icao;
        // with no track in the report, the bearing from the last position (when it moved more than about 1 km)
        double trk = p.trackDeg;
        bool hasTrk = p.hasTrack;
        if (!hasTrk && a.hasPos) {
            const double p1 = a.lat * kPi / 180, p2 = p.lat * kPi / 180, dl = (p.lon - a.lon) * kPi / 180;
            const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
            if (std::hypot(p.lat - a.lat, (p.lon - a.lon) * std::cos(p2)) > 0.01) {
                hasTrk = true;
                trk = std::fmod(std::atan2(y, x) * 180 / kPi + 360, 360);
            }
        }
        a.hasPos = true; a.lat = p.lat; a.lon = p.lon;
        a.hasAlt = p.hasAlt; a.altFt = p.altFt;
        a.hasTrack = hasTrk; a.trackDeg = trk;
        a.hasSpeed = p.hasSpeed; a.speedKt = p.speedKt;
        a.posSource = p.source; a.posKind = p.kind;
        a.posTime = tSec; a.posWall = wall;
        a.reportSecPastHour = p.secPastHour; a.reportSecOfDay = p.secOfDay;
        a.route = p.route;
        if (a.track.empty() || a.track.back().lat != p.lat || a.track.back().lon != p.lon) {
            a.track.push_back({p.lat, p.lon, p.altFt, tSec});
            if (a.track.size() > kMaxTrack) a.track.erase(a.track.begin());
        }
    }

    void publish() {
        std::lock_guard<std::mutex> lk(telMu);
        AcarsTelemetry t;
        t.seq = ++seq;
        t.centerHz = centerHz;
        t.nowSec = (double)wr / fs;
        t.blocksOk = okCount; t.blocksBad = badCount; t.parityFixed = fixedCount;
        t.framesStarted = framesTotal;
        t.positionsTotal = posTotal;
        t.dataValid = okCount > 0;
        const Cand* best = nullptr;
        int nAct = 0;
        for (auto& c : cands) {
            if (c.active) { nAct++; if (!best || c.ratio > best->ratio) best = &c; }
            if (c.seen || c.active) {
                AcarsChannelInfo ci;
                ci.freqHz = c.freq; ci.levelDb = c.active ? c.levelDb : -120.f; ci.snrDb = c.active ? c.snrDb : 0.f; ci.cfoHz = (float)c.cfo;
                ci.active = c.active; ci.messages = c.msgs; ci.bad = c.bad; ci.lastHeardSec = c.lastHeard;
                t.channels.push_back(ci);
            }
        }
        std::sort(t.channels.begin(), t.channels.end(), [](const AcarsChannelInfo& a, const AcarsChannelInfo& b) { return a.freqHz < b.freqHz; });
        if (t.channels.size() > (size_t)kMaxChanInfo) {          // keep the active ones and the busiest
            std::stable_sort(t.channels.begin(), t.channels.end(), [](const AcarsChannelInfo& a, const AcarsChannelInfo& b) {
                if (a.active != b.active) return a.active;
                return a.messages > b.messages;
            });
            t.channels.resize(kMaxChanInfo);
            std::sort(t.channels.begin(), t.channels.end(), [](const AcarsChannelInfo& a, const AcarsChannelInfo& b) { return a.freqHz < b.freqHz; });
        }
        t.state = (t.nowSec - lastMsgSec < 30.0) ? 2 : (nAct > 0 ? 1 : 0);
        if (best) { t.cfoHz = best->cfo; t.snrDb = best->snrDb; }
        t.messages.assign(msgs.begin(), msgs.end());
        std::vector<AcarsAircraft> al;
        for (auto& kv : aircraft) al.push_back(kv.second);
        std::sort(al.begin(), al.end(), [](const AcarsAircraft& a, const AcarsAircraft& b) { return a.lastHeardSec > b.lastHeardSec; });
        t.aircraft = std::move(al);
        pub = std::move(t);
    }
};

AcarsReceiver::AcarsReceiver() : p_(std::make_unique<Impl>()) {}
AcarsReceiver::~AcarsReceiver() = default;

void AcarsReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->fs = inputRateHz;
    p_->rdy = inputRateHz >= acarsTuning().minSampleRate - 1;
    if (p_->rdy) p_->init();
    else { p_->ring.clear(); p_->cands.clear(); p_->pool.clear(); }
}
void AcarsReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->offsetHz = hz; p_->dirty = true; }
void AcarsReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->centerHz = hz; p_->dirty = true; }
void AcarsReceiver::setChannels(const std::vector<double>& hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->userCh = hz;
    p_->dirty = true;
}
void AcarsReceiver::setThresholdDb(double db) { std::lock_guard<std::mutex> lk(p_->procMu); p_->thrDb = std::max(3.0, std::min(30.0, db)); }

bool AcarsReceiver::ready() const {
    return p_->rdy.load();
}

void AcarsReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->fs <= 0 || p_->ring.empty()) return;
    for (auto& c : p_->pool) c->reset();
    p_->clearRun();
    p_->dirty = true;
}

void AcarsReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.procMu);
    if (s.fs <= 0 || s.ring.empty()) return;
    size_t left = n;
    while (left) {
        size_t take = std::min<size_t>(left, (size_t)(s.nextDet - s.wr));
        // write to the ring (it may wrap once)
        size_t w = (size_t)(s.wr % s.rsz), first = std::min<size_t>(take, (size_t)s.rsz - w);
        memcpy(&s.ring[w], x, first * sizeof(cf32));
        if (first < take) memcpy(&s.ring[0], x + first, (take - first) * sizeof(cf32));
        s.wr += (int64_t)take; x += take; left -= take;
        if (s.wr >= s.nextDet) {
            s.nextDet += s.blk;
            s.detect();
            s.advance();
            if (s.wr >= s.nextPub) { s.nextPub += (int64_t)(0.25 * s.fs); s.publish(); }
        }
    }
    s.advance();
}

bool AcarsReceiver::telemetry(AcarsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void AcarsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

ModeTuning acarsTuning() {
    ModeTuning t;
    t.stdMode = 18; t.id = "acars"; t.name = "ACARS";
    t.minMhz = 118; t.maxMhz = 137; t.defMhz = 131.5;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 2;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
