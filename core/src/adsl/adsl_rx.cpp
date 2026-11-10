// ADS-L receiver: see adsl_rx.h and adsl_proto.h. The channel paths follow the FLARM receiver (flarm_rx.cpp): mix, low-pass, resample to a whole number of
// samples per chip, FM discriminator, an integrate-and-dump chip value at every sampling phase, the sync hunted at every phase. The Manchester M-band needs no
// threshold (a pair of chips is a bit); the O-band channels threshold against the mean of the last 32 chips, which the balanced preamble makes the carrier offset.
#include "dect2/adsl/adsl_rx.h"
#include "dect2/adsl/adsl_proto.h"
#include "dect2/adsl/adsl_tel.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fast_phase.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/results/results_io.h"
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace dect2 {

namespace {

using dect2::math::kPi;
using namespace adsl;

constexpr int kMaxSps = 8;
constexpr int kRing = 8192;                // chips; a 255 byte Manchester packet is 4096
constexpr double kDefaultCentreWide = 868.875e6, kDefaultCentreNarrow = 868.3e6;
constexpr double kWideRate = 1.6e6;        // from this sample rate the unknown centre is taken as the wide one
constexpr int kDcChips = 32;

// e^{j pi n / 8}, n = 0 .. 15: +50 kHz at 800 ksps
const cf32 kLo16[16] = {{1.f, 0.f}, {0.9238795f, 0.3826834f}, {0.7071068f, 0.7071068f}, {0.3826834f, 0.9238795f}, {0.f, 1.f}, {-0.3826834f, 0.9238795f},
                        {-0.7071068f, 0.7071068f}, {-0.9238795f, 0.3826834f}, {-1.f, 0.f}, {-0.9238795f, -0.3826834f}, {-0.7071068f, -0.7071068f},
                        {-0.3826834f, -0.9238795f}, {0.f, -1.f}, {0.3826834f, -0.9238795f}, {0.7071068f, -0.7071068f}, {0.9238795f, -0.3826834f}};
struct BandCfg { double passHz, stopHz; int sps; bool manchester; uint32_t sync; };
constexpr BandCfg kCfg[kNumBands] = {
    {85e3, 140e3, 8, true, kSyncMBand},
    {85e3, 140e3, 8, true, kSyncMBand},
    {32e3, 60e3, 8, false, kSyncLdr},
    {130e3, 210e3, 4, false, kSyncHdr},
};

struct Cand { int64_t n = 0; int p = 0; int64_t k0 = 0; bool inv = false; int err = 0; float score = 0; float dc = 0; int need = 0; };

struct Chan {
    Band band = BandM0;
    BandCfg cfg{};
    bool on = false;
    int sps = 8;
    double workRate = 800e3, kd = 1;
    double baseHz = 0, phase = 0;
    dmr::Fir<cf32> lp;
    ExactResampler rs;
    bool rsPass = true;
    std::vector<cf32> mixb, decb, ys;
    cf32 prev = cf32(1.f, 0.f);
    int64_t n = 0;
    float dring[kMaxSps] = {};
    cf32 t1[kMaxSps] = {}, t0[kMaxSps] = {};   // M-band tone detector: samples turned by -+50 kHz
    float S = 0;
    std::vector<float> soft;               // sps * kRing
    std::vector<float> pw;
    uint32_t reg[kMaxSps][2] = {};
    float dcSum[kMaxSps] = {};
    float pwAcc = 0;
    double blkSum = 0; size_t blkN = 0;
    std::deque<double> blocks;
    double nf = 0; bool nfKnown = false;
    int64_t lastEnergyN = -(int64_t)1e12;
    bool grpOpen = false; int64_t grpN0 = 0; Cand best;
    std::deque<Cand> jobs;
    std::deque<int64_t> doneK0;

    float& at(int p, int64_t k) { return soft[(size_t)p * kRing + (size_t)(k & (kRing - 1))]; }
    void resetState() {
        lp.reset(); if (!rsPass) rs.reset();
        phase = 0; prev = cf32(1.f, 0.f); n = 0;
        for (float& f : dring) f = 0;
        for (int j = 0; j < kMaxSps; j++) t1[j] = t0[j] = cf32(0.f, 0.f);
        S = 0;
        std::fill(soft.begin(), soft.end(), 0.f);
        std::fill(pw.begin(), pw.end(), 0.f);
        for (auto& r : reg) r[0] = r[1] = 0;
        for (float& f : dcSum) f = 0;
        pwAcc = 0; blkSum = 0; blkN = 0; blocks.clear(); nf = 0; nfKnown = false;
        lastEnergyN = -(int64_t)1e12; grpOpen = false; jobs.clear(); doneK0.clear();
    }
};

struct Rec { AdslAircraft a; double firstSeen = 0, lastSeen = 0, lastTrack = -1e9; };

std::string hex6(uint32_t v) { char b[12]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }

} // namespace

struct AdslReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0, offsetHz = 0, centreHz = 0;
    bool isReady = false;
    Chan ch[kNumBands];
    int64_t nIn = 0;
    double expirySec = 60;
    bool refValid = false; double refLat = 0, refLon = 0;
    std::unordered_map<uint32_t, Rec> rec;   // key: address table << 24 | address
    AdslTelemetry t, pub;
    uint64_t seq = 0;
    double nextReport = 0;
    int lastState = 0;
    uint32_t window[4] = {};
    uint32_t windowCount = 0;
    float levelEma = 0; bool levelInit = false;

    void say(const std::string& s) { if (log) log(s); }
    double now() const { return fs > 0 ? (double)nIn / fs : 0; }
    double centre() const { return centreHz > 0 ? centreHz : (fs >= kWideRate ? kDefaultCentreWide : kDefaultCentreNarrow); }

    // Mix frequencies follow the centre and the offset; a channel whose signal does not fit in the capture is off.
    void placeChannels() {
        std::string on;
        for (int b = 0; b < kNumBands; b++) {
            Chan& c = ch[b];
            c.baseHz = kChannelHz[b] - centre() + offsetHz;
            c.on = isReady && std::fabs(c.baseHz) + c.cfg.passHz < 0.49 * fs;
            t.bandOn[b] = c.on;
        }
    }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        if (fs < adslTuning().minSampleRate - 1) return;
        for (int b = 0; b < kNumBands; b++) {
            Chan& c = ch[b];
            c.band = (Band)b; c.cfg = kCfg[b]; c.sps = c.cfg.sps;
            c.workRate = kChipRate[b] * c.sps;
            c.kd = c.workRate / (2 * kPi * kDeviationHz[b]);
            const int d = std::max(1, (int)std::floor(fs / c.workRate));
            c.lp.design(dmr::lowpassTaps(c.cfg.passHz, std::min(c.cfg.stopHz, fs / d * 0.5 - 1000.0), fs, 50, 9), d);
            const double r = fs / d;
            c.rsPass = std::fabs(r - c.workRate) < 0.01;
            if (!c.rsPass && !c.rs.configure(r, c.workRate)) return;
            c.soft.assign((size_t)c.sps * kRing, 0.f);
            c.pw.assign(kRing, 0.f);
            c.resetState();
        }
        isReady = true;
        resetState();
        placeChannels();
        say("ADS-L: listening on the channels inside the capture");
    }

    void resetState() {
        for (Chan& c : ch) if (!c.soft.empty()) c.resetState();
        nIn = 0; nextReport = 0;
        AdslTelemetry o = t, n;
        n.inputRate = fs;
        n.blocksOk = o.blocksOk; n.blocksBad = o.blocksBad; n.syncs = o.syncs; n.traffic = o.traffic; n.status = o.status; n.uplink = o.uplink; n.fisB = o.fisB;
        n.remoteId = o.remoteId; n.other = o.other; n.encrypted = o.encrypted; n.signedPackets = o.signedPackets; n.corrected = o.corrected; n.maxRangeKm = o.maxRangeKm;
        for (int i = 0; i < 4; i++) { n.perBand[i] = o.perBand[i]; n.bandOn[i] = o.bandOn[i]; }
        t = n;
        for (auto& r : rec) { r.second.lastSeen -= o.timeSec; r.second.firstSeen -= o.timeSec; r.second.lastTrack -= o.timeSec; }
        lastState = 0;
        for (uint32_t& w : window) w = 0;
        windowCount = 0;
    }

    // ---- packets
    void applyTraffic(const Traffic& p, Band band, double tnow, double level, bool uplink, bool trusted) {
        const uint32_t key = (uint32_t)p.amt << 24 | (p.addr & 0xFFFFFF);
        auto it = rec.find(key);
        if (it == rec.end() && !trusted) return;              // a repaired packet can only add to an aircraft already known
        Rec& r = rec[key];
        const bool isNew = r.a.packets == 0;
        if (isNew) { r.a.id = p.addr & 0xFFFFFF; r.a.amt = p.amt; r.firstSeen = tnow; }
        r.lastSeen = tnow;
        AdslAircraft& a = r.a;
        a.packets++;
        a.band = (int)band; a.viaUplink = uplink; a.levelDb = (float)level;
        a.category = p.category; a.flightState = p.flightState; a.emergency = p.emergency;
        a.nic = p.nic; a.hAcc = p.hAcc; a.vAcc = p.vAcc; a.velAcc = p.velAcc;
        a.hasAlt = p.hasAlt; if (p.hasAlt) a.altM = p.altM;
        a.hasSpeed = p.hasSpeed; if (p.hasSpeed) a.speedKmh = (float)(p.speedMs * 3.6);
        a.hasClimb = p.hasClimb; if (p.hasClimb) a.climbMs = (float)p.climbMs;
        a.hasTrack = p.hasTrack; if (p.hasTrack) a.trackDeg = (float)p.trackDeg;
        if (p.hasPos) {
            a.hasPos = true; a.lat = p.lat; a.lon = p.lon;
            if (refValid) {
                double km, br;
                greatCircle(refLat, refLon, p.lat, p.lon, km, br);
                a.hasRange = true; a.distKm = (float)km; a.bearingDeg = (float)br;
                if (km < 1500.0 && km > t.maxRangeKm) t.maxRangeKm = (float)km;
            }
            if (tnow - r.lastTrack > 2.0 && (a.track.empty() || std::fabs(a.track.back().lat - p.lat) + std::fabs(a.track.back().lon - p.lon) > 0.0003)) {
                a.track.push_back({(float)p.lat, (float)p.lon});
                if (a.track.size() > 200) a.track.erase(a.track.begin());
                r.lastTrack = tnow;
            }
        }
        if (isNew && t.blocksOk <= 40) say(std::string("ADS-L: new aircraft, ") + amtName(p.amt) + " " + hex6(p.addr) + " (" + categoryName(p.category) + ")");
    }

    void onPacket(const Parsed& q, Band band, double level, int fixedBits) {
        const double tnow = now();
        const bool trusted = fixedBits == 0;
        t.blocksOk++; t.perBand[band]++; windowCount++;
        if (fixedBits) t.corrected++;
        if (q.signature) t.signedPackets++;
        levelEma = levelInit ? 0.9f * levelEma + 0.1f * (float)level : (float)level; levelInit = true;
        t.levelDb = levelEma;
        switch (q.kind) {
        case Parsed::TrafficMsg: t.traffic++; for (const Traffic& x : q.targets) applyTraffic(x, band, tnow, level, q.relay, trusted); break;
        case Parsed::StatusMsg: {
            t.status++;
            auto it = rec.find((uint32_t)q.amt << 24 | (q.addr & 0xFFFFFF));
            if (it != rec.end()) {
                AdslAircraft& a = it->second.a;
                a.hasStatus = true; a.maxVersion = q.status.maxVersion; a.mRx = q.status.mRx; a.ldrRx = q.status.ldrRx; a.hdrRx = q.status.hdrRx;
                a.mobile = q.status.mobile; a.econRx = q.status.econRx; a.econTx = q.status.econTx; a.xpdr = q.status.xpdr;
            }
            break;
        }
        case Parsed::UplinkMsg: t.uplink++; for (const Traffic& x : q.targets) applyTraffic(x, band, tnow, level, true, trusted); break;
        case Parsed::Encrypted: t.encrypted++; break;
        case Parsed::Other:
            if (q.type == TypeFisB) t.fisB++;
            else if (q.type == TypeRemoteId) t.remoteId++;
            else t.other++;
            break;
        default: t.other++; break;
        }
    }

    // A byte of the frame at the given bit offset, with the margin of each bit kept for the repair
    int chipsPerBit(const Chan& c) const { return c.cfg.manchester ? 2 : 1; }
    void readByte(Chan& c, const Cand& cd, int byteIdx, uint8_t& out, float* margin) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            const int64_t base = cd.k0 + 1 + (int64_t)(byteIdx * 8 + i) * chipsPerBit(c);
            int bit; float m;
            if (c.cfg.manchester) {
                const float a = c.at(cd.p, base), b = c.at(cd.p, base + 1);
                bit = b > a ? 1 : 0; m = std::fabs(b - a);
            } else {
                const float a = c.at(cd.p, base) - cd.dc;
                bit = a > 0 ? 1 : 0; m = std::fabs(a);
            }
            if (cd.inv) bit ^= 1;
            v = (uint8_t)(v << 1 | bit);
            if (margin) margin[i] = m;
        }
        out = v;
    }
    int headerBytes(const Chan& c) const { return c.band == BandLdr ? kLdrHeaderBytes : 1; }

    double packetLevel(Chan& c, const Cand& cd, int bytes) {
        const int64_t kEnd = cd.k0 + (int64_t)bytes * 8 * chipsPerBit(c);
        double pwr = 0; int cnt = 0;
        for (int64_t k = cd.k0 - 16; k <= kEnd; k++, cnt++) pwr += c.pw[(size_t)(k & (kRing - 1))];
        pwr /= (double)cnt * c.sps;
        const double nf = c.nfKnown ? c.nf : 1e-12;
        return 10 * std::log10(std::max(pwr - nf, nf * 0.01) / nf);
    }

    // A whole network packet of n bytes starting after the data link header: parity, repair, parse.
    bool tryNet(Chan& c, const Cand& cd, int start, int n, double level) {
        std::vector<uint8_t> net((size_t)n);
        std::vector<float> margin((size_t)n * 8);
        for (int i = 0; i < n; i++) readByte(c, cd, start + i, net[(size_t)i], margin.data() + 8 * i);
        int fixed = checkParity(net.data(), net.size());
        if (fixed < 0 && n == kTrafficNetBytes) {
            std::vector<int> order((size_t)n * 8);
            for (int i = 0; i < n * 8; i++) order[(size_t)i] = i;
            std::partial_sort(order.begin(), order.begin() + 4, order.end(), [&](int a, int b) { return margin[(size_t)a] < margin[(size_t)b]; });
            fixed = correctCrc(net.data(), order.data(), 4);
        }
        if (fixed < 0) return false;
        const Parsed q = parseNetwork(net.data(), net.size());
        if (q.kind == Parsed::Bad || q.kind == Parsed::Reserved) return false;
        onPacket(q, c.band, level, fixed);
        return true;
    }

    void tryJob(Chan& c, const Cand& cd) {
        bool ok = false;
        const int hb = headerBytes(c);
        const double level = packetLevel(c, cd, hb + 32);
        if (c.band == BandLdr) ok = tryNet(c, cd, kLdrHeaderBytes, kTrafficNetBytes, level);
        else {
            uint8_t len = 0;
            readByte(c, cd, 0, len, nullptr);
            int cands[3], nc = 0;
            if (len >= 8) cands[nc++] = len;                 // the length byte itself has no parity: fall back on the usual sizes
            if (len != kTrafficNetBytes) cands[nc++] = kTrafficNetBytes;
            if (len != kStatusNetBytes) cands[nc++] = kStatusNetBytes;
            for (int i = 0; i < nc && !ok; i++) ok = tryNet(c, cd, 1, cands[i], level);
        }
        if (!ok && cd.err <= 1) t.blocksBad++;
    }

    void commitGroup(Chan& c) {
        c.grpOpen = false;
        t.syncs++;
        Cand j = c.best;
        // Wait for the fixed part first; a length field stretches it below.
        j.need = c.band == BandLdr ? kLdrHeaderBytes + kTrafficNetBytes : 1 + std::max(kTrafficNetBytes, kStatusNetBytes);
        c.jobs.push_back(j);
        c.doneK0.push_back(j.k0);
        while (c.doneK0.size() > 8) c.doneK0.pop_front();
    }

    void processChan(Chan& c, const cf32* x, size_t n) {
        c.mixb.resize(n);
        const double w = -2 * kPi * c.baseHz / fs;
        const cf32 step((float)std::cos(w), (float)std::sin(w));
        cf32 rot((float)std::cos(c.phase), (float)std::sin(c.phase));
        for (size_t i = 0; i < n; i++) { c.mixb[i] = x[i] * rot; rot *= step; }
        c.phase = std::fmod(c.phase + w * (double)n, 2 * kPi);
        c.decb.clear();
        c.lp.process(c.mixb.data(), n, c.decb);
        const std::vector<cf32>* y = &c.decb;
        if (!c.rsPass) { c.ys.clear(); c.rs.process(c.decb.data(), c.decb.size(), c.ys); y = &c.ys; }
        const float kd = (float)c.kd;
        const int sps = c.sps;
        const uint32_t mask = 0xFFFFFFu;
        const size_t blockLen = (size_t)(c.workRate * 0.02);
        for (const cf32& s : *y) {
            const cf32 q = s * std::conj(c.prev);
            const float d = fastAtan2(q.imag(), q.real()) * kd;   // 3.9: fastAtan2 instead of the libm call
            c.prev = s;
            const int p = (int)(c.n % sps);
            const int64_t k = c.n / sps;
            const float pwr = s.real() * s.real() + s.imag() * s.imag();
            c.pwAcc = p == 0 ? pwr : c.pwAcc + pwr;
            if (p == sps - 1) c.pw[(size_t)(k & (kRing - 1))] = c.pwAcc;
            c.blkSum += pwr;
            if (++c.blkN >= blockLen) blockEnd(c);
            if (c.cfg.manchester && sps == 8 && AdslReceiver::toneDetect) {
                // M-band chips (3.9, as FLARM): the energy at +50 kHz minus that at -50 kHz over one chip, the noncoherent detector of the h = 1 GFSK
                const int ph16 = (int)(c.n & 15);
                c.t1[p] = s * kLo16[(16 - ph16) & 15];
                c.t0[p] = s * kLo16[ph16];
                cf32 b1(0.f, 0.f), b0(0.f, 0.f);
                for (int j = 0; j < 8; j++) { b1 += c.t1[j]; b0 += c.t0[j]; }
                c.S = std::norm(b1) - std::norm(b0);
            } else {
                c.S += d - c.dring[p];
                c.dring[p] = d;
            }
            c.at(p, k) = c.S;
            uint32_t* rg; float dc = 0;
            if (c.cfg.manchester) {
                const int bit = c.S > c.at(p, k - 1) ? 1 : 0;
                rg = &c.reg[p][k & 1];
                *rg = (*rg << 1 | (uint32_t)bit) & mask;
            } else {
                c.dcSum[p] += c.S - c.at(p, k - kDcChips);
                dc = c.dcSum[p] / kDcChips;
                rg = &c.reg[p][0];
                *rg = (*rg << 1 | (c.S > dc ? 1u : 0u)) & mask;
            }
            const int e = dect2::bits::popcount(*rg ^ c.cfg.sync);
            const int ei = kSyncBits - e;
            if (c.n > 400 && (e <= 2 || ei <= 2)) {
                bool dup = false;
                for (int64_t q : c.doneK0) if (std::llabs(q - k) <= 3 * (c.cfg.manchester ? 2 : 1)) dup = true;
                if (!dup) {
                    float score = 0;
                    if (c.cfg.manchester) for (int j = 0; j < kSyncBits; j++) score += std::fabs(c.at(p, k - 2 * j) - c.at(p, k - 2 * j - 1));
                    else for (int j = 0; j < kSyncBits; j++) score += std::fabs(c.at(p, k - j) - dc);
                    Cand cd; cd.n = c.n; cd.p = p; cd.k0 = k; cd.inv = ei < e; cd.err = std::min(e, ei); cd.score = score; cd.dc = dc;
                    if (!c.grpOpen) { c.grpOpen = true; c.grpN0 = c.n; c.best = cd; }
                    else if (score > c.best.score) c.best = cd;
                }
            }
            if (c.grpOpen && c.n >= c.grpN0 + 3 * sps) commitGroup(c);
            while (!c.jobs.empty()) {
                Cand& j = c.jobs.front();
                const int cpb = chipsPerBit(c);
                const int64_t endChip = j.k0 + (int64_t)j.need * 8 * cpb + 1;
                if (c.n < sps * endChip + j.p + 1) break;
                if (c.band != BandLdr && j.need <= 1 + kTrafficNetBytes) {    // the length byte is in: wait for all of a longer packet
                    uint8_t len = 0;
                    readByte(c, j, 0, len, nullptr);
                    if (len > kTrafficNetBytes) { j.need = 1 + len; continue; }
                }
                const Cand cd = j;
                c.jobs.pop_front();
                tryJob(c, cd);
            }
            c.n++;
        }
    }

    void blockEnd(Chan& c) {
        const double mean = c.blkSum / (double)c.blkN;
        c.blocks.push_back(mean);
        if (c.blocks.size() > 250) c.blocks.pop_front();
        if (c.blocks.size() >= 3) {
            std::vector<double> s(c.blocks.begin(), c.blocks.end());
            std::sort(s.begin(), s.end());
            c.nf = std::max(s[s.size() / 10] * 1.05, 1e-12);
            c.nfKnown = true;
        }
        if (c.nfKnown && mean > 1.6 * c.nf) c.lastEnergyN = c.n;
        c.blkSum = 0; c.blkN = 0;
    }

    void expire(double tnow) { for (auto it = rec.begin(); it != rec.end();) { if (tnow - it->second.lastSeen > expirySec) it = rec.erase(it); else ++it; } }

    void publish() {
        const double tnow = now();
        expire(tnow);
        t.timeSec = tnow;
        bool energy = false;
        double nf = 0; int nn = 0;
        for (Chan& c : ch) if (c.on) { if ((c.n - c.lastEnergyN) < (int64_t)(5 * c.workRate)) energy = true; if (c.nfKnown) { nf += c.nf; nn++; } }
        t.dataValid = !rec.empty();
        t.state = !rec.empty() ? 2 : (energy ? 1 : 0);
        t.snrDb = levelEma;
        t.noiseDb = nn ? (float)(10 * std::log10(nf / nn + 1e-20)) : -120.f;
        t.refValid = refValid; t.refLat = refLat; t.refLon = refLon;
        if (t.state != lastState) {
            say(t.state == 2 ? "ADS-L: receiving aircraft" : t.state == 1 ? "ADS-L: energy on a channel, no good packet lately" : "ADS-L: no signal");
            lastState = t.state;
        }
        AdslTelemetry o = t;
        o.seq = ++seq;
        o.aircraftCount = (uint32_t)rec.size();
        o.withPosition = 0;
        std::vector<const Rec*> v;
        for (auto& r : rec) { v.push_back(&r.second); if (r.second.a.hasPos) o.withPosition++; }
        std::sort(v.begin(), v.end(), [](const Rec* a, const Rec* b) { return a->lastSeen > b->lastSeen; });
        for (size_t i = 0; i < v.size() && i < 128; i++) { AdslAircraft a = v[i]->a; a.ageSec = (float)(tnow - v[i]->lastSeen); o.aircraft.push_back(std::move(a)); }
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        for (Chan& c : ch) if (c.on) processChan(c, x, n);
        nIn += (int64_t)n;
        while (now() >= nextReport) {
            nextReport += 0.25;
            window[3] = window[2]; window[2] = window[1]; window[1] = window[0]; window[0] = windowCount; windowCount = 0;
            t.packetsPerSec = (float)(window[0] + window[1] + window[2] + window[3]);
            publish();
        }
    }
};

