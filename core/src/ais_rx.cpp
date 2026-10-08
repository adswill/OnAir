// AIS receiver: two channels (AIS 1 at -25 kHz and AIS 2 at +25 kHz of the user's frequency) at the same time.
// Front end: mix out the signal offset, decimate to about 100 kHz, resample to exactly 96 kHz; each channel is mixed to zero, filtered and
// decimated to 48 kHz (5 samples per bit). A power detector finds the bursts (they are short and stand alone), each one is cut out of a ring of samples
// and decoded offline by aisDecodeBurst (ais_phy.cpp). The message layer and the station table are here.
#include "dect2/ais_rx.h"
#include "dect2/ais_phy.h"
#include "dect2/ais_proto.h"
#include "dect2/dmr_dsp.h"
#include "dect2/exact_resampler.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kChanHz = 25000.0;
constexpr double kMidRate = 96000.0;
constexpr int kW = 24;                      // power smoothing: 0.5 ms
constexpr size_t kRing = 16384;
constexpr size_t kMaxBurst = 7200;          // 150 ms: five slots
constexpr size_t kPre = 480, kPost = 120;   // samples kept around a burst
constexpr size_t kBlock = 960;              // noise blocks of 20 ms
constexpr size_t kBlocks = 64;              // the noise floor is a low percentile of the last 1.3 s
constexpr double kOn = 2.0;                 // burst starts at 3 dB above the noise
constexpr double kEndAbs = 1.5, kEndRel = 0.08;
constexpr size_t kMaxVessels = 300, kTrackCap = 100, kTrackShown = 25, kNmeaCap = 50;
constexpr size_t kPublishSamples = 12000;   // 0.25 s at 48 kHz

int pickDecimation(double fs) {
    const int hi = std::max(1, (int)std::floor(fs / kMidRate));
    const int lo = std::max(1, (int)std::floor(fs / 130000.0));
    auto largestPrime = [](int v) {
        int lp = 1;
        for (int p = 2; p <= v; p++) while (v % p == 0) { lp = p; v /= p; }
        return lp;
    };
    int best = hi, bestPrime = largestPrime(hi);
    for (int d = hi; d >= lo; d--) {
        const int lp = largestPrime(d);
        if (lp <= 7) { best = d; bestPrime = lp; break; }
        if (lp < bestPrime) { best = d; bestPrime = lp; }
    }
    return best;
}

std::vector<int> splitFactors(int d) {
    std::vector<int> primes;
    for (int p = 2; d > 1;) {
        if (d % p == 0) { primes.push_back(p); d /= p; }
        else p++;
    }
    std::sort(primes.rbegin(), primes.rend());
    std::vector<int> stages;
    for (int p : primes) {
        if (!stages.empty() && stages.back() * p <= 8) stages.back() *= p;
        else stages.push_back(p);
    }
    return stages;
}

struct Pending { uint64_t start, end, decodeAt; };

struct Chan {
    dmr::Fir<cf32> dec, filt;
    std::vector<cf32> ring = std::vector<cf32>(kRing);
    uint64_t total = 0;
    double delay[kW] = {};
    double sumW = 0;
    // noise floor from blocks of 20 ms
    double blkSum = 0; size_t blkN = 0; bool blkTaint = false;
    std::vector<double> blocks;
    double nf = 0; bool nfKnown = false;
    double levelEma = 0;
    // burst
    bool active = false;
    uint64_t start = 0; double peak = 0; int low = 0;
    std::deque<Pending> pend;
    void reset() {
        dec.reset(); filt.reset();
        total = 0;
        for (double& d : delay) d = 0;
        sumW = 0; blkSum = 0; blkN = 0; blkTaint = false;
        blocks.clear(); nf = 0; nfKnown = false; levelEma = 0;
        active = false; start = 0; peak = 0; low = 0;
        pend.clear();
    }
};

struct Rec {
    AisVessel v;
    double lastHeard = 0;
};

} // namespace

struct AisReceiver::Impl {
    std::mutex procMu;          // feed / reset / configure
    std::mutex telMu;           // the published snapshot
    std::function<void(const std::string&)> log;

