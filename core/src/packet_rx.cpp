// APRS / Packet receiver: one 25 kHz FM channel, AX.25 at 1200 baud (Bell 202 AFSK) and 9600 baud (G3RUH) decoded at the same time.
// Front end: mix out the signal offset, decimate to about 100 kHz, resample to exactly 96 kHz, channel filter, FM discriminator.
// 1200 baud: the audio goes to 48 kHz and three demodulators run side by side (tone correlators over one bit, with different windows, tone
// weights and clock-loop speeds - the way good TNCs do it); a frame is kept once even when several of them decode it.
// 9600 baud: two slicers with different filters, a clock loop, the G3RUH descrambler (x^17 + x^12 + 1) and NRZI.
// The frame layer is packet_ax25.*, the APRS layer packet_aprs.*.
#include "dect2/packet_rx.h"
#include "dect2/dmr_dsp.h"
#include "dect2/exact_resampler.h"
#include "dect2/packet_aprs.h"
#include "dect2/packet_ax25.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMidRate = 96000.0;
constexpr double kAudioRate = 48000.0;
constexpr size_t kBlock = 1920;                // power blocks of 20 ms at 96 kHz
constexpr size_t kNoiseBlocks = 250;           // the noise floor is a low percentile of the last 5 s
constexpr size_t kMaxFrames = 200, kMaxStations = 300;
constexpr int64_t kDupWindow = (int64_t)(0.03 * kMidRate);   // the same bytes this close together are one transmission
constexpr float kClipHz = 8000.f;              // FM clicks of a weak signal are cut to this many Hz of deviation

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

typedef std::function<void(const std::vector<uint8_t>&, bool, int64_t)> FrameSink;    // bytes, FCS ok, time in 96 kHz samples

// 1200 baud demodulator: correlate the audio with the mark and space tones over a window of about one bit; the stronger tone is the level;
// a clock loop picks the level once per bit; the levels go to the HDLC layer (which does NRZI, so the polarity does not matter).
struct Afsk1200 {
    int len = 40;
    float gain = 1.f, inertia = 0.75f, inertiaLocked = 0.85f;
    std::vector<float> mr, mi, sr, si;      // tap arrays, oldest sample first
    std::vector<float> ext;
    float ph = -0.5f;
    bool lev = false, prevLev = false;
    ax25::HdlcRx hdlc;
    int64_t pos = 0;

    void init(int L, bool hann, float g, float inert, float inertLocked) {
        len = L; gain = g; inertia = inert; inertiaLocked = inertLocked;
        mr.assign((size_t)L, 0.f); mi = mr; sr = mr; si = mr;
        for (int t = 0; t < L; t++) {
            const int k = L - 1 - t;
            const double w = hann ? 0.5 - 0.5 * std::cos(2 * kPi * (k + 0.5) / L) : 1.0;
            mr[(size_t)t] = (float)(w * std::cos(2 * kPi * 1200.0 * k / kAudioRate));
            mi[(size_t)t] = (float)(w * std::sin(2 * kPi * 1200.0 * k / kAudioRate));
            sr[(size_t)t] = (float)(w * std::cos(2 * kPi * 2200.0 * k / kAudioRate));
            si[(size_t)t] = (float)(w * std::sin(2 * kPi * 2200.0 * k / kAudioRate));
        }
        reset();
    }
    void reset() {
        ext.assign((size_t)len - 1, 0.f);
        ph = -0.5f; lev = prevLev = false; pos = 0;
        hdlc.reset();
    }
    void process(const float* x, size_t n) {
        const size_t h = (size_t)len - 1;
        ext.resize(h + n);
        std::copy(x, x + n, ext.begin() + (ptrdiff_t)h);
        const float step = (float)(1200.0 / kAudioRate);
        for (size_t i = 0; i < n; i++) {
            const float* p = &ext[i];
            float a = 0, b = 0, c = 0, d = 0;
            for (int t = 0; t < len; t++) {
                const float v = p[t];
                a += v * mr[(size_t)t]; b += v * mi[(size_t)t];
                c += v * sr[(size_t)t]; d += v * si[(size_t)t];
            }
            lev = (a * a + b * b) > gain * gain * (c * c + d * d);
            if (lev != prevLev) ph *= hdlc.flagSeen() ? inertiaLocked : inertia;
            prevLev = lev;
            ph += step;
            if (ph >= 0.5f) { ph -= 1.f; hdlc.bit(lev ? 1 : 0); }
            pos++;
        }
        std::copy(ext.end() - (ptrdiff_t)h, ext.end(), ext.begin());
        ext.resize(h);
    }
};

