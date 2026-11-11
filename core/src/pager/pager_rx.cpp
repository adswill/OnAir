// Pagers receiver: POCSAG (512, 1200, 2400 baud) and FLEX (1600/2, 3200/2, 3200/4, 6400/4) on one 25 kHz channel.
// Front end: mix out the signal offset, decimate to 96 kHz, filter to about +-10 kHz and decimate to 48 kHz, FM discriminator. The discriminator is
// kept as an unwrapped phase in a ring, so the mean frequency over any window (a symbol) is one subtraction. Four searchers look at every sample for
// a sync pattern: the POCSAG sync word at the three baud rates (the symbol sampling instants follow from the baud rate) and the FLEX sync 1 marker.
// The first that finds its pattern locks on (in either polarity) and reads the transmission; the symbol timing follows the data. The protocol layers
// are in pager_proto.cpp.
#include "dect2/session/mode_adapters.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/automation/engine.h"
#include "dect2/results/results_io.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/pager/pager_proto.h"
#include "dect2/pager/pager_intel.h"
#include "dect2/tones/seq_decoder.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kMidRate = 96000.0;
constexpr double kWork = 48000.0;
constexpr double kMinWideRate = 192000.0;   // whole-span decoding hands a receiver its carrier at this rate (setWideInput)
constexpr size_t kRing = 8192, kMask = kRing - 1;
constexpr size_t kMaxMessages = 2000;
constexpr double kMinAmp = 0.2;                // rad/sample: the smallest mean deviation a sync may have (the full one is about 0.6)
constexpr double kTimingGain = 0.02;
constexpr int kSyncMism = 5;                 // POCSAG sync candidates: up to this many wrong bits of 32 (2 without tone detection), then the tone check decides
constexpr double kToneSyncMin = 0.52, kToneSyncSlope = 0.025;   // tone correlation a candidate with more than 2 wrong bits needs: min + slope * wrong bits
constexpr double kToneMaxOffset = 0.4;       // rad/sample (3 kHz at 48 kHz): beyond it the channel filter's edge is in the upper tone

constexpr int kFirstBatchValid = 10;           // of the 16 words of the first batch after a searched sync, at least this many valid
constexpr float kDoubtText = 1.5f;             // message words (FLEX vectors too): any of the least reliable bits (a wrong one only damages a text)
constexpr float kDoubt = 0.6f;                // soft repair: only bits nearer the decision threshold than 0.6 of the outer amplitude may be overruled

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

inline int popcount32(uint32_t v) { return dect2::bits::popcount(v); }

// Soft-decision repair of a code word (Chase): when the BCH code cannot repair the hard decisions (more than two wrong bits), flip each
// combination of up to three of the six least reliable bits and repair again. Of the code words found, the one whose changed bits are the
// least reliable wins; it must differ from the hard decisions in at most four bits, all of them below `doubt` (the hard decoder alone
// accepts a quarter of random words: where a wrong word does harm, a confident bit is never overruled). rel[k]: the reliability of
// transmitted bit k (bit 31 - k of the word). Returns the word to hand on, which the plain repair then accepts (flipped where the search
// found the errors), or w unchanged.
uint32_t chaseWord(uint32_t w, const float* rel, float doubt, int maxErr = 4) {
    if (pager::correctWord(w).ok) return w;
    int idx[6];
    {
        int order[32];
        for (int k = 0; k < 32; k++) order[k] = k;
        std::partial_sort(order, order + 6, order + 32, [&](int a, int b) { return rel[a] < rel[b]; });
        for (int i = 0; i < 6; i++) idx[i] = order[i];
    }
    uint32_t bestW = w;
    float bestM = 1e30f;
    for (unsigned mask = 1; mask < 64; mask++) {
        if (popcount32(mask) > 3) continue;
        uint32_t flip = 0;
        for (int i = 0; i < 6; i++) if (mask & (1u << i)) flip |= 1u << (31 - idx[i]);
        const pager::Fixed f = pager::correctWord(w ^ flip);
        if (!f.ok) continue;
        const uint32_t e = w ^ pager::encodeWord(f.data);   // every bit the candidate changes
        if (popcount32(e) > maxErr) continue;
        float m = 0;
        bool sure = false;
        for (int k = 0; k < 32; k++) if (e & (1u << (31 - k))) { m += rel[k]; if (rel[k] >= doubt) sure = true; }
        if (sure || m >= bestM) continue;
        bestM = m; bestW = w ^ flip;
    }
    return bestW;
}

struct Hit {
    bool ok = false;
    double score = 0;          // soft correlation with the pattern, rad/sample per symbol
    double mean = 0;           // the carrier offset: mean over the pattern, rad/sample
    int pol = 1;               // +1 as defined (1 = lower frequency), -1 inverted
    uint16_t code = 0;         // FLEX: the sync code
    int64_t at = 0;            // sample index of the end of the pattern
    double ts = 0;             // tone-energy correlation with the pattern, -1 .. 1 (0: not computed)
};

} // namespace

