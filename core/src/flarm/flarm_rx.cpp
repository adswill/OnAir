// FLARM receiver: see flarm_rx.h and flarm_proto.h.
// Two channel paths (868.2 / 868.4 MHz = -100 / +100 kHz around the radio's centre): mix, low-pass, resample to 800 ksps (8 per chip), FM
// discriminator, then at each of the 8 sampling phases an integrate-and-dump chip value. A pair of chips is a data bit (second chip above the first
// = 1), so the carrier offset needs no threshold. The 32 sync bits are hunted at every phase and chip parity; the best phase of a match
// collects 26 bytes, the CRC and the V6 parity decide.
#include "dect2/flarm/flarm_rx.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fast_phase.h"
#include "dect2/flarm/flarm_proto.h"
#include "dect2/flarm/flarm_tel.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kWorkRate = 800000.0;     // 8 samples per chip
constexpr int kSps = 8;
constexpr int kRing = 1024;
constexpr int kDataChips = flarm::kFrameBytes * 16;      // 416
constexpr double kChanOffsetHz = 100000.0;
constexpr size_t kBlock = 16000;           // 20 ms noise blocks
constexpr size_t kNoiseBlocks = 250;
// e^{j 2 pi 50 kHz n / 800 kHz} = e^{j pi n / 8}, n = 0 .. 15
const cf32 kLo[16] = {{1.f, 0.f}, {0.9238795f, 0.3826834f}, {0.7071068f, 0.7071068f}, {0.3826834f, 0.9238795f}, {0.f, 1.f}, {-0.3826834f, 0.9238795f},
                      {-0.7071068f, 0.7071068f}, {-0.9238795f, 0.3826834f}, {-1.f, 0.f}, {-0.9238795f, -0.3826834f}, {-0.7071068f, -0.7071068f},
                      {-0.3826834f, -0.9238795f}, {0.f, -1.f}, {0.3826834f, -0.9238795f}, {0.7071068f, -0.7071068f}, {0.9238795f, -0.3826834f}};

int popcount32(uint32_t v) { return dect2::bits::popcount(v); }

struct Cand { int64_t n = 0; int p = 0; int64_t k0 = 0; bool inv = false; int err = 0; float score = 0; };

struct Chan {
    int idx = 0;
    double offHz = 0, phase = 0;
    dmr::Fir<cf32> lp;
    ExactResampler rs;
    bool rsPass = true;
    std::vector<cf32> mixb, decb, ys;
    cf32 prev = cf32(1.f, 0.f);
    int64_t n = 0;
    float dring[kSps] = {};
    cf32 t1[kSps] = {}, t0[kSps] = {};   // samples turned by -+50 kHz (tone detector)
    float toneScale = 1.f;
    float S = 0;
    float soft[kSps][kRing] = {};
    uint32_t reg[kSps][2] = {};
    float pw[kRing] = {};
    float pwAcc = 0;
    double blkSum = 0; size_t blkN = 0;
    std::deque<double> blocks;
    double nf = 0; bool nfKnown = false;
    int64_t lastEnergyN = -(int64_t)1e12;
    bool grpOpen = false; int64_t grpN0 = 0; Cand best;
    std::deque<Cand> jobs;
    std::deque<int64_t> doneK0;      // sync positions already taken (chips), so a neighbouring phase of the same packet is not a second packet

    void resetState() {
        lp.reset(); if (!rsPass) rs.reset();
        phase = 0; prev = cf32(1.f, 0.f); n = 0;
        for (float& f : dring) f = 0;
        for (int j = 0; j < kSps; j++) t1[j] = t0[j] = cf32(0.f, 0.f);
        S = 0;
        for (auto& r : soft) for (float& f : r) f = 0;
        for (auto& r : reg) r[0] = r[1] = 0;
        for (float& f : pw) f = 0;
        pwAcc = 0; blkSum = 0; blkN = 0; blocks.clear(); nf = 0; nfKnown = false;
        lastEnergyN = -(int64_t)1e12; grpOpen = false; jobs.clear(); doneK0.clear();
    }
};