// 9600 baud: direct FSK on the discriminator output. Slicer threshold from tracked peaks, clock loop, descrambler, NRZI/HDLC.
struct Fsk9600 {
    float inertia = 0.7f, inertiaLocked = 0.8f;
    float hi = 0, lo = 0, ph = -0.5f;
    bool lev = false, prevLev = false;
    uint32_t sr = 0;
    ax25::HdlcRx hdlc;
    int64_t pos = 0;
    void init(float inert, float inertLocked) { inertia = inert; inertiaLocked = inertLocked; reset(); }
    void reset() { hi = lo = 0; ph = -0.5f; lev = prevLev = false; sr = 0; pos = 0; hdlc.reset(); }
    void process(const float* x, size_t n) {
        const float step = (float)(9600.0 / kMidRate);
        for (size_t i = 0; i < n; i++) {
            const float v = x[i];
            if (v > hi) hi += 0.25f * (v - hi); else hi += 0.0015f * (v - hi);
            if (v < lo) lo += 0.25f * (v - lo); else lo += 0.0015f * (v - lo);
            lev = v > 0.5f * (hi + lo);
            if (lev != prevLev) ph *= hdlc.flagSeen() ? inertiaLocked : inertia;
            prevLev = lev;
            ph += step;
            if (ph >= 0.5f) {
                ph -= 1.f;
                const uint32_t raw = lev ? 1u : 0u;
                const uint32_t d = raw ^ ((sr >> 11) & 1u) ^ ((sr >> 16) & 1u);     // the descrambler
                sr = ((sr << 1) | raw) & 0x1FFFFu;
                hdlc.bit((int)d);
            }
            pos++;
        }
    }
};

uint64_t hashBytes(const std::vector<uint8_t>& d) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : d) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

struct Rec { PacketStation s; };

} // namespace