AdslReceiver::AdslReceiver() : p_(std::make_unique<Impl>()) {}
AdslReceiver::~AdslReceiver() = default;

void AdslReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->procMu); p_->configure(r); }
void AdslReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->offsetHz = hz; p_->placeChannels(); }
void AdslReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->centreHz = hz; p_->placeChannels(); }
double AdslReceiver::flarmOffsetHz() const { std::lock_guard<std::mutex> lk(p_->procMu); return 868.3e6 - p_->centre() + p_->offsetHz; }
void AdslReceiver::setReference(double la, double lo) { std::lock_guard<std::mutex> lk(p_->procMu); p_->refValid = true; p_->refLat = la; p_->refLon = lo; }
void AdslReceiver::clearReference() { std::lock_guard<std::mutex> lk(p_->procMu); p_->refValid = false; }
void AdslReceiver::setExpiry(double s) { std::lock_guard<std::mutex> lk(p_->procMu); p_->expirySec = std::max(5.0, s); }
bool AdslReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void AdslReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->isReady) p_->resetState(); }
void AdslReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->rec.clear();
    const double rate = p_->fs;
    bool on[4]; for (int i = 0; i < 4; i++) on[i] = p_->t.bandOn[i];
    p_->t = AdslTelemetry();
    p_->t.inputRate = rate;
    for (int i = 0; i < 4; i++) p_->t.bandOn[i] = on[i];
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.aircraft.clear();
    p_->pub.aircraftCount = 0; p_->pub.withPosition = 0;
    p_->pub.seq = ++p_->seq;
}
std::string AdslReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    std::vector<AdslAircraft> v;
    for (auto& r : p_->rec) v.push_back(r.second.a);
    return results::pack(p_->t.timeSec, v);
}
bool AdslReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<AdslAircraft> v;
    if (!results::unpack(blob, t0, v)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    for (auto& a : v) { Rec r; r.a = a; r.firstSeen = r.lastSeen = p_->t.timeSec; p_->rec[(uint32_t)a.amt << 24 | a.id] = r; }
    return true;
}
void AdslReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
bool AdslReceiver::telemetry(AdslTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void AdslReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

ModeTuning adslTuning() {
    ModeTuning t;
    t.stdMode = 302; t.id = "adsl"; t.name = "ADS-L";
    t.minMhz = 860; t.maxMhz = 930; t.defMhz = 868.875;
    t.sampleRate = 2000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.75;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