struct PagerReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    double rate = 0;                       // from configure()
    double offsetHz = 0;                   // setSignalOffset(): where the user's frequency sits in the input
    PagerTelemetry pub;                    // the published report
    std::atomic<bool> resetReq{true};
    bool wideInput = false;                // setWideInput(): a channelised carrier, not a radio capture
    std::atomic<int> protoMask{3};        // setProtocols(): 1 POCSAG, 2 FLEX
    std::atomic<bool> narrow{false};      // setNarrowChannel(): the channel filter of a 12.5 kHz channel, not 25 kHz
    std::atomic<int> pocsagBaudMask{7};   // setPocsagBauds(): 1 512, 2 1200, 4 2400 bd
    std::atomic<bool> pocsagSecond{true};  // setPocsagSecondPath()

    // 4.0: a second, POCSAG-only decoder behind a 7 kHz channel filter (docs/SENSITIVITY-4.0-A3.md). POCSAG (2 level FSK, +-4.5 kHz) is about 3 dB
    // more sensitive behind a 7 kHz filter than behind the 10 kHz one FLEX needs (4 level 6400 baud collapses behind it). Same samples, own pipeline;
    // its messages go to the first decoder's list (dedup: the same page seen by both is kept once), its code word counts are merged by maximum.
    std::unique_ptr<Impl> second;
    Impl* sink = nullptr;                  // set in the second decoder: where its messages go
    double finHalfHz = 0;                  // the second decoder's channel filter half width (0: the usual 10 kHz / 6.5 kHz)
    struct Recent { uint64_t key; double t; int src; };
    std::deque<Recent> recent;             // messages of the last seconds, to drop the copy the other decoder reports

    // ---- receiver thread
    PagerTelemetry tel;
    double curRate = 0;
    bool built = false;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0;                      // sum of |x|^2 of the input since the last report
    int64_t nPower = 0;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    dmr::Fir<cf32> fin;
    double offPhase = 0;
    std::vector<cf32> bufA, bufB, mixBuf, rsOut, finOut, clean;
    // coarse AFC at 96 kHz, ahead of the +-10 kHz channel filter: a radio 50 ppm off at 450 MHz puts the carrier 22 kHz out. The mean
    // frequency of 10 ms blocks with a carrier gives the error; it is only followed while no transmission is being read
    double afcHz = 0, afcEst = 0, afcPhase = 0, afcLast = 0;
    cf32 afcPrev = cf32(0, 0);
    double afcPow = 0, afcPow2 = 0, afcDisc = 0; int afcN = 0;
    std::deque<double> afcBlocks;
    std::vector<cf32> afcBuf;

    // five-tone selective calls on the discriminator audio (setSelcall)
    std::atomic<bool> selOn{false};
    SeqToneDecoder selDec;
    dmr::Fir<float> sel8;
    std::vector<float> selIn, selOut;
    double selPrevPhase = 0;
    bool selWasOn = false;

    // discriminator
    cf32 prev = cf32(0, 0);
    double phase = 0;
    std::vector<double> ring = std::vector<double>(kRing, 0.0);
    std::vector<cf32> zring = std::vector<cf32>(kRing, cf32(0, 0));   // the complex samples, for the tone-energy symbol values
    std::atomic<bool> toneChipsOn{true};   // setToneDetection(): POCSAG symbols from tone energies (default on)
    bool toneChips = true;                 // the value of toneChipsOn at the last reset (receiver thread)
    int64_t cur = -1;                      // index of the newest sample
    // channel power
    double blkPow = 0; int blkN = 0;
    double noiseFloor = 0, snrEma = 0;
    int sigHold = 0;
    bool haveFloor = false;

    // searchers
    struct Pat { double sps; int w; int off[32]; };
    Pat pat[3];                            // POCSAG
    Pat flexPat;                           // FLEX sync 1 (1600 baud)
    // peak search
    int peakH = -1; Hit best; int hold = 0;
    // lock
    bool locked = false;
    int lockH = -1;                        // 0 to 2 POCSAG, 3 FLEX
    int pol = 1;
    double m = 0, amp = 1;                 // symbol centre (carrier offset) and outer amplitude, rad/sample
    double tNext = 0, sps = 0; int w = 0, delta = 1;
    bool rebased = false;
    int speed = 0;
    // POCSAG
    pager::PocsagParser parser;
    pager::FlexPhaseCombiner flexComb;     // multi-copy combining of repeated FLEX phases (#20)
    std::atomic<bool> combineOn{true};     // setCombineCopies(): both protocols
    // the smart layer (pager_intel.h): text repair, positions, fields, incidents. Used on the receiver thread only; the setters leave requests
    pager::PagerBrain brain;
    std::atomic<bool> smartRepair{true}, smartIntel{true}, smartMedical{false}, brainClearReq{false}, placesReq{false};
    bool incidentsDirty = true;
    std::string placesText;                // guarded by mu
    int wi = 0, bi = 0; uint32_t curWord = 0; bool inSync = false;
    float wordRel[32] = {};                // the reliability of each bit of curWord, relative to the outer amplitude
    bool firstBatch = false;               // the batch after a searched sync: held until enough of its words are valid
    uint32_t held[16] = {};
    int heldValid = 0;
    std::vector<float> fr[4];              // FLEX: the same for every bit of the phases in fb
    // FLEX
    int stage = 0, cnt = 0; uint32_t acc = 0;
    const pager::FlexMode* fmode = nullptr;
    double sx = 0, sa = 0; int sn = 0;
    std::vector<uint8_t> fb[4];
    int dataIdx = 0, dataTotal = 0;
    int64_t lockStart = 0;

    // results (list and serial under mu: Clear and the results file reach them from other threads)
    std::deque<PagerMessage> list;         // newest first
    bool listDirty = false;
    uint64_t serial = 0;
    int64_t lastLockEnd = -(int64_t)1e12;
    std::vector<PagerMessage> tmp;

    void say(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = log; } if (cb) cb(s); }
    double nowSec() const { return curRate > 0 ? (double)nIn / curRate : 0; }

    void design() {
        // searchers
        for (int i = 0; i < 3; i++) {
            const double baud = i == 0 ? 512 : (i == 1 ? 1200 : 2400);
            pat[i].sps = kWork / baud;
            pat[i].w = (int)std::lround(pat[i].sps);
            for (int k = 0; k < 32; k++) pat[i].off[k] = (int)std::lround((31 - k) * pat[i].sps);
        }
        flexPat.sps = kWork / 1600.0;
        flexPat.w = 30;
        for (int k = 0; k < 32; k++) flexPat.off[k] = (31 - k) * 30;
    }

    void resetState() {
        double r, off;
        bool wideIn;
        { std::lock_guard<std::mutex> lk(mu); r = rate; off = offsetHz; curRate = r; wideIn = wideInput; }
        const uint64_t s = tel.seq;        // the report number goes on
        const double t0 = tel.timeSec;     // the clock starts again: the kept messages move back by the time it had reached
        tel = PagerTelemetry();
        tel.seq = s;
        tel.inputRate = r;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        built = false;
        stages.clear();
        offPhase = 0;
        afcHz = afcEst = afcPhase = afcLast = 0; afcPrev = cf32(0, 0); afcPow = afcPow2 = afcDisc = 0; afcN = 0; afcBlocks.clear();
        if (r >= pagerTuning().minSampleRate - 1 || (r >= kMinWideRate - 1 && wideIn)) {
            const int d = pickDecimation(r);
            double rr = r;
            for (int f : splitFactors(d)) {
                stages.emplace_back();
                const double out = rr / f;
                const double pass = 38000.0, stop = std::max(pass + 4000.0, out - pass);
                stages.back().design(dmr::lowpassTaps(pass, std::min(stop, rr * 0.5), rr, 60, 9), f);
                rr = out;
            }
            rsPass = std::fabs(rr - kMidRate) < 0.01;
            built = rsPass || rs.configure(rr, kMidRate);
            if (!rsPass) rs.reset();
            // 25 kHz channel: +-10 kHz. 12.5 kHz channel: +-6.5 kHz (the pager deviation is 4.5 kHz, so the signal fits and a neighbour
            // 12.5 kHz away, whose nearer edge is at 6.5 kHz, is cut hard instead of reaching the discriminator)
            if (finHalfHz > 0) fin.design(dmr::lowpassTaps(finHalfHz, finHalfHz + 4000.0, kMidRate, 60, 9), 2);
            else if (narrow.load()) fin.design(bbf::overrideTaps("pager", dmr::lowpassTaps(6500.0, 9500.0, kMidRate, 60, 9), kMidRate, 16000.0), 2);
            else fin.design(bbf::overrideTaps("pager", dmr::lowpassTaps(10000.0, 14000.0, kMidRate, 60, 9), kMidRate, 24000.0), 2);
            design();
        }
        (void)off;
        prev = cf32(0, 0); phase = 0; cur = -1; selPrevPhase = 0; selWasOn = false;
        toneChips = toneChipsOn.load();
        std::fill(ring.begin(), ring.end(), 0.0);
        blkPow = 0; blkN = 0; noiseFloor = 0; snrEma = 0; sigHold = 0; haveFloor = false;
        peakH = -1; locked = false; lockH = -1; hold = 0;
        parser.reset();
        parser.combine = combineOn.load();
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& m : list) m.timeSec -= t0;   // the messages stay (clearResults)
            listDirty = true;
        }
        lastLockEnd = -(int64_t)1e12;
        if (built) say("Pagers: listening for POCSAG and FLEX");
    }

    // ---- reports
    void report() {
        tel.seq++;
        tel.timeSec = nowSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.signal = sigHold > 0;
        tel.sync = locked;
        tel.state = (locked || (double)(cur - lastLockEnd) < 2.0 * kWork) ? 1 : 0;
        tel.snrDb = tel.signal ? (float)snrEma : 0.f;
        applySmartRequests();
        if (listDirty || incidentsDirty) tel.incidents = brain.incidents();
        incidentsDirty = false;
        std::lock_guard<std::mutex> lk(mu);
        if (listDirty) {
            tel.messages = std::make_shared<const std::vector<PagerMessage>>(list.begin(), list.end());
            listDirty = false;
        }
        pub = tel;
        if (second) {   // the second (narrow) POCSAG decoder: the larger code word count per speed, its signal and lock
            const PagerTelemetry& t2 = second->tel;
            for (int sp = 0; sp < kFlex1600_2; sp++) {
                const PagerSpeedStat& a = tel.speeds[sp];
                const PagerSpeedStat& b = t2.speeds[sp];
                PagerSpeedStat& o = pub.speeds[sp];
                if (b.ok + b.fixed > a.ok + a.fixed) pub.blocksOk += (b.ok + b.fixed) - (a.ok + a.fixed);
                if (b.failed > a.failed) pub.blocksBad += b.failed - a.failed;
                o.ok = std::max(a.ok, b.ok); o.fixed = std::max(a.fixed, b.fixed); o.failed = std::max(a.failed, b.failed);
                o.transmissions = std::max(a.transmissions, b.transmissions);
            }
            pub.signal = pub.signal || t2.signal;
            pub.sync = pub.sync || t2.sync;
            pub.lastPocsagSec = std::max(pub.lastPocsagSec, t2.lastPocsagSec);
            if (!tel.signal && t2.signal) pub.snrDb = t2.snrDb;
        }
    }


    // ---- results
    void applySmartRequests() {
        brain.opt.repair = smartRepair.load();
        brain.opt.intel = smartIntel.load();
        brain.opt.analyseMedical = smartMedical.load();
        if (brainClearReq.exchange(false)) brain.reset();
        if (placesReq.exchange(false)) {
            std::string t;
            { std::lock_guard<std::mutex> lk(mu); t = placesText; }
            brain.clearPlaces();
            brain.loadPlaces(t);
        }
    }
    void emit(std::vector<PagerMessage>& v, int spd, bool flex) {
        if (sink) { sink->emitFrom(v, spd, flex, 1); return; }
        emitFrom(v, spd, flex, 0);
    }
    static uint64_t msgKey(const PagerMessage& pm) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t x) { h ^= x; h *= 1099511628211ull; };
        mix(pm.address); mix((uint64_t)pm.function); mix((uint64_t)pm.type);
        for (unsigned char c : pm.text) mix(c);
        return h;
    }
    void emitFrom(std::vector<PagerMessage>& v, int spd, bool flex, int src) {
        applySmartRequests();
        for (size_t mi = 0; mi < v.size(); mi++) {
            PagerMessage& pm = v[mi];
            if (!flex) {   // the same page from both decoders: the first one counts
                const double tn = nowSec();
                while (!recent.empty() && tn - recent.front().t > 8.0) recent.pop_front();
                const uint64_t k = msgKey(pm);
                bool dup = false;
                for (const Recent& r : recent) if (r.key == k && r.src != src) { dup = true; break; }
                if (dup) continue;
                recent.push_back({k, tn, src});
            }
            pm.speed = spd;
            pm.flex = flex;
            brain.process(pm, pm.wallTime ? pm.wallTime : (int64_t)std::time(nullptr));
            uint64_t sn;
            {
                std::lock_guard<std::mutex> lk(mu);
                pm.serial = sn = ++serial;
                pm.timeSec = nowSec();
                pm.speed = spd;
                pm.flex = flex;
                list.push_front(pm);
                if (list.size() > kMaxMessages) list.pop_back();
                listDirty = true;
                incidentsDirty = true;
            }
            tel.speeds[spd].messages++;
            tel.messagesTotal++;
            tel.dataValid = true;
            if (automation::wants(automation::FPager)) {   // the automations (automation/engine.h)
                automation::Event ev("pager.message");
                ev.at(automation::wallNow(), flex ? "FLEX" : "POCSAG", 0);
                ev.str("protocol", flex ? "FLEX" : "POCSAG").str("speed", pagerSpeedName(spd)).num("address", pm.address).num("function", pm.function)
                    .str("type", pagerTypeName(pm.type)).str("text", pm.text).flag("damaged", pm.damaged);
                automation::emit(std::move(ev));
            }
            if (sn <= 40) {
                char b[160];
                snprintf(b, sizeof b, "Pagers: %s %u %s", pagerSpeedName(spd), (unsigned)pm.address, pagerTypeName(pm.type));
                say(b);
            }
        }
        v.clear();
    }
    void heard(int spd) {
        tel.lastSpeed = spd;
        if (spd >= kFlex1600_2) tel.lastFlexSec = nowSec(); else tel.lastPocsagSec = nowSec();
    }
    void addCounts(int spd, int ok, int fixed, int failed) {
        PagerSpeedStat& st = tel.speeds[spd];
        st.ok += (uint64_t)ok; st.fixed += (uint64_t)fixed; st.failed += (uint64_t)failed;
        tel.blocksOk += (uint64_t)(ok + fixed); tel.blocksBad += (uint64_t)failed;
        if (ok + fixed > 0) st.transmissions++;
    }

    // ---- the discriminator and the symbol values
    inline double v(int64_t idx, int win) const {
        return (ring[(size_t)idx & kMask] - ring[(size_t)(idx - win) & kMask]) / win;
    }

    void blockPower() {
        const double p = blkPow / blkN;
        blkPow = 0; blkN = 0;
        if (!haveFloor) { noiseFloor = p; haveFloor = true; }
        else noiseFloor = std::min(p, noiseFloor * 1.0005);
        if (p > 2.0 * noiseFloor) {
            sigHold = 30;
            const double s = 10 * std::log10(std::max(p - noiseFloor, 1e-20) / std::max(noiseFloor, 1e-20));
            snrEma = snrEma == 0 ? s : 0.9 * snrEma + 0.1 * s;
        } else if (sigHold > 0) sigHold--;
    }

    // The symbol value from tone energies instead of the mean frequency: the two tones m -+ amp (rad/sample) correlated over the symbol window
    // (noncoherent matched detection of the 2FSK), scaled to m + amp * s with s in -1 .. 1. The unwrapped phase of the discriminator takes a
    // 2 pi step at every click of a weak signal; the energies do not.
    double toneValue(int64_t idx, int win) const {
        double ar = 0, ai = 0, br = 0, bi = 0;
        const std::complex<double> sh = std::polar(1.0, -(m + amp)), sl = std::polar(1.0, -(m - amp));
        std::complex<double> rh(1, 0), rl(1, 0);
        for (int k = 0; k < win; k++) {
            const cf32 z = zring[(size_t)(idx - win + 1 + k) & kMask];
            ar += z.real() * rh.real() - z.imag() * rh.imag(); ai += z.real() * rh.imag() + z.imag() * rh.real();
            br += z.real() * rl.real() - z.imag() * rl.imag(); bi += z.real() * rl.imag() + z.imag() * rl.real();
            rh *= sh; rl *= sl;
        }
        const double eh = (ar * ar + ai * ai) * invGHi, el = (br * br + bi * bi) * invGLo, t = eh + el;
        return t > 0 ? m + amp * (eh - el) / t : m;
    }

    void onSample(cf32 z) {
        blkPow += std::norm(z);
        if (++blkN >= 480) blockPower();
        const cf32 d = z * std::conj(prev);
        if (std::norm(d) > 1e-30f) phase += std::atan2((double)d.imag(), (double)d.real());
        prev = z;
        cur++;
        ring[(size_t)cur & kMask] = phase;
        zring[(size_t)cur & kMask] = z;
        if (selOn.load(std::memory_order_relaxed)) selIn.push_back((float)((phase - selPrevPhase) * (kWork / (2 * kPi))));
        selPrevPhase = phase;
        step();
    }

    // ---- searching
    // The sync pattern against the tone energies: for each of its symbols the energy at the lower tone against the upper one, for the centre and
    // deviation the discriminator found and a few around them (it is biased in noise), the best mean of the expected sign over the 32 symbols.
    double toneSync(const Pat& P, uint32_t word, int pol, int64_t at, double mean, double score) const {
        const int win = P.w;
        if (win < 4 || at < P.off[0] + win + 2) return 0;
        double best = -1;
        static const double dm[3] = {-0.09, 0, 0.09};
        const double amps[3] = {0.45, 0.58, 0.7};
        (void)score;
        for (double d : dm) for (double a : amps) {
            const std::complex<double> sl = std::polar(1.0, -(mean + d - a)), sh = std::polar(1.0, -(mean + d + a));
            double acc = 0;
            for (int k = 0; k < 32; k++) {
                const int64_t idx = at - P.off[k];
                std::complex<double> rl(1, 0), rh(1, 0), al(0, 0), ah(0, 0);
                for (int j = 0; j < win; j++) {
                    const cf32 z = zring[(size_t)(idx - win + 1 + j) & kMask];
                    const std::complex<double> zz(z.real(), z.imag());
                    al += zz * rl; ah += zz * rh; rl *= sl; rh *= sh;
                }
                const double el = std::norm(al), eh = std::norm(ah), t = el + eh;
                const int sb = (int)((word >> (31 - k)) & 1u);
                const int bit = pol > 0 ? sb : 1 - sb;   // 1: the lower tone
                if (t > 0) acc += (bit ? el - eh : eh - el) / t;
            }
            best = std::max(best, acc / 32);
        }
        return best;
    }

    // POCSAG sync word at baud index h
    Hit evalPocsag(int h) const {
        Hit r;
        const Pat& P = pat[h];
        if (cur < P.off[0] + P.w + 1) return r;
        double x[32], mean = 0;
        const double inv = 1.0 / P.w;
        for (int k = 0; k < 32; k++) {
            const int64_t idx = cur - P.off[k];
            x[k] = (ring[(size_t)idx & kMask] - ring[(size_t)(idx - P.w) & kMask]) * inv;
            mean += x[k];
        }
        mean /= 32;
        uint32_t word = 0;
        for (int k = 0; k < 32; k++) word = (word << 1) | (x[k] < mean ? 1u : 0u);
        const int mism = popcount32(word ^ pager::kPocsagSync);
        int pl;
        const int lim = toneChips ? kSyncMism : 2;
        if (mism <= lim) pl = 1; else if (mism >= 32 - lim) pl = -1; else return r;
        double score = 0;
        for (int k = 0; k < 32; k++) score += ((pager::kPocsagSync >> (31 - k)) & 1 ? -1.0 : 1.0) * (x[k] - mean);
        score *= pl;
        score /= 32;
        if (score < kMinAmp) return r;
        if (toneChips) {
            r.ts = toneSync(P, pager::kPocsagSync, pl, cur, mean, score);
            // The tone decisions of a pattern that only looks like the sync word agree with the discriminator's hard bits, so they match about as
            // well as the discriminator did (1 - 2 mism / 32); a real sync word is matched better by the tones. Hence the threshold grows with mism.
            const int mmEff = std::min(mism, 32 - mism);
            if (mmEff > 2 && r.ts < kToneSyncMin + kToneSyncSlope * (double)mmEff) return r;
        }
        r.ok = true; r.score = score; r.mean = mean; r.pol = pl; r.at = cur;
        return r;
    }

    // FLEX: the marker in the middle of sync 1, then the sync code before it
    Hit evalFlex() const {
        Hit r;
        const Pat& P = flexPat;
        if (cur < P.off[0] + P.w + 16 * 30 + 1) return r;
        double x[32], mean = 0;
        const double inv = 1.0 / P.w;
        for (int k = 0; k < 32; k++) {
            const int64_t idx = cur - P.off[k];
            x[k] = (ring[(size_t)idx & kMask] - ring[(size_t)(idx - P.w) & kMask]) * inv;
            mean += x[k];
        }
        mean /= 32;
        uint32_t word = 0;
        for (int k = 0; k < 32; k++) word = (word << 1) | (x[k] < mean ? 1u : 0u);
        const int mism = popcount32(word ^ pager::kFlexMarker);
        int pl;
        if (mism <= 3) pl = 1; else if (mism >= 29) pl = -1; else return r;
        double score = 0;
        for (int k = 0; k < 32; k++) score += ((pager::kFlexMarker >> (31 - k)) & 1 ? -1.0 : 1.0) * (x[k] - mean);
        score *= pl;
        score /= 32;
        if (score < kMinAmp) return r;
        // the 16 symbols before the marker are the sync code
        uint32_t code = 0;
        for (int j = 0; j < 16; j++) {
            const int64_t idx = cur - (int64_t)(32 + 15 - j) * 30;
            const double xv = v(idx, 30);
            code = (code << 1) | ((pl > 0 ? xv < mean : xv > mean) ? 1u : 0u);
        }
        if (!pager::flexModeForCode((uint16_t)code, 3)) return r;
        r.ok = true; r.score = score; r.mean = mean; r.pol = pl; r.code = (uint16_t)code; r.at = cur;
        return r;
    }

    void step() {
        if (locked) { advance(); return; }
        if (peakH >= 0) {
            const Hit h = peakH == 3 ? evalFlex() : evalPocsag(peakH);
            if (h.ok && (toneChips && peakH != 3 ? h.ts > best.ts : h.score > best.score)) best = h;
            if (--hold <= 0) { const int hh = peakH; peakH = -1; startLock(hh, best); }
            return;
        }
        if (cur < 4000) return;
        const int pm = protoMask.load();
        const int bm = pocsagBaudMask.load();
        for (int h = 0; h < 4; h++) {
            if (!(pm & (h == 3 ? 2 : 1))) continue;   // that protocol is off: do not look for its sync
            if (h < 3 && !(bm & (1 << h))) continue;
            const Hit r = h == 3 ? evalFlex() : evalPocsag(h);
            if (!r.ok) continue;
            peakH = h; best = r;
            hold = (int)std::ceil((h == 3 ? 30.0 : pat[h].sps) * 0.6);
            return;
        }
    }

    // ---- coarse AFC (see afcHz)
    const cf32* afc(const cf32* c, size_t n) {
        afcBuf.resize(n);
        double w = -2 * kPi * afcHz / kMidRate;
        for (size_t i = 0; i < n; i++) {
            const cf32 y = c[i] * cf32((float)std::cos(afcPhase), (float)std::sin(afcPhase));
            afcPhase += w;
            if (afcPhase > kPi) afcPhase -= 2 * kPi; else if (afcPhase < -kPi) afcPhase += 2 * kPi;
            afcBuf[i] = y;
            const cf32 z = y * std::conj(afcPrev);
            afcPrev = y;
            afcPow += (double)std::norm(y);
            afcPow2 += (double)std::norm(y) * (double)std::norm(y);
            if (z != cf32(0, 0)) afcDisc += std::atan2(z.imag(), z.real());
            if (++afcN < 960) continue;
            const double p = afcPow / afcN, f = afcDisc / afcN * kMidRate / (2 * kPi);
            // a carrier: well above the noise floor, or (before a floor is known: a recording that starts in a transmission) an almost
            // constant envelope, which FM has and noise has not (power spread 1 for noise, near 0 for a strong carrier)
            const double spread = p > 0 ? afcPow2 / afcN / (p * p) - 1 : 1;
            afcPow = afcPow2 = afcDisc = 0; afcN = 0;
            afcBlocks.push_back(p);
            if (afcBlocks.size() > 300) afcBlocks.pop_front();
            bool carrier = spread < 0.15;
            if (afcBlocks.size() >= 20) {
                std::vector<double> v(afcBlocks.begin(), afcBlocks.end());
                std::nth_element(v.begin(), v.begin() + (ptrdiff_t)(v.size() / 10), v.end());
                carrier = carrier || p > 6 * v[v.size() / 10];
            }
            if (locked || !carrier) { afcLast = 0; continue; }
            // fast while far off (two blocks in a row agree), slowly once close; an error below 2 kHz is left to the searchers
            const bool farBy = std::fabs(f) > 2000 && std::fabs(afcLast) > 2000 && f * afcLast > 0;
            afcEst = std::max(-30000.0, std::min(30000.0, afcEst + (farBy ? 0.7 : 0.05) * (afcHz + f - afcEst)));
            afcLast = f;
            if (std::fabs(afcEst) > 3000) afcHz = afcEst; else if (std::fabs(afcEst) < 1500) afcHz = 0;
            w = -2 * kPi * afcHz / kMidRate;
        }
        return afcBuf.data();
    }

    // The tone-energy values need the two tone frequencies; the discriminator's mean and outer amplitude are biased by noise. The 32 symbols of the sync
    // word are known: a grid search over the centre and the deviation for the pair that puts the most energy on the expected tones (and the least on the
    // other ones) over the whole word.
    void refineTones(int h, const Hit& hit) {
        const Pat& P = h < 3 ? pat[h] : flexPat;
        const uint32_t word = h < 3 ? pager::kPocsagSync : pager::kFlexMarker;
        const int win = P.w;
        if (win < 4 || hit.at < P.off[0] + win + 2) return;
        auto energy = [&](int64_t idx, double om) {
            std::complex<double> acc(0, 0), r(1, 0);
            const std::complex<double> st = std::polar(1.0, -om);
            for (int k = 0; k < win; k++) { const cf32 z = zring[(size_t)(idx - win + 1 + k) & kMask]; acc += std::complex<double>(z.real(), z.imag()) * r; r *= st; }
            return std::norm(acc);
        };
        double bestScore = -1e300, bm = m, ba = amp, bgh = 1, bgl = 1;
        const double m0 = hit.mean;
        for (int im = -6; im <= 6; im++) {
            const double mm = m0 + 0.03 * im;
            for (int ia = 0; ia <= 12; ia++) {
                const double aa = 0.3 + 0.05 * ia;
                double sc = 0, gh = 0, gl = 0;
                int nh = 0, nl = 0;
                for (int k = 0; k < 32; k++) {
                    const int64_t idx = hit.at - P.off[k];
                    const int sb = (int)((word >> (31 - k)) & 1u);
                    const int bit = hit.pol > 0 ? sb : 1 - sb;     // 1: the lower tone
                    const double eLo = energy(idx, mm - aa), eHi = energy(idx, mm + aa);
                    sc += bit ? eLo - eHi : eHi - eLo;
                    if (bit) { gl += eLo; nl++; } else { gh += eHi; nh++; }
                }
                if (sc > bestScore) { bestScore = sc; bm = mm; ba = aa; bgh = nh ? gh / nh : 0; bgl = nl ? gl / nl : 0; }
            }
        }
        if (std::fabs(bm) > kToneMaxOffset) { toneOk = false; return; }   // an off-centre carrier: the channel filter cuts the upper tone, the discriminator copes better
        m = bm; amp = ba; toneOk = true;
        const double g = 0.5 * (bgh + bgl);
        invGHi = bgh > 0.3 * g ? 1.0 / bgh : 1.0 / std::max(g, 1e-30);   // the two energies are from few symbols: a wild ratio is not believed
        invGLo = bgl > 0.3 * g ? 1.0 / bgl : 1.0 / std::max(g, 1e-30);
    }

    // ---- reading a transmission
    void startLock(int h, const Hit& hit) {
        locked = true; lockH = h; pol = hit.pol;
        m = hit.mean; amp = std::max(hit.score, 0.1);
        invGHi = invGLo = 1; toneOk = false;
        if (toneChips && h < 3) refineTones(h, hit);
        tel.cfoHz = m * kWork / (2 * kPi) + afcHz;
        parser.reset();
        wi = bi = 0; curWord = 0; inSync = false;
        firstBatch = true; heldValid = 0;
        stage = 0; cnt = 0; acc = 0;
        lockStart = cur;
        if (h < 3) {
            speed = h == 0 ? kPocsag512 : (h == 1 ? kPocsag1200 : kPocsag2400);
            sps = pat[h].sps; w = pat[h].w;
        } else {
            fmode = pager::flexModeForCode(hit.code, 3);
            speed = fmode->speed;
            sps = 30; w = 30;
        }
        delta = std::max(1, (int)std::lround(sps / 4));
        tNext = (double)hit.at + sps;
    }

    void endLock() {
        if (lockH < 3) {
            parser.flush(tmp);
            emit(tmp, speed, false);
            if (parser.ok + parser.fixed > 0) { addCounts(speed, parser.ok, parser.fixed, parser.failed); heard(speed); }
            parser.ok = parser.fixed = parser.failed = 0;
        }
        locked = false; lockH = -1;
        lastLockEnd = cur;
    }

    void advance() {
        for (;;) {
            const int64_t idx = (int64_t)std::llround(tNext);
            if (idx + delta > cur) return;
            if (idx - delta - w < cur - (int64_t)kRing + 8) { endLock(); return; }        // fell too far behind: lost
            const double xd = v(idx, w);
            const bool two = lockH < 3;   // FLEX keeps the discriminator (see docs/SENSITIVITY-4.0-B.md)
            const double x = toneOk && two && !(lockH == 3 && stage == 1) && !(lockH == 3 && stage == 3) ? toneValue(idx, w) : xd;
            xDisc = xd;
            const double e = (std::fabs(v(idx + delta, w) - m) - std::fabs(v(idx - delta, w) - m)) / std::max(amp, 0.05);
            const double adj = std::max(-1.0, std::min(1.0, kTimingGain * e * sps * 0.5));
            const double tCur = tNext;
            rebased = false;
            symbol(x, tCur);
            if (!locked) return;
            if (!rebased) tNext = tCur + sps + adj;
        }
    }

    bool toneOk = false;             // this lock reads tone energies
    double invGHi = 1, invGLo = 1;   // the inverse signal energy of each tone (the channel filter's slope makes them differ with a carrier offset)
    double xDisc = 0;   // the discriminator's value of the symbol being read (the centre follows it; the tone value does not carry the offset)
    void symbol(double x, double tCur) {
        if (lockH < 3) pocsagSymbol(x);
        else flexSymbol(x, tCur);
    }

    inline int bitOf(double x) const { return pol > 0 ? (x < m ? 1 : 0) : (x > m ? 1 : 0); }

    void pocsagSymbol(double x) {
        curWord = (curWord << 1) | (uint32_t)bitOf(x);
        wordRel[bi] = (float)(std::fabs(x - m) / std::max(amp, 0.05));
        m += 0.004 * (xDisc - m);
        if (++bi < 32) return;
        bi = 0;
        if (inSync) {
            // the batch is over: commit its code words, then the next must start with the sync word
            const bool good = parser.ok + parser.fixed > 0;
            if (good) { addCounts(speed, parser.ok, parser.fixed, parser.failed); heard(speed); }
            parser.ok = parser.fixed = parser.failed = 0;
            if (popcount32(curWord ^ pager::kPocsagSync) <= 4 && good) { inSync = false; wi = 0; }
            else { parser.flush(tmp); emit(tmp, speed, false); locked = false; lockH = -1; lastLockEnd = cur; }
        } else {
            uint32_t wd = chaseWord(curWord, wordRel, kDoubtText);
            // a soft repair may complete a message or an idle word, never start a page: a wrong address is a page that was never sent
            if (wd != curWord && pager::pocsagClassify(wd).kind == pager::PocsagWord::Address) wd = curWord;
            if (firstBatch) {
                // The batch after a sync the search found is held back until it proves itself: a sync in noise is rare but the BCH repair
                // accepts a quarter of random words, which made pages out of nothing. A real batch has most of its words valid.
                held[wi] = wd;
                if (pager::correctWord(curWord).ok) heldValid++;   // the hard decisions alone: the soft repair makes noise look valid too
                if (++wi < 16) return;
                inSync = true;
                firstBatch = false;
                if (heldValid < kFirstBatchValid) { parser.reset(); parser.ok = parser.fixed = parser.failed = 0; locked = false; lockH = -1; return; }
                for (int k = 0; k < 16; k++) { parser.word(held[k], k, tmp); if (!tmp.empty()) emit(tmp, speed, false); }
                return;
            }
            parser.word(wd, wi, tmp);
            if (!tmp.empty()) emit(tmp, speed, false);
            if (++wi == 16) inSync = true;
        }
    }

    void flexSymbol(double x, double tCur) {
        switch (stage) {
        case 0:   // the complement of the sync code
            acc = (acc << 1) | (uint32_t)bitOf(x);
            if (++cnt == 16) {
                if (popcount32((acc & 0xFFFF) ^ (uint32_t)(uint16_t)~fmode->code) > 4) { endLock(); return; }
                stage = 1; cnt = 0; acc = 0;
            }
            break;
        case 1:   // 16 symbols of dotting
            if (++cnt == 16) { stage = 2; cnt = 0; acc = 0; }
            break;
        case 2: { // the frame information word: like the data, sent complemented against the sync code and marker (pager_gen.cpp flexFrameTx)
            acc = (acc << 1) | (uint32_t)(1 - bitOf(x));
            wordRel[cnt] = (float)(std::fabs(x - m) / std::max(amp, 0.05));
            if (++cnt < 32) break;
            const pager::Fixed c = pager::correctWord(chaseWord(acc, wordRel, kDoubt));
            int cycle, frame;
            if (!c.ok || !pager::flexFiwCheck(pager::rev21(c.data), cycle, frame)) {
                // A bad FIW is common on a real channel; dropping lock here threw away the 1600/4 and 3200/4 pages.
                stage = 0; cnt = 0; acc = 0; return;
            }
            // sync 2 follows at the data rate
            sps = kWork / fmode->baud; w = (int)std::lround(sps);
            delta = std::max(1, (int)std::lround(sps / 4));
            tNext = tCur + sps; rebased = true;
            stage = 3; cnt = 0; sx = sa = 0; sn = 0;
            break;
        }
        case 3: { // sync 2: dotting, which gives the centre and the outer amplitude at the data rate
            cnt++;
            if (cnt > 6) { sx += x; sn++; sa += std::fabs(x - m); }
            if (cnt >= fmode->baud * 25 / 1000) {
                if (sn > 4) {
                    const double m2 = sx / sn, a2 = sa / sn;      // a2: mean distance from the old centre, close enough to the outer amplitude
                    if (std::fabs(m2 - m) < 0.35 * amp) m = m2;
                    if (a2 > 0.6 * amp && a2 < 1.5 * amp) amp = a2;
                }
                stage = 4; dataIdx = 0;
                dataTotal = fmode->baud == 1600 ? 2816 : 5632;
                for (auto& b : fb) b.assign(2816, 0);
                for (auto& r : fr) r.assign(2816, 1.f);
            }
            break;
        }
        default: { // the data
            const int bidx = dataIdx;
            int a1 = 0, b1 = 0;
            float ra = 0, rb = 0;   // their reliabilities: 0 on the decision threshold, 1 on a clean level
            // the data bits are the complement of the sync sense (bitOf); in 4-level frames the inner levels carry a second bit of 1
            if (fmode->levels == 2) { a1 = 1 - bitOf(x); ra = (float)(std::fabs(x - m) / std::max(amp, 0.05)); }
            else {
                const double y = (x - m) / std::max(amp, 0.05) * pol;
                if (y < -0.667) { a1 = 0; b1 = 0; } else if (y < 0) { a1 = 0; b1 = 1; } else if (y < 0.667) { a1 = 1; b1 = 1; } else { a1 = 1; b1 = 0; }
                ra = (float)(std::fabs(y) * 3.0); rb = (float)(std::fabs(std::fabs(y) - 0.667) * 3.0);
            }
            const int spd = fmode->speed;
            auto put = [&](int ph, int at, int bit, float r) { fb[ph][(size_t)at] = (uint8_t)bit; fr[ph][(size_t)at] = r; };
            if (spd == kFlex1600_2) put(0, bidx, a1, ra);
            else if (spd == kFlex3200_4) { put(0, bidx, a1, ra); put(1, bidx, b1, rb); }
            else if (spd == kFlex3200_2) put(bidx & 1, bidx >> 1, a1, ra);
            else { const int o = (bidx & 1) * 2; put(o, bidx >> 1, a1, ra); put(o + 1, bidx >> 1, b1, rb); }
            if (++dataIdx >= dataTotal) finishFrame();
            break;
        }
        }
    }

    void finishFrame() {
        const int nph = fmode->speed == kFlex1600_2 ? 1 : (fmode->speed == kFlex6400_4 ? 4 : 2);
        pager::FlexPhaseStat st[4];
        std::vector<PagerMessage> got;
        bool any = false;
        int ok = 0, fixed = 0, failed = 0;
        for (int p = 0; p < nph; p++) {
            // Soft repair of the words the BCH code alone cannot repair (chaseWord); each block interleaves 8 words bit by bit. The block
            // information word (word 0) says where the address words are: those are repaired only where the bits are doubtful (a wrong
            // address is a page that was never sent), the vector and message words more boldly (a wrong one only damages a page's text).
            uint32_t words[pager::kFlexPhaseWords];
            float rels[pager::kFlexPhaseWords][32];
            for (int i = 0; i < pager::kFlexPhaseWords; i++) {
                words[i] = 0;
                for (int j = 0; j < 32; j++) {
                    const size_t at = (size_t)((i / 8) * 256 + j * 8 + i % 8);
                    words[i] = (words[i] << 1) | (fb[p][at] & 1u);
                    rels[i][j] = fr[p][at];
                }
            }
            int aoff = 1, voff = 1;   // the address words: [aoff, voff)
            {
                const pager::Fixed b = pager::correctWord(chaseWord(words[0], rels[0], kDoubt));
                if (b.ok) {
                    const uint32_t f0 = pager::rev21(b.data);
                    aoff = (int)((f0 >> 8) & 3) + 1;
                    voff = (int)((f0 >> 10) & 0x3F);
                    if (voff < aoff) voff = aoff;
                } else voff = pager::kFlexPhaseWords;   // unknown: every word is treated as an address
            }
            for (int i = 0; i < pager::kFlexPhaseWords; i++) {
                const bool addr = i == 0 || (i >= aoff && i < voff);
                const uint32_t fixedW = chaseWord(words[i], rels[i], addr ? kDoubt : kDoubtText);
                if (fixedW != words[i])
                    for (int j = 0; j < 32; j++) fb[p][(size_t)((i / 8) * 256 + j * 8 + i % 8)] = (uint8_t)((fixedW >> (31 - j)) & 1u);
            }
            const size_t g0 = got.size();
            const int recovered = combineOn.load() ? flexComb.process(fmode->speed * 4 + p, fb[p].data(), (int64_t)std::time(nullptr)) : 0;
            pager::flexDecodePhase(fb[p].data(), got, st[p]);
            if (recovered > 0) for (size_t k = g0; k < got.size(); k++) { got[k].combined = true; got[k].copies = 2; }
            any |= st[p].biwOk;
            ok += st[p].ok; fixed += st[p].fixed; failed += st[p].failed;
        }
        if (any) { addCounts(speed, ok, fixed, failed); heard(speed); }
        emit(got, speed, true);
        locked = false; lockH = -1;
        lastLockEnd = cur;
    }

    // ---- the chain
    // ---- five-tone selective calls: the discriminator at 48 kHz, down to 8 kHz, into the sequential decoder
    void runSelcall() {
        const bool on = selOn.load();
        if (on != selWasOn) {
            selWasOn = on;
            sel8.reset(); selDec.reset();
            if (on) {
                sel8.design(dmr::lowpassTaps(3200.0, 4600.0, kWork, 50, 9), 6);
                selDec.setCallback([this](const SeqCall& c) { emitSelcall(c); });
            }
        }
        if (!on || selIn.empty()) return;
        selOut.clear();
        sel8.process(selIn.data(), selIn.size(), selOut);
        if (!selOut.empty()) selDec.feed(selOut.data(), selOut.size());
    }
    void emitSelcall(const SeqCall& c) {
        PagerMessage pm;
        pm.selcall = true;
        pm.selStd = c.standard;
        pm.text = c.digits;
        pm.type = kPagerNumeric;
        pm.address = 0;
        pm.function = -1;
        pm.wallTime = (int64_t)std::time(nullptr);
        {
            std::lock_guard<std::mutex> lk(mu);
            pm.serial = ++serial;
            pm.timeSec = nowSec();
            list.push_front(pm);
            if (list.size() > kMaxMessages) list.pop_back();
            listDirty = true;
        }
        tel.messagesTotal++;
        tel.dataValid = true;
        say("Pagers: " + c.standard + " selective call " + c.digits);
    }

    void feed(const cf32* x, size_t n) {
        if (!built || n == 0) return;
        for (size_t i = 0; i < n; i++) power += (double)std::norm(x[i]);
        nPower += (int64_t)n;
        const cf32* in = x;
        bool bad = false;
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real(), im = x[i].imag();
            if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; }
        }
        if (bad) {
            clean.assign(x, x + n);
            for (auto& s : clean) if (!(std::fabs(s.real()) < 1e4f && std::fabs(s.imag()) < 1e4f)) s = cf32(0, 0);
            in = clean.data();
        }
        double off;
        { std::lock_guard<std::mutex> lk(mu); off = offsetHz; }
        const cf32* c = in;
        size_t cn = n;
        if (off != 0) {
            mixBuf.resize(n);
            const double wm = -2 * kPi * off / curRate;
            const cf32 stp((float)std::cos(wm), (float)std::sin(wm));
            cf32 ph((float)std::cos(offPhase), (float)std::sin(offPhase));
            for (size_t i = 0; i < n; i++) {
                mixBuf[i] = in[i] * ph;
                ph *= stp;
                if ((i & 1023) == 1023) ph /= std::abs(ph);
            }
            offPhase = std::fmod(offPhase + wm * (double)n, 2 * kPi);
            c = mixBuf.data();
        }
        for (size_t s = 0; s < stages.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? bufB : bufA;
            dst.clear();
            stages[s].process(c, cn, dst);
            c = dst.data(); cn = dst.size();
        }
        if (!rsPass) {
            rsOut.clear();
            rs.process(c, cn, rsOut);
            c = rsOut.data(); cn = rsOut.size();
        }
        c = afc(c, cn);
        finOut.clear();
        fin.process(c, cn, finOut);
        // the report instants are in input samples; the decoding runs a little ahead of them
        nIn += (int64_t)n;
        selIn.clear();
        for (const cf32& z : finOut) onSample(z);
        runSelcall();
        while ((double)nIn >= nextReport) {
            nextReport += 0.25 * curRate;
            report();
        }
    }
};