struct PacketReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;

    double fs = 0, offsetHz = 0;
    bool isReady = false;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    double offPhase = 0;
    dmr::Fir<cf32> chan;
    dmr::Fir<float> audio48;               // 96 -> 48 kHz low-pass for the 1200 baud path
    dmr::Fir<float> lp9600[2];
    Afsk1200 afsk[3];
    Fsk9600 fsk[2];
    std::vector<cf32> bufA, bufB, mixBuf, o2, ch, clean;
    std::vector<float> disc, a48, f9[2];
    cf32 prevY = cf32(1.f, 0.f);

    // IF level: 20 ms blocks
    double blkSum = 0, blkDisc = 0; size_t blkN = 0;
    std::deque<double> blocks;
    double nf = 0; bool nfKnown = false;
    bool carrier = false;
    int64_t lastCarrier = -(int64_t)1e12, carrierSince = 0;     // the last block with a carrier; where the present carrier began
    double snrEma = 0, cfoEma = 0; bool haveSnr = false;
    double inPower = 0; int64_t inN = 0;       // whole-input level over a report

    int64_t n96 = 0, nextReport = 0;
    uint64_t seq = 0;

    // results
    struct Recent { uint64_t h; int64_t t; int baud; };
    std::deque<Recent> recent;
    struct Fail { int baud; int64_t t; bool carrier; };
    std::deque<Fail> fails;
    std::deque<PacketFrameInfo> frames;
    std::unordered_map<std::string, Rec> stations;
    PacketTelemetry t;
    int lastState = 0;
    PacketTelemetry pub;

    double now() const { return (double)n96 / kMidRate; }
    void say(const std::string& s) { if (log) log(s); }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        stages.clear();
        if (fs < packetTuning().minSampleRate - 1) return;
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
        chan.design(dmr::lowpassTaps(9000.0, 17000.0, kMidRate, 60, 9), 1);
        audio48.design(dmr::lowpassTaps(3300.0, 8000.0, kMidRate, 50, 9), 2);
        lp9600[0].design(dmr::lowpassTaps(5200.0, 11000.0, kMidRate, 40, 9), 1);
        lp9600[1].design(dmr::lowpassTaps(4000.0, 9000.0, kMidRate, 40, 9), 1);
        afsk[0].init(40, false, 1.0f, 0.70f, 0.80f);
        afsk[1].init(40, true, 0.7f, 0.75f, 0.85f);
        afsk[2].init(32, false, 1.5f, 0.80f, 0.88f);
        fsk[0].init(0.70f, 0.80f);
        fsk[1].init(0.75f, 0.85f);
        for (int i = 0; i < 3; i++) afsk[i].hdlc.setCallback([this, i](const std::vector<uint8_t>& dat, bool ok) { onRaw(1200, i, dat, ok, afsk[i].pos * 2); });
        for (int i = 0; i < 2; i++) fsk[i].hdlc.setCallback([this, i](const std::vector<uint8_t>& dat, bool ok) { onRaw(9600, i, dat, ok, fsk[i].pos); });
        isReady = true;
        resetState();
    }

    void resetState() {
        for (auto& s : stages) s.reset();
        if (!rsPass) rs.reset();
        chan.reset(); audio48.reset(); lp9600[0].reset(); lp9600[1].reset();
        for (auto& a : afsk) a.reset();
        for (auto& a : fsk) a.reset();
        offPhase = 0; prevY = cf32(1.f, 0.f);
        blkSum = blkDisc = 0; blkN = 0; blocks.clear(); nf = 0; nfKnown = false; carrier = false;
        lastCarrier = -(int64_t)1e12; snrEma = cfoEma = 0; haveSnr = false;
        inPower = 0; inN = 0;
        n96 = 0; nextReport = 0;
        recent.clear(); fails.clear(); frames.clear(); stations.clear();
        const uint64_t s = seq;
        t = PacketTelemetry();
        t.inputRate = fs;
        t.last1200Sec = t.last9600Sec = t.lastAprsSec = -1e9;
        lastState = 0;
        std::lock_guard<std::mutex> lk(telMu);
        const uint64_t ps = pub.seq;
        pub = PacketTelemetry();
        pub.seq = std::max(s, ps);
        seq = pub.seq;
        pub.inputRate = fs;
        say("APRS / Packet: listening for 1200 and 9600 baud AX.25");
    }

    // ---- frames
    void onRaw(int baud, int variant, const std::vector<uint8_t>& dat, bool ok, int64_t tt) {
        if (!ok) {
            if (variant == 0) fails.push_back({baud, tt, (carrier || n96 - lastCarrier < (int64_t)(0.2 * kMidRate)) && tt - carrierSince > (int64_t)(0.05 * kMidRate)});         // one demodulator counts the failures; another one may still get the frame
            return;
        }
        const uint64_t h = hashBytes(dat);
        while (!recent.empty() && recent.front().t < tt - 4 * kDupWindow) recent.pop_front();
        for (const Recent& r : recent) if (r.h == h && r.baud == baud && std::llabs(r.t - tt) < kDupWindow) return;
        recent.push_back({h, tt, baud});
        accept(baud, dat, tt);
    }

    void flushFails(bool all) {
        while (!fails.empty() && (all || fails.front().t < n96 - (int64_t)(1.3 * kMidRate))) {
            const Fail f = fails.front();
            fails.pop_front();
            if (!f.carrier) continue;                              // no carrier then: random bits of noise
            if (f.baud == 1200) t.bad1200++; else t.bad9600++;
            t.blocksBad++;
            if (t.blocksBad <= 20) say(std::string("APRS: a ") + std::to_string(f.baud) + " baud frame failed the check at " + std::to_string((double)f.t / kMidRate) + " s");
        }
    }

    void touchStation(const std::string& call, const aprs::Info* a, bool object, const std::string& via, double tnow) {
        auto it = stations.find(call);
        if (it == stations.end()) {
            if (stations.size() >= kMaxStations) {
                auto old = stations.begin();
                for (auto j = stations.begin(); j != stations.end(); ++j) if (j->second.s.lastHeardSec < old->second.s.lastHeardSec) old = j;
                stations.erase(old);
            }
            it = stations.emplace(call, Rec()).first;
            it->second.s.call = call;
            if (stations.size() <= 40) say("APRS: new station " + call);
        }
        PacketStation& s = it->second.s;
        s.lastHeardSec = tnow;
        s.count++;
        s.isObject = object;
        s.via = via;
        if (!a) return;
        if (a->hasPos) {
            s.hasPos = true; s.lat = a->lat; s.lon = a->lon;
            s.hasAlt = a->hasAlt; s.altM = a->altM;
            s.hasCourse = a->hasCourse; s.courseDeg = a->courseDeg;
            s.hasSpeed = a->hasSpeed; s.speedKnots = a->speedKnots;
            if (a->symTable) { s.symTable = a->symTable; s.symCode = a->symCode; }
            s.comment = a->comment;
        } else if (a->type == "Status") s.comment = a->text;
    }

    void accept(int baud, const std::vector<uint8_t>& dat, int64_t tt) {
        const double tnow = (double)tt / kMidRate;
        ax25::Frame f;
        if (!ax25::parseFrame(dat.data(), dat.size(), f)) { t.blocksBad++; if (baud == 1200) t.bad1200++; else t.bad9600++; return; }
        t.blocksOk++;
        if (baud == 1200) { t.ok1200++; t.last1200Sec = tnow; } else { t.ok9600++; t.last9600Sec = tnow; }
        aprs::Info a;
        const bool isAprs = f.isUi() && f.pid == 0xF0 && aprs::parse(f.to.call, f.info, a);
        PacketFrameInfo pf;
        pf.timeSec = tnow; pf.from = f.from.str(); pf.to = f.to.str(); pf.path = f.pathStr(); pf.info = f.info; pf.baud = baud;
        if (isAprs) {
            pf.type = a.type; pf.summary = a.summary; pf.hasPos = a.hasPos; pf.lat = a.lat; pf.lon = a.lon;
            t.aprsFrames++; t.lastAprsSec = tnow;
        } else {
            pf.type = f.typeName();
            pf.summary = "AX.25 " + f.typeName() + (f.info.empty() ? "" : ": " + f.info);
        }
        frames.push_back(pf);
        while (frames.size() > kMaxFrames) frames.pop_front();
        touchStation(pf.from, isAprs ? &a : nullptr, false, "", tnow);
        if (isAprs && (a.type == "Object" || a.type == "Item") && !a.name.empty()) touchStation(a.name, &a, true, pf.from, tnow);
        // a good frame explains a failure of another demodulator at the same moment (a failure is only counted 1.3 s after it, for this)
        for (auto it = fails.begin(); it != fails.end();) {
            // the other speed's demodulator sees rubbish while a frame of this speed is on the air: those are not failures either
            const bool same = it->baud == baud && std::llabs(it->t - tt) < (int64_t)(0.08 * kMidRate);
            const bool other = it->baud != baud && it->t <= tt && tt - it->t < (int64_t)(1.2 * kMidRate);
            if (same || other) it = fails.erase(it); else ++it;
        }
    }

    // ---- IF level and carrier
    void blockEnd() {
        const double mean = blkSum / (double)blkN;
        blocks.push_back(mean);
        if (blocks.size() > kNoiseBlocks) blocks.pop_front();
        if (blocks.size() >= 3) {
            std::vector<double> s(blocks.begin(), blocks.end());
            std::sort(s.begin(), s.end());
            nf = std::max(s[s.size() / 10] * 1.05, 1e-12);
            nfKnown = true;
        }
        const bool was = carrier;
        carrier = nfKnown && mean > 2.0 * nf;
        if (carrier && !was) carrierSince = n96 - (int64_t)kBlock;
        if (carrier) {
            lastCarrier = n96;
            const double snr = 10 * std::log10((mean - nf) / nf);
            snrEma = haveSnr ? 0.9 * snrEma + 0.1 * snr : snr;
            const double cfo = blkDisc / (double)blkN;
            cfoEma = haveSnr ? 0.9 * cfoEma + 0.1 * cfo : cfo;
            haveSnr = true;
        }
        blkSum = blkDisc = 0; blkN = 0;
    }

    void publish() {
        flushFails(false);
        const double tnow = now();
        t.timeSec = tnow;
        t.carrier = carrier || (n96 - lastCarrier) < (int64_t)(0.2 * kMidRate);
        t.dataValid = tnow - std::max(t.last1200Sec, t.last9600Sec) < 15.0;
        t.state = t.dataValid ? 2 : (t.carrier || (n96 - lastCarrier) < (int64_t)(5 * kMidRate) ? 1 : 0);
        t.cfoHz = cfoEma;
        t.snrDb = (float)snrEma;
        t.levelDb = inN ? (float)(10 * std::log10(inPower / (double)inN + 1e-20)) : -200.f;
        inPower = 0; inN = 0;
        if (t.state != lastState) {
            say(t.state == 2 ? "APRS: decoding frames" : t.state == 1 ? "APRS: signal, no good frame lately" : "APRS: no signal");
            lastState = t.state;
        }
        PacketTelemetry o = t;
        o.seq = ++seq;
        o.frames.assign(frames.begin(), frames.end());
        std::vector<const PacketStation*> order;
        for (const auto& kv : stations) order.push_back(&kv.second.s);
        std::sort(order.begin(), order.end(), [](const PacketStation* a, const PacketStation* b) { return a->lastHeardSec > b->lastHeardSec; });
        for (const PacketStation* s : order) o.stations.push_back(*s);
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
        for (size_t i = 0; i < n; i++) inPower += (double)std::norm(x[i]);
        inN += (int64_t)n;
        const cf32* cur = x;
        size_t cn = n;
        if (offsetHz != 0) {
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
        ch.clear();
        chan.process(cur, cn, ch);
        // discriminator, in Hz, and the IF level blocks
        disc.resize(ch.size());
        const float k = (float)(kMidRate / (2 * kPi));
        for (size_t i = 0; i < ch.size(); i++) {
            const cf32 y = ch[i];
            const cf32 z = y * std::conj(prevY);
            float d = (z.real() == 0.f && z.imag() == 0.f) ? 0.f : std::atan2(z.imag(), z.real()) * k;
            d = std::max(-kClipHz, std::min(kClipHz, d));
            prevY = y;
            disc[i] = d;
            blkSum += (double)std::norm(y);
            blkDisc += d;
            if (++blkN == kBlock) blockEnd();
        }
        a48.clear();
        audio48.process(disc.data(), disc.size(), a48);
        if (!a48.empty()) for (auto& a : afsk) a.process(a48.data(), a48.size());
        for (int i = 0; i < 2; i++) {
            f9[i].clear();
            lp9600[i].process(disc.data(), disc.size(), f9[i]);
            if (!f9[i].empty()) fsk[i].process(f9[i].data(), f9[i].size());
        }
        n96 += (int64_t)ch.size();
        while (n96 >= nextReport) {
            nextReport += (int64_t)(0.25 * kMidRate);
            publish();
        }
    }
};

PacketReceiver::PacketReceiver() : p_(std::make_unique<Impl>()) {}
PacketReceiver::~PacketReceiver() = default;

void PacketReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->configure(inputRateHz);
}
void PacketReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->offsetHz = hz;
    p_->offPhase = 0;
}
bool PacketReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return p_->isReady;
}
void PacketReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->isReady) p_->resetState();
}
void PacketReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->feed(x, n);
}
bool PacketReceiver::telemetry(PacketTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void PacketReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}

ModeTuning packetTuning() {
    ModeTuning t;
    t.stdMode = 26; t.id = "packet"; t.name = "APRS / Packet";
    t.minMhz = 25; t.maxMhz = 1000; t.defMhz = 144.8;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as Inmarsat-C and Aero)
    return t;
}

} // namespace dect2