struct Rec {
    FlarmAircraft a;
    double firstSeen = 0, lastSeen = 0, lastTrack = -1e9;
    uint32_t lat19 = 0, lon20 = 0;      // the last packet's raw position bits, to tell the right key time
};

} // namespace

struct FlarmReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0, offsetHz = 0;
    bool isReady = false;
    Chan ch[2];
    int64_t nIn = 0;                         // input samples since the start
    bool wallClock = true;
    double unix0 = 0;
    double expirySec = 60;
    bool refValid = false; double refLat = 0, refLon = 0;

    std::unordered_map<uint32_t, Rec> rec;
    std::unordered_set<uint64_t> privSet, newSet;       // salted hashes, in memory only: nothing about a flagged aircraft is kept but the count
    uint64_t salt = 0;
    FlarmTelemetry t, pub;
    uint64_t seq = 0;
    double nextReport = 0;
    int lastState = 0;
    uint32_t window[4] = {};
    double windowStart = 0;
    uint32_t windowCount = 0;
    float levelEma = 0; bool levelInit = false;

    Impl() { std::random_device rd; salt = ((uint64_t)rd() << 32) ^ rd() ^ 0x9E3779B97F4A7C15ULL; }

    void say(const std::string& s) { if (log) log(s); }
    double now() const { return fs > 0 ? (double)nIn / fs : 0; }
    double unixNow() const {
        if (wallClock) return (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
        return unix0 + now();
    }
    uint64_t hashId(uint32_t id) const { uint64_t h = (uint64_t)id ^ salt; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33; return h; }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        if (fs < flarmTuning().minSampleRate - 1) return;
        const int d = std::max(1, (int)std::floor(fs / kWorkRate));
        for (int c = 0; c < 2; c++) {
            Chan& h = ch[c];
            h.idx = c;
            h.offHz = (c ? kChanOffsetHz : -kChanOffsetHz);
            h.lp.design(dmr::lowpassTaps(85000.0, std::min(140000.0, fs / d * 0.5 - 1000.0), fs, 50, 9), d);
            const double r = fs / d;
            h.rsPass = std::fabs(r - kWorkRate) < 0.01;
            if (!h.rsPass && !h.rs.configure(r, kWorkRate)) return;
            h.resetState();
        }
        isReady = true;
        resetState();
        say("FLARM: listening on 868.2 and 868.4 MHz (old protocol decoded, newer protocol counted)");
    }

    void resetState() {
        for (Chan& h : ch) h.resetState();
        nIn = 0; nextReport = 0;
        const FlarmTelemetry old = t;
        FlarmTelemetry n;
        n.inputRate = fs;
        n.blocksOk = old.blocksOk; n.blocksBad = old.blocksBad; n.newProtocol = old.newProtocol; n.otherType = old.otherType;
        n.perChannel[0] = old.perChannel[0]; n.perChannel[1] = old.perChannel[1]; n.syncs = old.syncs; n.privacyPackets = old.privacyPackets;
        n.maxRangeKm = old.maxRangeKm;
        t = n;
        for (auto& r : rec) { r.second.lastSeen -= old.timeSec; r.second.firstSeen -= old.timeSec; r.second.lastTrack -= old.timeSec; }
        lastState = 0;
        for (uint32_t& w : window) w = 0;
        windowCount = 0; windowStart = 0;
    }

    // ---- packets
    void onFrame(Chan& c, const Cand& cd, const uint8_t* f, double level) {
        t.perChannel[c.idx]++;
        const int type = flarm::packetType(f);
        const uint32_t id = flarm::packetAddr(f);
        if (type == 2) {
            t.newProtocol++;
            if (newSet.insert(hashId(id)).second) t.newProtocolDevices = newSet.size();
            return;
        }
        if (type != 0) { t.otherType++; return; }
        // V6: the key needs the time (64 s steps): the computer's clock, and the neighbouring step near a boundary
        const double un = unixNow();
        const uint32_t ts0 = (uint32_t)un;
        uint32_t cands[3]; int nc = 0;
        cands[nc++] = ts0;
        const uint32_t frac = ts0 & 63;
        if (frac >= 52) cands[nc++] = ts0 + 64;
        if (frac < 12) cands[nc++] = ts0 - 64;
        // every key candidate whose parity holds: a wrong one passes half of the time, so the right one is told by agreeing with the aircraft's other packets
        std::vector<flarm::V6> D;
        for (int i = 0; i < nc; i++) { flarm::V6 q; if (flarm::decodeV6(f, cands[i], q) == flarm::DecOk) D.push_back(q); }
        if (D.empty()) { t.blocksBad++; return; }
        // A wrong key still passes the parity half of the time, and garbage can set any flag: an aircraft counts only when a second packet of the
        // same address follows within a minute and agrees with the first (type, flags, rough position). Until then only a salted hash is kept.
        const double tn0 = now();
        const uint64_t h = hashId(id);
        auto dmod = [](uint32_t a, uint32_t b, uint32_t m) { const uint32_t d = (a - b) & m; return std::min(d, (m + 1) - d); };
        auto isFlagged = [](const flarm::V6& q) { return q.noTrack || q.stealth || q.addrType == flarm::AddrAnonymous; };
        auto nearBy = [&](const flarm::V6& q, int acType, bool fl, uint32_t la, uint32_t lo) {
            return isFlagged(q) == fl && q.aircraftType == acType && dmod(la, q.lat19, 0x7FFFF) < 4000 && dmod(lo, q.lon20, 0xFFFFF) < 4000;
        };
        const auto rit = rec.find(id);
        if (rit != rec.end()) {                         // a known aircraft: the candidate that agrees with its last packet
            for (const flarm::V6& q : D)
                if (!isFlagged(q) && nearBy(q, rit->second.a.acType, false, rit->second.lat19, rit->second.lon20)) { apply(q, tn0, c.idx, level); return; }
        }
        if (privSet.count(h)) {                         // a known privacy-flagged aircraft: only counted
            for (const flarm::V6& q : D) if (isFlagged(q)) { t.privacyPackets++; return; }
            return;
        }
        for (auto it = pending.begin(); it != pending.end();) { if (tn0 - it->second.front().t > 60.0) it = pending.erase(it); else ++it; }
        auto pit = pending.find(h);
        if (pit != pending.end()) {
            for (const flarm::V6& q : D)
                for (const Pending& pd : pit->second)
                    if (nearBy(q, pd.acType, pd.flagged, pd.lat19, pd.lon20)) {
                        const Pending first = pd;
                        pending.erase(pit);
                        if (isFlagged(q)) { t.privacyPackets += 2; privSet.insert(h); t.privacyFlagged = privSet.size(); return; }   // nothing but a count survives a flagged packet
                        apply(first.p, first.t, first.ch, first.level);
                        apply(q, tn0, c.idx, level);
                        return;
                    }
        }
        std::vector<Pending> v;
        for (const flarm::V6& q : D) {
            Pending pd; pd.t = tn0; pd.flagged = isFlagged(q); pd.acType = q.aircraftType; pd.lat19 = q.lat19; pd.lon20 = q.lon20;
            if (!pd.flagged) { pd.p = q; pd.level = level; pd.ch = c.idx; }
            v.push_back(pd);
        }
        pending[h] = std::move(v);
        (void)cd;
    }

    struct Pending { double t = 0; bool flagged = false; int acType = 0; uint32_t lat19 = 0, lon20 = 0; flarm::V6 p; double level = 0; int ch = 0; };
    std::unordered_map<uint64_t, std::vector<Pending>> pending;

    void apply(const flarm::V6& p, double tnow, int chan, double level) {
        t.blocksOk++;
        Rec& r = rec[p.addr];
        const bool isNew = r.a.packets == 0;
        if (isNew) { r.a.id = p.addr; r.firstSeen = tnow; }
        r.lastSeen = tnow;
        r.lat19 = p.lat19; r.lon20 = p.lon20;
        r.a.addrType = p.addrType; r.a.acType = p.aircraftType; r.a.packets++;
        r.a.altM = (int)p.alt; r.a.airborne = p.airborne; r.a.channel = chan;
        r.a.levelDb = (float)level;
        double sp, tr, cl;
        flarm::unpackVelocity(p, sp, tr, cl);
        r.a.speedKmh = (float)(sp * 3.6); r.a.trackDeg = (float)tr; r.a.climbMs = (float)cl;
        if (refValid) {
            double la, lo, km, br;
            flarm::unpackPosition(p.lat19, p.lon20, refLat, refLon, la, lo);
            greatCircle(refLat, refLon, la, lo, km, br);
            if (km < 600.0) {                      // farther away is an alias of the 19 / 20 bit position, not an aircraft
                r.a.hasPos = true; r.a.lat = la; r.a.lon = lo;
                r.a.hasRange = true; r.a.distKm = (float)km; r.a.bearingDeg = (float)br;
                if (km > t.maxRangeKm) t.maxRangeKm = (float)km;
                if (tnow - r.lastTrack > 2.0 && (r.a.track.empty() || std::fabs(r.a.track.back().lat - la) + std::fabs(r.a.track.back().lon - lo) > 0.0003)) {
                    r.a.track.push_back({(float)la, (float)lo});
                    if (r.a.track.size() > 200) r.a.track.erase(r.a.track.begin());
                    r.lastTrack = tnow;
                }
            }
        }
        windowCount++;
        levelEma = levelInit ? 0.9f * levelEma + 0.1f * (float)level : (float)level; levelInit = true;
        t.levelDb = levelEma;
        if (isNew && t.blocksOk <= 40) say(std::string("FLARM: new aircraft, device ") + hex6(p.addr) + " (" + flarm::aircraftTypeName(p.aircraftType) + ")");
    }
    static std::string hex6(uint32_t v) { char b[12]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }

    void tryJob(Chan& c, const Cand& cd) {
        uint8_t raw[flarm::kFrameBytes] = {};
        for (int i = 0; i < kDataChips / 2; i++) {
            const float a = c.soft[cd.p][(cd.k0 + 1 + 2 * i) & (kRing - 1)];
            const float b = c.soft[cd.p][(cd.k0 + 2 + 2 * i) & (kRing - 1)];
            const int bit = ((b > a) ? 1 : 0) ^ (cd.inv ? 1 : 0);
            raw[i >> 3] = (uint8_t)((raw[i >> 3] << 1) | bit);
        }
        uint8_t f[flarm::kFrameBytes];
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; pass++) {
            for (int i = 0; i < flarm::kFrameBytes; i++) f[i] = pass ? (uint8_t)~raw[i] : raw[i];
            ok = flarm::checkCrc(f);
        }
        // Chase decoding (3.9): the 6 bits with the smallest chip difference are the likely errors; the 63 ways of flipping them are tried in the
        // order of their summed margins, the first that the CRC accepts is taken. The V6 parity and the two-packet agreement of onFrame still
        // stand behind it, so a wrong pick (about 2 in 1000 for a noise sync) does not make an aircraft.
        if (!ok && FlarmReceiver::chaseOn) {
            constexpr int L = 6;
            int wi[L]; float wr[L];
            for (int j = 0; j < L; j++) { wi[j] = -1; wr[j] = 1e30f; }
            for (int i = 0; i < kDataChips / 2; i++) {
                const float r = std::fabs(c.soft[cd.p][(cd.k0 + 2 + 2 * i) & (kRing - 1)] - c.soft[cd.p][(cd.k0 + 1 + 2 * i) & (kRing - 1)]);
                if (r >= wr[L - 1]) continue;
                int j = L - 1;
                while (j > 0 && wr[j - 1] > r) { wr[j] = wr[j - 1]; wi[j] = wi[j - 1]; j--; }
                wr[j] = r; wi[j] = i;
            }
            int order[(1 << L) - 1]; float cost[1 << L];
            for (int m = 1; m < (1 << L); m++) { float sc = 0; for (int j = 0; j < L; j++) if (m >> j & 1) sc += wr[j]; cost[m] = sc; order[m - 1] = m; }
            std::sort(order, order + (1 << L) - 1, [&](int x, int y) { return cost[x] < cost[y]; });
            for (int q = 0; q < (1 << L) - 1 && !ok; q++) {
                const int m = order[q];
                for (int pass = 0; pass < 2 && !ok; pass++) {
                    for (int i = 0; i < flarm::kFrameBytes; i++) f[i] = pass ? (uint8_t)~raw[i] : raw[i];
                    for (int j = 0; j < L; j++) if ((m >> j & 1) && wi[j] >= 0) f[wi[j] >> 3] ^= (uint8_t)(0x80 >> (wi[j] & 7));
                    ok = flarm::checkCrc(f);
                }
            }
        }
        // level over the packet: chip powers of the sync and data, over the noise floor
        double pwr = 0;
        const int64_t kEnd = cd.k0 + kDataChips;
        int cnt = 0;
        for (int64_t k = cd.k0 - 31; k <= kEnd; k++, cnt++) pwr += c.pw[k & (kRing - 1)];
        pwr /= (double)cnt * kSps;
        const double nf = c.nfKnown ? c.nf : 1e-12;
        const double snr = 10 * std::log10(std::max(pwr - nf, nf * 0.01) / nf);
        if (ok) { onFrame(c, cd, f, snr); }
        else if (cd.err <= 1) t.blocksBad++;
    }

    void commitGroup(Chan& c) {
        c.grpOpen = false;
        t.syncs++;
        c.jobs.push_back(c.best);
        c.doneK0.push_back(c.best.k0);
        while (c.doneK0.size() > 8) c.doneK0.pop_front();
    }

    void processChan(Chan& c, const cf32* x, size_t n) {
        // mix the channel down
        c.mixb.resize(n);
        const double w = -2 * kPi * (c.offHz + offsetHz) / fs;
        const cf32 step((float)std::cos(w), (float)std::sin(w));
        cf32 rot((float)std::cos(c.phase), (float)std::sin(c.phase));
        for (size_t i = 0; i < n; i++) { c.mixb[i] = x[i] * rot; rot *= step; }
        c.phase += w * (double)n;
        c.phase = std::fmod(c.phase, 2 * kPi);
        c.decb.clear();
        c.lp.process(c.mixb.data(), n, c.decb);
        const std::vector<cf32>* y = &c.decb;
        if (!c.rsPass) { c.ys.clear(); c.rs.process(c.decb.data(), c.decb.size(), c.ys); y = &c.ys; }
        const float kd = (float)(kWorkRate / (2 * kPi) / 50000.0);
        for (const cf32& s : *y) {
            const cf32 q = s * std::conj(c.prev);
            const float d = fastAtan2(q.imag(), q.real()) * kd;   // (std::arg: the libm call was most of the cost)
            c.prev = s;
            const int p = (int)(c.n & 7);
            const int64_t k = c.n >> 3;
            // power
            const float pwr = s.real() * s.real() + s.imag() * s.imag();
            c.pwAcc = p == 0 ? pwr : c.pwAcc + pwr;
            if (p == 7) c.pw[k & (kRing - 1)] = c.pwAcc;
            c.blkSum += pwr;
            if (++c.blkN >= kBlock) blockEnd(c);
            if (FlarmReceiver::toneDetect) {
                // chip value (3.9): the energy at +50 kHz minus the energy at -50 kHz over the last 8 samples (one chip), the noncoherent
                // detector of orthogonal FSK (h = 1); below about 12 dB the discriminator's clicks cost about 1 dB more
                const int ph16 = (int)(c.n & 15);
                c.t1[p] = s * kLo[(16 - ph16) & 15];
                c.t0[p] = s * kLo[ph16];
                cf32 b1(0.f, 0.f), b0(0.f, 0.f);
                for (int j = 0; j < kSps; j++) { b1 += c.t1[j]; b0 += c.t0[j]; }
                c.S = (std::norm(b1) - std::norm(b0)) * c.toneScale;
            } else {
                // chip value: the sum of the last 8 discriminator samples
                c.S += d - c.dring[p];
                c.dring[p] = d;
            }
            c.soft[p][k & (kRing - 1)] = c.S;
            // the pair that ends at this chip
            const float prevChip = c.soft[p][(k - 1) & (kRing - 1)];
            const int bit = c.S > prevChip ? 1 : 0;
            uint32_t& r = c.reg[p][k & 1];
            r = (r << 1) | (uint32_t)bit;
            const int e = popcount32(r ^ flarm::kSyncWord);
            const int ei = 32 - e;
            bool dupSync = false;
            if (c.n > 400 && (e <= 2 || ei <= 2)) for (int64_t d : c.doneK0) if (std::llabs(d - k) <= 6) dupSync = true;
            if (c.n > 400 && !dupSync && (e <= 2 || ei <= 2)) {
                float score = 0;
                for (int j = 0; j < 32; j++) score += std::fabs(c.soft[p][(k - 2 * j) & (kRing - 1)] - c.soft[p][(k - 2 * j - 1) & (kRing - 1)]);
                Cand cd; cd.n = c.n; cd.p = p; cd.k0 = k; cd.inv = ei < e; cd.err = std::min(e, ei); cd.score = score;
                if (!c.grpOpen) { c.grpOpen = true; c.grpN0 = c.n; c.best = cd; }
                else if (score > c.best.score) c.best = cd;
            }
            if (c.grpOpen && c.n >= c.grpN0 + 24) commitGroup(c);
            // a packet whose data is all in
            while (!c.jobs.empty()) {
                const Cand& j = c.jobs.front();
                if (c.n < 8 * (j.k0 + kDataChips) + j.p + 1) break;
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
        if (c.blocks.size() > kNoiseBlocks) c.blocks.pop_front();
        if (c.blocks.size() >= 3) {
            std::vector<double> s(c.blocks.begin(), c.blocks.end());
            std::sort(s.begin(), s.end());
            c.nf = std::max(s[s.size() / 10] * 1.05, 1e-12);
            c.nfKnown = true;
        }
        if (c.nfKnown && mean > 1.6 * c.nf) c.lastEnergyN = c.n;
        c.blkSum = 0; c.blkN = 0;
    }

    void expire(double tnow) {
        for (auto it = rec.begin(); it != rec.end();) {
            if (tnow - it->second.lastSeen > expirySec) it = rec.erase(it); else ++it;
        }
    }

    void publish() {
        const double tnow = now();
        expire(tnow);
        t.timeSec = tnow;
        const bool energy = (ch[0].n - ch[0].lastEnergyN) < (int64_t)(5 * kWorkRate) || (ch[1].n - ch[1].lastEnergyN) < (int64_t)(5 * kWorkRate);
        t.dataValid = !rec.empty() || (t.newProtocol > 0 && tnow - lastNewAt < 30);
        t.state = !rec.empty() ? 2 : (energy ? 1 : 0);
        t.snrDb = levelEma;
        double nf = 0; int nn = 0;
        for (Chan& c : ch) if (c.nfKnown) { nf += c.nf; nn++; }
        t.noiseDb = nn ? (float)(10 * std::log10(nf / nn + 1e-20)) : -120.f;
        t.refValid = refValid; t.refLat = refLat; t.refLon = refLon;
        t.clockFromWall = wallClock;
        if (t.state != lastState) {
            say(t.state == 2 ? "FLARM: receiving aircraft" : t.state == 1 ? "FLARM: energy on a channel, no good packet lately" : "FLARM: no signal");
            lastState = t.state;
        }
        FlarmTelemetry o = t;
        o.seq = ++seq;
        o.aircraftCount = (uint32_t)rec.size();
        o.withPosition = 0;
        std::vector<const Rec*> v;
        for (auto& r : rec) { v.push_back(&r.second); if (r.second.a.hasPos) o.withPosition++; }
        std::sort(v.begin(), v.end(), [](const Rec* a, const Rec* b) { return a->lastSeen > b->lastSeen; });
        for (size_t i = 0; i < v.size() && i < 128; i++) { FlarmAircraft a = v[i]->a; a.ageSec = (float)(tnow - v[i]->lastSeen); o.aircraft.push_back(std::move(a)); }
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }
    double lastNewAt = -1e9;

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        const uint64_t nv = t.newProtocol;
        processChan(ch[0], x, n);
        processChan(ch[1], x, n);
        nIn += (int64_t)n;
        if (t.newProtocol != nv) lastNewAt = now();
        while (now() >= nextReport) {
            nextReport += 0.25;
            // the packets-per-second window: the last four quarter seconds
            window[3] = window[2]; window[2] = window[1]; window[1] = window[0]; window[0] = windowCount; windowCount = 0;
            t.packetsPerSec = (float)(window[0] + window[1] + window[2] + window[3]);
            publish();
        }
    }
};