PagerReceiver::PagerReceiver() : p_(std::make_unique<Impl>()) {}
PagerReceiver::~PagerReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void PagerReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
void PagerReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    p_->resetReq = true;
}
bool PagerReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= pagerTuning().minSampleRate - 1;
}
void PagerReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    auto keep = p_->pub.messages;        // the messages stay (clearResults)
    p_->pub = PagerTelemetry();
    p_->pub.seq = s;
    p_->pub.messages = std::move(keep);
}

void PagerReceiver::clearResults() {
    p_->brainClearReq = true;                // what was learned goes with the messages
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->list.clear();
    p_->listDirty = true;
    p_->pub.messages.reset();
    p_->pub.seq++;
}
std::string PagerReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->serial, p_->list);
}
bool PagerReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    uint64_t serial = 0;
    std::deque<PagerMessage> list;
    if (!results::unpack(blob, t0, serial, list)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& m : list) m.timeSec += p_->pub.timeSec - t0;
    p_->list = std::move(list);
    p_->serial = std::max(p_->serial, serial);
    p_->listDirty = true;
    p_->pub.messages = std::make_shared<const std::vector<PagerMessage>>(p_->list.begin(), p_->list.end());
    p_->pub.seq++;
    return true;
}

void PagerReceiver::feed(const cf32* x, size_t n) {
    Impl& im = *p_;
    const bool reset = im.resetReq.exchange(false);
    if (reset) im.resetState();
    if (im.curRate <= 0) return;
    im.feed(x, n);
    // 4.0: the second, POCSAG-only decoder behind the 7 kHz filter, on a 25 kHz channel (a 12.5 kHz channel's filter is narrower already)
    const bool want = (im.protoMask.load() & 1) && !im.narrow.load() && im.pocsagSecond.load();
    if (!want) { im.second.reset(); return; }
    if (!im.second) { im.second = std::make_unique<Impl>(); im.second->sink = &im; im.second->finHalfHz = 7000.0; im.second->resetReq = true; }
    Impl& s2 = *im.second;
    if (reset || s2.resetReq.load()) {
        { std::lock_guard<std::mutex> lk(im.mu); s2.rate = im.rate; s2.offsetHz = im.offsetHz; s2.wideInput = im.wideInput; }
        s2.protoMask = 1; s2.narrow = false; s2.pocsagBaudMask = im.pocsagBaudMask.load(); s2.toneChipsOn = im.toneChipsOn.load();
        s2.selOn = false; s2.combineOn = im.combineOn.load();
        s2.resetReq = false;
        s2.resetState();
    }
    s2.pocsagBaudMask = im.pocsagBaudMask.load();
    if (s2.curRate > 0) s2.feed(x, n);
}