    double fs = 0, offsetHz = 0;
    bool isReady = false;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    double enbw = 10000;        // noise bandwidth of the channel chain in Hz
    cf32 mixTab[2][96];
    uint64_t n96 = 0;
    double offPhase = 0;
    Chan ch[2];
    std::vector<cf32> bufA, bufB, mixBuf, tmp, o1, o2, o3, clean;

    // results
    AisTelemetry t;             // counters and tables (the vessels and sentences are kept separately below)
    std::unordered_map<uint32_t, Rec> vessels;
    std::deque<std::string> nmea;
    std::deque<double> goodTimes[2];
    int nmeaSeq = 0;
    double lastGood = -1e9, lastBurst = -1e9, cfoEma = 0, snrEma = 0;
    bool haveSnr = false;
    uint64_t pubMark = 0;
    int lastState = 0;
    uint64_t seq = 0;
    AisTelemetry pub;           // what telemetry() hands out

    AisPhyConfig phy;

    void logLine(const std::string& s) { if (log) log(s); }
    double now() const { return (double)ch[0].total / kAisWorkRate; }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        stages.clear();
        if (fs < aisTuning().minSampleRate - 1) return;
        const int d = pickDecimation(fs);
        const std::vector<int> f = splitFactors(d);
        double r = fs;
        stages.assign(f.size(), dmr::Fir<cf32>());
        for (size_t i = 0; i < f.size(); i++) {
            const double out = r / f[i];
            const double pass = 38000.0, stop = std::max(pass + 4000.0, out - pass);
            stages[i].design(dmr::lowpassTaps(pass, std::min(stop, r * 0.5), r, 60, 9), f[i]);
            r = out;
        }
        rsPass = std::fabs(r - kMidRate) < 0.01;
        if (!rsPass && !rs.configure(r, kMidRate)) return;
        // the two channels: mixers at +-25 kHz of the 96 kHz stream
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 96; i++) {
                const double a = 2 * kPi * (c ? -kChanHz : kChanHz) * i / kMidRate;      // mixing the channel down
                mixTab[c][i] = cf32((float)std::cos(a), (float)std::sin(a));
            }
        // the channel filters: 96 -> 48 kHz (rejects the other channel), then the channel filter itself
        const std::vector<float> h1 = dmr::lowpassTaps(9500.0, 38000.0, kMidRate, 60, 9);
        const std::vector<float> h2 = dmr::lowpassTaps(phy.chanPassHz, phy.chanStopHz, kAisWorkRate, 60, 9);
        for (int c = 0; c < 2; c++) { ch[c].dec.design(h1, 2); ch[c].filt.design(h2, 1); }
        // noise bandwidth of both filters together: sum of |H1 H2|^2 df over the 48 kHz of the output
        double acc = 0;
        const int np = 4096;
        for (int k = 0; k < np; k++) {
            const double fHz = (k - np / 2) * (kAisWorkRate / np);
            std::complex<double> a1(0, 0), a2(0, 0);
            for (size_t i = 0; i < h1.size(); i++) a1 += (double)h1[i] * std::polar(1.0, -2 * kPi * fHz * (double)i / kMidRate);
            for (size_t i = 0; i < h2.size(); i++) a2 += (double)h2[i] * std::polar(1.0, -2 * kPi * fHz * (double)i / kAisWorkRate);
            acc += std::norm(a1 * a2);
        }
        enbw = std::max(1000.0, acc * (kAisWorkRate / np));
        isReady = true;
        resetState();
    }

    void resetState() {
        for (auto& s : stages) s.reset();
        if (!rsPass) rs.reset();
        n96 = 0; offPhase = 0;
        ch[0].reset(); ch[1].reset();
        t = AisTelemetry();
        vessels.clear(); nmea.clear(); goodTimes[0].clear(); goodTimes[1].clear();
        lastGood = lastBurst = -1e9; cfoEma = snrEma = 0; haveSnr = false;
        pubMark = 0; lastState = 0;
        std::lock_guard<std::mutex> lk(telMu);
        const uint64_t s = pub.seq;
        pub = AisTelemetry();
        pub.seq = s;
    }

    // ---- stations
    void updateVessel(const ais::AisMsg& m, char chan, double tnow) {
        auto it = vessels.find(m.mmsi);
        if (it == vessels.end()) {
            if (vessels.size() >= kMaxVessels) {
                auto old = vessels.begin();
                for (auto j = vessels.begin(); j != vessels.end(); ++j) if (j->second.lastHeard < old->second.lastHeard) old = j;
                vessels.erase(old);
            }
            it = vessels.emplace(m.mmsi, Rec()).first;
            it->second.v.mmsi = m.mmsi;
            if (vessels.size() <= 40) logLine("AIS: new station " + std::to_string(m.mmsi));
        }
        Rec& r = it->second;
        AisVessel& v = r.v;
        r.lastHeard = tnow;
        v.messages++;
        v.lastType = m.type;
        v.channel = chan;
        if (m.cls != AIS_CLASS_OTHER) v.cls = m.cls;
        if (!m.valid) return;
        const bool movement = m.type == 1 || m.type == 2 || m.type == 3 || m.type == 9 || m.type == 18 || m.type == 19 || m.type == 27;
        if (m.hasPos) {
            const bool moved = !v.hasPos || std::fabs(v.lat - m.lat) > 0.0002 || std::fabs(v.lon - m.lon) > 0.0002;
            v.hasPos = true; v.lat = m.lat; v.lon = m.lon;
            if (moved) {
                v.track.push_back({(float)m.lat, (float)m.lon});
                if (v.track.size() > kTrackCap) v.track.erase(v.track.begin());
            }
        }
        if (movement) {
            v.sog = m.sog; v.cog = m.cog; v.heading = m.heading;
            if (m.type <= 3) { v.hasRot = m.hasRot; v.rotDegMin = m.rotDegMin; v.navStatus = m.navStatus; }
            if (m.type == 27) v.navStatus = m.navStatus;
            if (m.type == 9) v.altitudeM = m.altitudeM;
        }
        if (!m.name.empty()) v.name = m.name;
        if (!m.callsign.empty()) v.callsign = m.callsign;
        if (!m.destination.empty() || m.type == 5) v.destination = m.destination;
        if (m.imo) v.imo = m.imo;
        if (m.shipType >= 0) v.shipType = m.shipType;
        if (m.dimA || m.dimB || m.dimC || m.dimD) { v.dimA = m.dimA; v.dimB = m.dimB; v.dimC = m.dimC; v.dimD = m.dimD; }
        if (m.type == 5) {
            v.etaMonth = m.etaMonth; v.etaDay = m.etaDay; v.etaHour = m.etaHour; v.etaMin = m.etaMin;
            v.draughtM = m.draughtM;
        }
        if (m.type == 21) {
            v.aidType = m.aidType; v.virtualAid = m.virtualAid;
            if (m.second < 60) v.offPosition = m.offPosition;
        }
    }

    void onFrame(int c, const ais::Bits& bits, double snrDb, double cfoHz) {
        const double tnow = now();
        if (bits.size() < 38) { t.blocksBad++; t.channelBad[c]++; return; }
        ais::AisMsg m;
        ais::decodeMessage(bits, m);
        const char chn = c ? 'B' : 'A';
        t.blocksOk++; t.channelOk[c]++;
        t.typeCount[c][m.type >= 1 && m.type <= 27 ? m.type : 0]++;
        goodTimes[c].push_back(tnow);
        lastGood = tnow;
        cfoEma = haveSnr ? 0.85 * cfoEma + 0.15 * cfoHz : cfoHz;
        snrEma = haveSnr ? 0.85 * snrEma + 0.15 * snrDb : snrDb;
        haveSnr = true;
        for (const std::string& s : ais::toNmea(bits, chn, nmeaSeq)) nmea.push_back(s);
        if (bits.size() > 6 * 60) nmeaSeq = (nmeaSeq + 1) % 10;
        while (nmea.size() > kNmeaCap) nmea.pop_front();
        if (m.mmsi) updateVessel(m, chn, tnow);
    }

    // ---- bursts
    void decodeBurst(int c, const Pending& p) {
        Chan& C = ch[c];
        t.bursts++;
        lastBurst = now();
        const uint64_t oldest = C.total > kRing ? C.total - kRing : 0;
        uint64_t s0 = p.start > kPre ? p.start - kPre : 0;
        if (s0 < oldest) s0 = oldest;
        const uint64_t e0 = std::min(p.end + kPost, C.total);
        if (e0 <= s0 + 200) return;
        std::vector<cf32> seg((size_t)(e0 - s0));
        for (uint64_t i = s0; i < e0; i++) seg[(size_t)(i - s0)] = C.ring[(size_t)(i % kRing)];
        const size_t core0 = (size_t)(p.start > s0 ? p.start - s0 : 0), core1 = (size_t)(std::min(p.end, e0) - s0);
        const AisBurstResult r = aisDecodeBurst(seg.data(), seg.size(), core0, core1, phy);
        if (r.frames.empty()) {
            if (r.bad > 0) { t.blocksBad++; t.channelBad[c]++; }
            return;
        }
        // signal to noise ratio in 48 kHz: the burst power above the noise floor against the noise floor scaled from the filter's bandwidth
        double pw = 0;
        const size_t a = std::min(core0 + kW, seg.size()), b = core1 > kW ? core1 - kW : 0;
        size_t cnt = 0;
        for (size_t i = a; i < b; i++) { pw += std::norm(seg[i]); cnt++; }
        double snr = 0;
        if (cnt > 10 && C.nfKnown) {
            const double sig = pw / (double)cnt - C.nf;
            const double n48 = C.nf * kAisWorkRate / enbw;
            snr = sig > 0 ? 10 * std::log10(sig / n48) : -10;
        }
        for (const auto& f : r.frames) onFrame(c, f, snr, r.cfoHz);
    }

    void chanSample(int c, cf32 x) {
        Chan& C = ch[c];
        const uint64_t idx = C.total++;
        C.ring[(size_t)(idx % kRing)] = x;
        const double p = std::norm(x);
        double& dl = C.delay[idx % kW];
        C.sumW += p - dl;
        dl = p;
        const double s = std::max(0.0, C.sumW) / kW;
        // noise blocks
        C.blkSum += p; C.blkN++;
        if (C.active) C.blkTaint = true;
        if (C.blkN == kBlock) {
            const double mean = C.blkSum / (double)kBlock;
            if (!C.blkTaint) {
                C.blocks.push_back(mean);
                if (C.blocks.size() > kBlocks) C.blocks.erase(C.blocks.begin());
                if (C.blocks.size() >= 3) {
                    std::vector<double> sorted = C.blocks;
                    std::sort(sorted.begin(), sorted.end());
                    C.nf = std::max(sorted[sorted.size() / 5] * 1.03, 1e-10);
                    C.nfKnown = true;
                }
            }
            C.levelEma = C.levelEma == 0 ? mean : 0.9 * C.levelEma + 0.1 * mean;
            C.blkSum = 0; C.blkN = 0; C.blkTaint = false;
        }
        if (!C.active) {
            if (C.nfKnown && idx >= (uint64_t)kW && s > kOn * C.nf) {
                C.active = true; C.start = idx - kW / 2; C.peak = s; C.low = 0;
                C.blkTaint = true;
            }
        } else {
            C.peak = std::max(C.peak, s);
            const double endThr = std::max(kEndAbs * C.nf, kEndRel * C.peak);
            if (s < endThr) C.low++; else C.low = 0;
            if (C.low >= 12 || idx - C.start > kMaxBurst) {
                const uint64_t end = idx - (uint64_t)C.low;
                if (end > C.start + 100) C.pend.push_back({C.start, end, idx + kPost});
                C.active = false;
            }
        }
        while (!C.pend.empty() && C.pend.front().decodeAt <= idx) {
            const Pending pd = C.pend.front();
            C.pend.pop_front();
            decodeBurst(c, pd);
        }
    }

    // ---- publishing
    void publish() {
        const double tnow = now();
        for (int c = 0; c < 2; c++) {
            while (!goodTimes[c].empty() && goodTimes[c].front() < tnow - 60) goodTimes[c].pop_front();
            const double span = std::max(5.0, std::min(60.0, tnow));
            t.burstsPerMin[c] = (float)((double)goodTimes[c].size() * 60.0 / span);
            const double scale = kAisWorkRate / enbw;
            t.levelDbfs[c] = ch[c].levelEma > 0 ? (float)(10 * std::log10(ch[c].levelEma * scale)) : -120.f;
            t.noiseDbfs[c] = ch[c].nfKnown ? (float)(10 * std::log10(ch[c].nf * scale)) : -120.f;
        }
        t.timeSec = tnow;
        t.vesselCount = (uint32_t)vessels.size();
        t.dataValid = lastGood > -1e8 && tnow - lastGood < 10.0;
        t.state = t.dataValid ? 2 : (lastBurst > -1e8 && tnow - lastBurst < 10.0 ? 1 : 0);
        t.cfoHz = cfoEma;
        t.snrDb = (float)snrEma;
        if (t.state != lastState) {
            logLine(t.state == 2 ? "AIS: decoding messages" : t.state == 1 ? "AIS: signal, no good message" : "AIS: lost the signal");
            lastState = t.state;
        }
        AisTelemetry o = t;
        o.seq = ++seq;
        std::vector<const Rec*> order;
        order.reserve(vessels.size());
        for (const auto& kv : vessels) order.push_back(&kv.second);
        std::sort(order.begin(), order.end(), [](const Rec* a, const Rec* b) { return a->lastHeard > b->lastHeard; });
        o.vessels.reserve(order.size());
        for (size_t i = 0; i < order.size(); i++) {
            AisVessel v = order[i]->v;
            v.ageSec = (float)(tnow - order[i]->lastHeard);
            if (i >= kTrackShown) v.track.clear();
            o.vessels.push_back(std::move(v));
        }
        o.nmea.assign(nmea.begin(), nmea.end());
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        bool bad = false;
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real(), im = x[i].imag();
            if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; }
        }
        if (bad) {
            clean.assign(x, x + n);
            for (auto& v : clean) if (!(std::fabs(v.real()) < 1e4f && std::fabs(v.imag()) < 1e4f)) v = cf32(0, 0);
            x = clean.data();
        }
        const cf32* cur = x;
        size_t cn = n;
        if (offsetHz != 0) {
            // the middle of the two channels goes to 0 Hz
            mixBuf.resize(n);
            const double w = -2 * kPi * offsetHz / fs;
            const cf32 stp((float)std::cos(w), (float)std::sin(w));
            cf32 ph((float)std::cos(offPhase), (float)std::sin(offPhase));
            for (size_t i = 0; i < n; i++) {
                mixBuf[i] = x[i] * ph;
                ph *= stp;
                if ((i & 1023) == 1023) ph /= std::abs(ph);
            }
            offPhase = std::fmod(offPhase + w * (double)n, 2 * kPi);
            cur = mixBuf.data();
        }
        for (size_t s = 0; s < stages.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? bufB : bufA;
            dst.clear();
            stages[s].process(cur, cn, dst);
            cur = dst.data(); cn = dst.size();
        }
        if (!rsPass) {
            o2.clear();
            rs.process(cur, cn, o2);
            cur = o2.data(); cn = o2.size();
        }
        for (int c = 0; c < 2; c++) {
            tmp.resize(cn);
            for (size_t i = 0; i < cn; i++) tmp[i] = cur[i] * mixTab[c][(n96 + i) % 96];
            o1.clear();
            ch[c].dec.process(tmp.data(), cn, o1);
            o3.clear();
            ch[c].filt.process(o1.data(), o1.size(), o3);
            for (const cf32& v : o3) chanSample(c, v);
        }
        n96 += cn;
        if (ch[0].total - pubMark >= kPublishSamples) {
            pubMark = ch[0].total;
            publish();
        }
    }
};

AisReceiver::AisReceiver() : p_(std::make_unique<Impl>()) {}
AisReceiver::~AisReceiver() = default;

void AisReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->configure(inputRateHz);
}
void AisReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->offsetHz = hz;
    p_->offPhase = 0;
}
bool AisReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return p_->isReady;
}
void AisReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->isReady) p_->resetState();
}
void AisReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->feed(x, n);
}
bool AisReceiver::telemetry(AisTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void AisReceiver::setPhyConfig(const AisPhyConfig& c) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->phy = c;
}
void AisReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}

ModeTuning aisTuning() {
    ModeTuning t;
    t.stdMode = 16; t.id = "ais"; t.name = "AIS";
    t.minMhz = 156; t.maxMhz = 163; t.defMhz = 162;     // the default centre puts AIS 1 and AIS 2 at -25 and +25 kHz
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.1;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 0;                                 // DC falls between the two channels
    return t;
}

} // namespace dect2