FlarmReceiver::FlarmReceiver() : p_(std::make_unique<Impl>()) {}
FlarmReceiver::~FlarmReceiver() = default;

void FlarmReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->configure(inputRateHz); }
void FlarmReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->offsetHz = hz; }
void FlarmReceiver::setReference(double la, double lo) { std::lock_guard<std::mutex> lk(p_->procMu); p_->refValid = true; p_->refLat = la; p_->refLon = lo; }
void FlarmReceiver::clearReference() { std::lock_guard<std::mutex> lk(p_->procMu); p_->refValid = false; }
void FlarmReceiver::setWallClock(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->wallClock = on; }
void FlarmReceiver::setUnixTime(double u) { std::lock_guard<std::mutex> lk(p_->procMu); p_->unix0 = u; p_->wallClock = false; }
void FlarmReceiver::setExpiry(double s) { std::lock_guard<std::mutex> lk(p_->procMu); p_->expirySec = std::max(5.0, s); }
bool FlarmReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void FlarmReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->isReady) p_->resetState(); }
void FlarmReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->rec.clear(); p_->privSet.clear(); p_->newSet.clear(); p_->pending.clear();
    const double rate = p_->fs;
    p_->t = FlarmTelemetry();
    p_->t.inputRate = rate;
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.aircraft.clear();
    p_->pub.aircraftCount = 0; p_->pub.withPosition = 0; p_->pub.privacyFlagged = 0; p_->pub.newProtocolDevices = 0;
    p_->pub.seq = ++p_->seq;
}
std::string FlarmReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    std::vector<FlarmAircraft> v;
    for (auto& r : p_->rec) v.push_back(r.second.a);     // only openly broadcasting aircraft are ever in the table
    return results::pack(p_->t.timeSec, v);
}
bool FlarmReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<FlarmAircraft> v;
    if (!results::unpack(blob, t0, v)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    for (auto& a : v) { Rec r; r.a = a; r.firstSeen = r.lastSeen = p_->t.timeSec; p_->rec[a.id] = r; }
    return true;
}
void FlarmReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
bool FlarmReceiver::telemetry(FlarmTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void FlarmReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

ModeTuning flarmTuning() {
    ModeTuning t;
    t.stdMode = 68; t.id = "flarm"; t.name = "FLARM";
    t.minMhz = 860; t.maxMhz = 930; t.defMhz = 868.3;
    t.sampleRate = 1000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