bool PagerReceiver::telemetry(PagerTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    sess::observePager(out);
    return true;
}
void PagerReceiver::setProtocols(int mask) { p_->protoMask = mask & 3; }
int PagerReceiver::protocols() const { return p_->protoMask; }
int PagerReceiver::pocsagBauds() const { return p_->pocsagBaudMask; }
void PagerReceiver::setWideInput(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->wideInput = on; p_->resetReq = true; }
void PagerReceiver::setNarrowChannel(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->narrow.exchange(on) != on) p_->resetReq = true;   // the filter is rebuilt by the receiver thread
}
bool PagerReceiver::narrowChannel() const { return p_->narrow; }
void PagerReceiver::setSmartRepair(bool on) { p_->smartRepair = on; }
bool PagerReceiver::smartRepair() const { return p_->smartRepair; }
void PagerReceiver::setSmartInsight(bool on) { p_->smartIntel = on; }
bool PagerReceiver::smartInsight() const { return p_->smartIntel; }
void PagerReceiver::setAnalyseMedical(bool on) { p_->smartMedical = on; }
bool PagerReceiver::analyseMedical() const { return p_->smartMedical; }
void PagerReceiver::setPlaces(const std::string& csv) { { std::lock_guard<std::mutex> lk(p_->mu); p_->placesText = csv; } p_->placesReq = true; }
void PagerReceiver::setCombineCopies(bool on) { p_->combineOn = on; }
bool PagerReceiver::combineCopies() const { return p_->combineOn; }
void PagerReceiver::setSelcall(bool on) { p_->selOn = on; }
bool PagerReceiver::selcall() const { return p_->selOn; }
void PagerReceiver::setPocsagBauds(int mask) { p_->pocsagBaudMask = mask & 7; }
void PagerReceiver::setPocsagSecondPath(bool on) { p_->pocsagSecond = on; }
bool PagerReceiver::pocsagSecondPath() const { return p_->pocsagSecond.load(); }
void PagerReceiver::setToneDetection(bool on) { p_->toneChipsOn = on; p_->resetReq = true; }
bool PagerReceiver::toneDetection() const { return p_->toneChipsOn.load(); }
void PagerReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning pagerTuning() {
    ModeTuning t;
    t.stdMode = 25; t.id = "pager"; t.name = "Pagers";
    t.minMhz = 25; t.maxMhz = 1000; t.defMhz = 466.075;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as Inmarsat-C and Aero)
    return t;
}

} // namespace dect2
