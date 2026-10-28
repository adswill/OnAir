// DTMB receiver (GB 20600-2006, multi-carrier C=3780 mode): matched filter and resampling to the symbol rate, frame search from the PN headers,
// tracking (timing, carrier offset, channel from the headers), header leakage removal and equalisation of the OFDM body, system information,
// then the channel decoder. See docs/modes/dtmb.md.
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/channels/chan_locate.h"
#include "dect2/dtmb/dtmb_chain.h"
#include "dect2/dtmb/dtmb_demod.h"
#include "dect2/dtmb/dtmb_front.h"
#include "dect2/dtmb/dtmb_map.h"
#include "dect2/dtmb/dtmb_sync.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace dtmb;

namespace {
using dect2::math::kPi;
constexpr float kPeakNoiseRatio = 25.f;    // a header counts when its strongest tap stands this far (power) above the estimation noise
constexpr int kLostFrames = 12;            // frames without a usable header before the lock is given up
}

namespace {

// DTMB_PROFILE=1 in the environment: time per stage, printed to stderr when the receiver is destroyed
#ifndef _WIN32
inline double threadSeconds() { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec; }   // CPU time of the thread: a busy machine does not inflate it
#else
inline double threadSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#endif
struct Prof {
    bool on = std::getenv("DTMB_PROFILE") != nullptr;
    double sec[16] = {};
    static constexpr const char* kName[16] = {"front end (matched filter + resample)", "header estimates", "equaliser", "system information", "pushFrame (de-interleave + LLR)", "pollChain", "feed() total", "displays (cells, impulse response)", "front end pull + carrier derotation", "buffer trimming", "input level, mixer, channel search", "frame search after lost samples", "first search for the signal", "rest of the frame loop", "window adaptation (wide header estimate)", "normalise + decision error"};
    struct Scope {
        Prof& p; int k; double t0 = 0;
        Scope(Prof& pr, int kk) : p(pr), k(kk) { if (p.on) t0 = threadSeconds(); }
        ~Scope() { if (p.on) p.sec[k] += threadSeconds() - t0; }
    };
};

// System information of one hypothesis of the carrier mode: the correlation of the 36 symbols with the 22 words, accumulated over frames
struct SiTracker {
    float acc[22] = {};
    int count = 0, cand = -1, run = 0;
    float score = 0;
    void reset() { std::fill(acc, acc + 22, 0.f); count = 0; cand = -1; run = 0; score = 0; }
    // The index (3 .. 24) once it has led clearly in three frames in a row, else -1
    int update(const cf32* si, const float* sivar) {
        float w[kSiSymbols], sc[22];
        for (int i = 0; i < kSiSymbols; i++) w[i] = 1.f / sivar[i];
        siScores(si, w, sc);
        for (int i = 0; i < 22; i++) acc[i] = 0.7f * acc[i] + 0.3f * sc[i];
        count++;
        int best = 0, second = 1;
        if (acc[1] > acc[0]) std::swap(best, second);
        for (int i = 2; i < 22; i++) { if (acc[i] > acc[best]) { second = best; best = i; } else if (acc[i] > acc[second]) second = i; }
        // 0.7^n weights: after n frames the accumulated score is (1 - 0.7^n) times the mean
        const float norm = 1.f - std::pow(0.7f, (float)std::min(count, 30));
        score = acc[best] / std::max(norm, 0.3f);
        const float margin = (acc[best] - acc[second]) / std::max(norm, 0.3f);
        if (score > 0.45f && margin > 0.35f) {
            if (best == cand) run++; else { cand = best; run = 1; }
        } else run = 0;
        return run >= 3 ? best + 3 : -1;
    }
};

// The gain of the body against the header is a property of the transmitter (the header is boosted, or not): the median of the last 15 frames, so
// that frames hit by an impulse do not move it
struct GainState {
    static constexpr int kHist = 15;
    float hist[kHist] = {};
    uint64_t count = 0;
    void reset() { count = 0; }
    double update(double g) {
        hist[(size_t)(count++ % kHist)] = (float)g;
        float tmp[kHist];
        const int n = (int)std::min<uint64_t>(count, kHist);
        std::copy(hist, hist + n, tmp);
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        return tmp[n / 2];
    }
};

} // namespace

struct DtmbReceiver::Impl {
    // ---- input
    double inRate = 0;
    bool rateOk = false;
    SrrcResampler front;
    std::atomic<bool> resetReq{false};
    std::vector<cf32> tmp;
    double levelAcc = 0; uint64_t levelN = 0; float levelDb = -120;

    // ---- symbol stream
    std::vector<cf32> buf;                 // buf[i] is symbol number base + i
    long base = 0;
    double cfoHz = 0, derotPhase = 0;      // the stream is multiplied by exp(j derotPhase); the phase moves by -2 pi cfoHz / symbol rate per symbol
    long reportAt = 0;                     // symbol count of the next telemetry report

    // ---- search
    Acquirer acq;
    double symRate = kSymbolRate;   // 7.56 Msym/s, or 5.67 in a 6 MHz channel
    int hyp = 0;
    long sinceAttempt = 1 << 30;
    std::vector<cf32> block;

    // ---- tracking
    int state = 0;                         // 0 searching, 1 tracking, 2 decoding
    Header hdr = Header::Pn945;
    bool rotates = true;
    long S = 0;                            // first symbol of the header of the frame being processed
    int fnum = 0;                          // its number inside the super-frame
    std::unique_ptr<HeaderEstimator> est, estWide;   // est: the window that fits the channel, estWide: the widest the header allows
    Taps rawA, rawB, cleanA, cleanB, rawW, cleanW;
    bool haveA = false, haveWide = false;
    BodyEqualizer eq;
    SingleCarrierEqualizer eq1;
    std::vector<cf32> bins, sym1;
    std::vector<float> var, var1;
    struct Body { std::vector<cf32> si, data; std::vector<float> sivar, dvar; GainState gain; SiTracker tracker; };
    Body body[2];                          // the 36 system information symbols and the 3744 data symbols under each hypothesis: 0 multi-carrier, 1 single carrier
    int carriers = 3780;                   // the mode of the signal once the system information is known
    int siLost = 0;
    uint64_t frames = 0;
    int lostRun = 0;
    double lossAvg = 0;
    double snrPn = 0, merAvg = 0, frameErr = 0; bool snrInit = false;
    double timeInt = 0;
    int timeJumpRun = 0;
    float echoSpanUs = 0, peakOffUs = 0;
    uint64_t superIdx = 0;
    int windowPre = 16, windowPost = 100, wideMaxPre = 64, widePost = 128, sinceWide = 0;

    // ---- system information
    int siIndex = 0;
    bool siOk = false;
    Profile profile;

    // ---- channel decoder
    std::unique_ptr<FecChain> chain;
    std::atomic<int> decThreads{std::max(1, std::min(4, (int)std::thread::hardware_concurrency() / 2))};   // set from the interface thread
    std::atomic<int> computeMode{2};                      // 0 CPU, 1 GPU, 2 auto
    uint64_t pktTotal = 0;
    ChainStats lastStats;
    long lastGoodAt = (std::numeric_limits<long>::min() / 2);
    std::function<void(const uint8_t*, size_t, double)> pktCb;
    std::function<void(const std::string&)> logCb;

    // ---- telemetry
    std::vector<cf32> scrub;   // the input with non-finite samples replaced
    // not cleared by doReset(): where the channel sits in the input band, and whether the spectrum is mirrored
    ChannelLocator locator;
    ChannelMixer mixer;
    bool mirror = false, flipReq = false;
    int flips = 0;
    std::mutex mu;
    Prof prof;
    double inSamples = 0;
    DtmbTelemetry tel;
    uint64_t seq = 0;
    std::vector<cf32> cells;
    std::vector<float> cir;
    int cirFirst = 0;

    void log(const std::string& s) { if (logCb) logCb(s); }

    void resetSi() {
        siOk = false; siIndex = 0; carriers = 3780; siLost = 0;
        for (Body& b : body) { b.tracker.reset(); b.gain.reset(); }
    }

    // ------------------------------------------------------------------ plumbing
    long produced() const { return base + (long)buf.size(); }

    size_t pullMore(size_t want) {
        Prof::Scope pp(prof, 8);
        tmp.clear();
        const size_t n = front.pull(tmp, want);
        if (!n) return 0;
        const double dph = -2.0 * kPi * cfoHz / symRate;
        cf32 step((float)std::cos(dph), (float)std::sin(dph));
        cf32 rot((float)std::cos(derotPhase), (float)std::sin(derotPhase));
        const size_t at = buf.size();
        buf.resize(at + n);
        // Four rotators, so that the multiplications do not wait for each other. (Written out: std::complex multiplication checks for NaN and
        // calls a library routine unless the compiler is allowed to ignore them.)
        auto mul = [](cf32 a, cf32 b) { return cf32(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()); };
        cf32 r4[4];
        {
            cf32 r = rot;
            for (int k = 0; k < 4; k++) { r4[k] = r; r = mul(r, step); }
        }
        const cf32 step2 = mul(step, step), step4 = mul(step2, step2);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            for (int k = 0; k < 4; k++) { buf[at + i + (size_t)k] = mul(tmp[i + (size_t)k], r4[k]); r4[k] = mul(r4[k], step4); }
            if ((i & 1023) == 1020) for (int k = 0; k < 4; k++) r4[k] /= std::abs(r4[k]);
        }
        rot = r4[0];
        for (; i < n; i++) {
            buf[at + i] = mul(tmp[i], rot);
            rot = mul(rot, step);
        }
        derotPhase = std::fmod(derotPhase + dph * (double)n, 2.0 * kPi);
        return n;
    }

    // make symbols with absolute index < upTo available; false when the front end has no more input
    bool ensure(long upTo) {
        while (produced() < upTo) {
            if (!pullMore((size_t)(upTo - produced()))) return false;
        }
        return true;
    }

    const cf32* at(long i) const { return &buf[(size_t)(i - base)]; }

    void dropBefore(long idx) {
        if (idx - base > 8192) {
            Prof::Scope pt(prof, 9);
            const size_t d = (size_t)(idx - base);
            buf.erase(buf.begin(), buf.begin() + (long)d);
            base += (long)d;
        }
    }

    void doReset() {
        front.reset();
        buf.clear(); base = 0;
        cfoHz = 0; derotPhase = 0;
        reportAt = (long)(symRate * 0.25);
        hyp = 0; sinceAttempt = 1 << 30;
        state = 0; haveA = false; haveWide = false; frames = 0; lostRun = 0; lossAvg = 0;
        snrInit = false; timeInt = 0; timeJumpRun = 0;
        resetSi();
        if (chain) chain->reset();
        chain.reset();
        lastStats = ChainStats();
        pktTotal = 0;
        lastGoodAt = (std::numeric_limits<long>::min() / 2);
        cells.clear(); cir.clear();
    }

    // ------------------------------------------------------------------ search
    void search() {
        for (;;) {
            const size_t got = pullMore(65536);
            if (!got) break;
            sinceAttempt += (long)got;
            // keep the block the search needs and a little more
            if ((long)buf.size() > Acquirer::kBlock + 131072) dropBefore(produced() - Acquirer::kBlock - 16384);
            if ((int)buf.size() >= Acquirer::kBlock && sinceAttempt >= 262144) {
                sinceAttempt = 0;
                if (attempt()) return;
            }
            if (report()) {}
        }
    }

    bool attempt() {
        // the correlation with the 255 / 511 chip core tolerates about +-5 kHz, so the offsets are tried 8 kHz apart; the header fit then finds the
        // offset itself within +-14 kHz of the one tried
        // A UHF radio 50 ppm off is up to 43 kHz away: hypotheses out to +-40 kHz, the fit covering the rest
        static const double hyps[11] = {0, 8000, -8000, 16000, -16000, 24000, -24000, 32000, -32000, 40000, -40000};
        const double h = hyps[hyp % 11];
        hyp++;
        const long first = produced() - Acquirer::kBlock;
        block.resize((size_t)Acquirer::kBlock);
        const double dph = -2.0 * kPi * h / symRate;
        cf32 rot(1, 0), step((float)std::cos(dph), (float)std::sin(dph));
        for (int i = 0; i < Acquirer::kBlock; i++) {
            block[(size_t)i] = *at(first + i) * rot;
            rot *= step;
            if ((i & 1023) == 1023) rot /= std::abs(rot);
        }
        AcqResult r;
        if (!acq.run(block.data(), r)) return false;
        // The correlation peaks of a block that is still several kHz off are not reliable (the position in the super-frame can come out wrong), so
        // the block is derotated by the offset the headers gave and searched again: the second result is the one that counts.
        double extra = r.cfoHz;
        if (std::fabs(extra) > 1000.0) {
            const double dph2 = -2.0 * kPi * extra / symRate;
            cf32 rot2(1, 0), step2((float)std::cos(dph2), (float)std::sin(dph2));
            for (int i = 0; i < Acquirer::kBlock; i++) {
                block[(size_t)i] *= rot2;
                rot2 *= step2;
                if ((i & 1023) == 1023) rot2 /= std::abs(rot2);
            }
            AcqResult r2;
            if (!acq.run(block.data(), r2)) return false;
            extra += r2.cfoHz;
            r = r2;
        }
        // the block was derotated by cfoHz (the stream) and h (the hypothesis); `extra` is what the headers found in it
        const double cfoNew = cfoHz + h + extra;
        startTrack(r, first + r.start, cfoNew);
        return true;
    }

    void startTrack(const AcqResult& r, long start, double newCfo) {
        hdr = r.header; rotates = r.rotates; S = start; fnum = r.frame;
        // re-derotate what is already in the buffer: the stream carried a derotation of cfoHz, it should carry newCfo
        {
            const long n0 = std::max(base, S - 256);
            const double dph = -2.0 * kPi * (newCfo - cfoHz) / symRate;
            cf32 step((float)std::cos(dph), (float)std::sin(dph)), rot(1, 0);
            for (long i = n0; i < produced(); i++) {
                buf[(size_t)(i - base)] *= rot;
                rot *= step;
                if (((i - n0) & 1023) == 1023) rot /= std::abs(rot);
            }
            derotPhase = std::fmod(derotPhase + dph * (double)(produced() - n0), 2.0 * kPi);
            cfoHz = newCfo;
        }
        est = std::make_unique<HeaderEstimator>(hdr);
        estWide = std::make_unique<HeaderEstimator>(hdr);
        {
            const HeaderInfo& hi = headerInfo(hdr);
            if (hi.cyclic()) { wideMaxPre = hi.prefix - 2; widePost = hi.suffix - 2; }
            else { wideMaxPre = 110; widePost = 118; }   // PN595: the fixed header allows about 229 taps in all; echoes before the main path (10 us) count as much as after it
            estWide->setWindow(wideMaxPre, widePost);
            windowPre = std::min(wideMaxPre, hdr == Header::Pn945 ? 40 : 24);
            windowPost = std::min(widePost, hdr == Header::Pn945 ? 160 : 100);
            est->setWindow(windowPre, windowPost);
        }
        sinceWide = 1 << 20;
        haveA = false; haveWide = false;
        state = 1;
        lostRun = 0; lossAvg = 0; frames = 0;
        snrInit = false;
        timeInt = 0; timeJumpRun = 0; takeoverRun = 0;
        front.setSpacing(0);
        resetSi();
        chain.reset();
        char b[160];
        snprintf(b, sizeof b, "%s frames found (%s), carrier offset %.0f Hz", headerInfo(hdr).name, rotates ? "PN phase rotating" : "PN phase fixed", cfoHz);
        log(b);
        dropBefore(S - 2048);
    }

    void loseLock(const char* why) {
        log(std::string("frame lock lost: ") + why);
        state = 0; haveA = false;
        resetSi();
        if (chain) chain->reset();
        chain.reset();
        sinceAttempt = 1 << 30;
        base = produced();
        buf.clear();
        front.setSpacing(0);
    }

    // ------------------------------------------------------------------ one signal frame
    int phaseOf(int f) const { return rotates ? pnPhase(hdr, f % headerInfo(hdr).framesPerSuper) : 0; }

    void estimateAt(long s, int phase, Taps& raw, Taps& clean) {
        Prof::Scope ps(prof, 1);
        est->estimate(at(s), phase, raw);
        clean = raw;
        HeaderEstimator::clean(clean, raw.tapVar, 12.f);
    }

    // The channel's extent from an estimate over the widest window: the narrow window follows it (cheaper, and a better fit)
    void adaptWindow(long s, int phase) {
        Prof::Scope pw(prof, 14);
        estWide->estimate(at(s), phase, rawW);
        cleanW = rawW;
        HeaderEstimator::clean(cleanW, rawW.tapVar, 12.f);
        haveWide = true;
        if (cleanW.peakPower <= kPeakNoiseRatio * rawW.tapVar) return;
        const float lim = std::max(cleanW.peakPower * 1e-4f, 12.f * rawW.tapVar);
        int first = 1 << 20, last = -(1 << 20);
        for (int i = 0; i < (int)cleanW.v.size(); i++) if (std::norm(cleanW.v[(size_t)i]) > lim) { first = std::min(first, i - cleanW.pre); last = std::max(last, i - cleanW.pre); }
        if (first > last) return;
        // (in steps of 8 and 16 taps: a window that has been used before has its matrix already, see HeaderEstimator::setWindow)
        const int pre = std::max(8, std::min(wideMaxPre, (-first + 8 + 7) / 8 * 8));
        const int post = std::max(32, std::min(widePost, (last + 24 + 15) / 16 * 16));
        // follow growth at once, shrinking only when the change is worth a new matrix
        if (pre > windowPre || post > windowPost || windowPre - pre >= 8 || windowPost - post >= 16) {
            windowPre = pre; windowPost = post;
            est->setWindow(windowPre, windowPost);
        }
    }

    bool processFrame() {
        const HeaderInfo& hi = headerInfo(hdr);
        const int Lh = hi.length, Lf = Lh + kBody;
        // (a frame further on than the header after this one: room to look for that header after lost samples, and the one after it)
        if (!ensure(S + Lf + relockSpan())) return false;
        const long S2 = S + Lf;
        const int phA = phaseOf(fnum), phB = phaseOf(fnum + 1);
        if (++sinceWide >= 32) { sinceWide = 0; adaptWindow(S2, phB); haveA = false; }
        if (!haveA) { estimateAt(S, phA, rawA, cleanA); haveA = true; }
        estimateAt(S2, phB, rawB, cleanB);
        const bool goodA = cleanA.peakPower > kPeakNoiseRatio * rawA.tapVar;
        const bool goodB = cleanB.peakPower > kPeakNoiseRatio * rawB.tapVar;
        lossAvg = 0.95 * lossAvg + 0.05 * ((goodA && goodB) ? 0.0 : 1.0);
        frames++;
        if (!goodB) { Prof::Scope pr(prof, 11); if (relock(S2)) { lostRun = 0; return true; } }
        if (!goodB) {
            // no header where it should be: coast on the old timing
            lostRun++;
            if (chain && siOk) chain->skipFrame();
            if (lostRun >= kLostFrames) { loseLock("no PN header"); return true; }
            haveA = false;
            S += Lf; fnum++;
            return true;
        }
        lostRun = 0;
        // ---- samples lost inside this frame, fewer than the header is long: the whole channel moves in this header, earlier by the number lost or,
        // as the cyclic header repeats every period, later by a period less that. The grid below would follow only after three frames, decoding
        // those at the wrong place, and possibly to the alias. The channel of this header against that of the one before: the same shape moved
        // is a gap, and the header is looked for over the frame as for a header that is missing. (A path that grows while another fades, the
        // takeover below, does not keep the shape; neither does an echo that was there before.)
        if (goodA && std::abs(cleanB.peakIndex - cleanA.peakIndex) >= 4) {
            double off = 0, off0 = 0;
            const int sh = cleanB.peakIndex - cleanA.peakIndex;
            if (shapeOffset(cleanA, cleanB, sh, off) > 0.8 && std::fabs(off) < 1.0 && shapeOffset(cleanA, cleanB, 0, off0, 0) < 0.5) { Prof::Scope pr(prof, 11); if (relock(S2)) return true; }
        }
        // ---- timing: the reference path of the frame grid. Normally the strongest tap. With two paths of similar strength (a single-frequency network,
        // a strong echo) the strongest one changes from frame to frame as the sampling phase drifts and splits a path over two taps, and a grid that
        // follows it jumps back and forth; so the grid sticks to the path it has (the strongest tap within 3 symbols of it, as long as that is within
        // 3 dB of the strongest of all) and only a path that has been clearly stronger for a while takes over.
        {
            const int Lt = (int)cleanB.v.size();
            auto frac = [&](int i) {   // fractional position of the tap at index i by a parabola through its amplitude and its neighbours
                if (i <= 0 || i + 1 >= Lt) return 0.0;
                const double a = std::sqrt(std::norm(cleanB.v[(size_t)i - 1])), b = std::sqrt(std::norm(cleanB.v[(size_t)i])), c = std::sqrt(std::norm(cleanB.v[(size_t)i + 1]));
                const double den = a - 2 * b + c;
                return std::fabs(den) > 1e-12 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0.0;
            };
            int ref = cleanB.peakIndex + cleanB.pre;   // index of the tap that defines the grid
            if (frames > 3) {
                int lp = -1; float lpw = 0;
                for (int i = std::max(0, cleanB.pre - 3); i <= std::min(Lt - 1, cleanB.pre + 3); i++) { const float pw = std::norm(cleanB.v[(size_t)i]); if (pw > lpw) { lpw = pw; lp = i; } }
                if (lp >= 0 && lpw * 2.f >= cleanB.peakPower) { ref = lp; takeoverRun = 0; }
                else if (lp >= 0 && lpw > 12.f * rawB.tapVar && ++takeoverRun < 16) ref = lp;
                else takeoverRun = 0;   // the reference path is gone, or another has been stronger for 16 frames: the grid moves to the strongest
            }
            const double e = (double)(ref - cleanB.pre) + (ref == cleanB.peakIndex + cleanB.pre ? (double)cleanB.peakFrac : frac(ref));
            int j = 0;
            if (std::fabs(e) >= 1.5) {
                // a jump of the reference path or a drift: re-centre the frame grid only after it has lasted a few frames
                timeJumpRun++;
                if (timeJumpRun >= 3 || std::fabs(e) < 4.0) j = (int)std::lround(e);
            } else timeJumpRun = 0;
            // a jump that is not taken yet must not reach the loops: a few frames of a 16 symbol error would pull the clock 50 ppm off
            const double ef = (std::fabs(e) >= 1.5 && j == 0) ? 0.0 : e - j;
            front.shift(0.20 * ef);
            const double sp = front.spacing() + 0.015 * ef / Lf;
            front.setSpacing(std::max(-3e-4, std::min(3e-4, sp)));
            timeInt = j;
            pendingJump = j;
            peakOffUs = (float)(e / (symRate / 1e6));
        }
        // ---- the body. The header says which carrier mode can be there: PN420 and PN945 go with multi-carrier, PN595 with either (single carrier is
        // defined with PN595 only); until the system information tells, both are tried
        int8_t chipsA[945], chipsB[945];
        pnHeader(hdr, phA, chipsA); pnHeader(hdr, phB, chipsB);
        const int8_t* ca = chipsA; const int8_t* cb = chipsB;
        // a header that is not usable stands in for its neighbour
        const Taps& ha = goodA ? cleanA : cleanB;
        const double hAmp = std::sqrt(hi.powerRatio / 2.0);
        const bool tryOfdm = hdr != Header::Pn595 || !siOk || carriers == 3780;
        const bool trySingle = hdr == Header::Pn595 && (!siOk || carriers == 1);
        if (tryOfdm) {
            Prof::Scope pe(prof, 2);
            eq.run(at(S + Lh), ha, ca, cleanB, cb, Lh, hAmp, bins.data(), var.data());
            Body& b = body[0];
            const int16_t* sb = siBins(); const int16_t* db = dataBins();
            for (int i = 0; i < kSiSymbols; i++) { b.si[(size_t)i] = bins[(size_t)sb[i]]; b.sivar[(size_t)i] = var[(size_t)sb[i]]; }
            for (int i = 0; i < kDataSymbols; i++) { b.data[(size_t)i] = bins[(size_t)db[i]]; b.dvar[(size_t)i] = var[(size_t)db[i]]; }
            normalise(b);
            if (siOk && carriers == 3780) commonPhase(b);
        }
        if (trySingle) {
            Prof::Scope pe(prof, 2);
            eq1.run(at(S + Lh), ha, ca, cleanB, cb, Lh, hAmp, sym1.data(), var1.data());
            Body& b = body[1];
            for (int i = 0; i < kSiSymbols; i++) { b.si[(size_t)i] = sym1[(size_t)i]; b.sivar[(size_t)i] = var1[(size_t)i]; }
            for (int i = 0; i < kDataSymbols; i++) { b.data[(size_t)i] = sym1[(size_t)(kSiSymbols + i)]; b.dvar[(size_t)i] = var1[(size_t)(kSiSymbols + i)]; }
            normalise(b);
        }
        Body& act = body[siOk && carriers == 1 ? 1 : 0];   // the one that carries the signal (before the system information is known: the multi-carrier one, for the display)
        // The noise estimate comes from the headers; whatever hits the body alone (an impulse, a level step, interference that comes and goes) is not in it.
        // The decision error of the frame is: when it is clearly larger than the estimate, the whole frame gets the larger variance, so that the decoder
        // treats it as the poor frame it is instead of trusting garbage (the interleaver spreads one such frame over about 170 frames of codewords).
        if (siOk) {
            frameErr = decisionError(profile.map, act.data.data(), kDataSymbols, 3);
            double vm = 0;
            for (int i = 0; i < kDataSymbols; i++) vm += act.dvar[(size_t)i];
            vm /= kDataSymbols;
            const double ratio = frameErr / std::max(vm, 1e-9);
            if (ratio > 1.5) {
                const float sc = (float)ratio;
                for (int i = 0; i < kDataSymbols; i++) act.dvar[(size_t)i] *= sc;
            }
        }
        // ---- statistics: C/N from the header fit (the channel energy against the noise left by the fit), MER from decisions below
        {
            const double noise = 0.5 * (rawA.noise + rawB.noise);
            const double chanEnergy = 0.5 * (cleanA.energy + cleanB.energy);   // the body has unit power, a single tap of unit gain is a clean channel
            const double cn = noise > 0 ? 10.0 * std::log10(std::max(1e-6, chanEnergy / noise)) : 99.0;
            if (!snrInit) { snrPn = cn; snrInit = true; } else snrPn += 0.2 * (cn - snrPn);
        }
        { Prof::Scope pi(prof, 3); updateSi(tryOfdm, trySingle); }
        // a lock that never produces system information: too weak to use, or the header fit is wrong (a bad carrier offset); look again
        if (!siOk && frames >= 200 && snrPn < -15.0) { loseLock("signal too weak"); return true; }
        // (a lock with a good header fit stays, whatever the body is: a signal this receiver does not know shows its constellation and C/N)
        if (!siOk && snrPn < 6.0 && frames > (uint64_t)(1.5 * symRate / (double)Lf)) { loseLock("no system information"); return true; }
        // A clean header fit but no system information after 0.4 s: I and Q may be swapped (the PN header survives a mirrored spectrum, the
        // body does not). Try the other orientation, a few times at most.
        if (!siOk && snrPn >= 6.0 && flips < 4 && frames > (uint64_t)(0.4 * symRate / (double)Lf)) { flipReq = true; return false; }
        if (siOk) {
            merAvg += 0.3 * (10.0 * std::log10(1.0 / std::max(frameErr, 1e-9)) - merAvg);
            if (!chain) makeChain();
            // a frame whose grid is several symbols off and waiting to move (the body taken at the wrong place) is erased, not decoded as garbage
            if (timeJumpRun > 0 && pendingJump == 0) chain->skipFrame();
            else { Prof::Scope pp(prof, 4); chain->pushFrame(act.data.data(), act.dvar.data()); }
        }
        { Prof::Scope pd(prof, 7); updateCells(); updateCir(); }
        // ---- carrier offset: the phase of the channel from one header to the next (only when the two estimates agree in shape: a header that an
        // impulse or a level step hit gives a random phase)
        {
            std::complex<double> z = 0;
            double ea = 0, eb = 0;
            for (size_t i = 0; i < cleanA.v.size() && cleanA.pre == cleanB.pre && i < cleanB.v.size(); i++) {
                z += std::complex<double>(cleanB.v[i]) * std::conj(std::complex<double>(cleanA.v[i]));
                ea += std::norm(cleanA.v[i]); eb += std::norm(cleanB.v[i]);
            }
            if (std::abs(z) > 0 && cleanA.pre == cleanB.pre && std::abs(z) > 0.7 * std::sqrt(ea * eb)) {
                const double df = std::arg(z) * symRate / (2.0 * kPi * Lf);
                cfoHz += 0.25 * df;   // the stream is derotated by cfoHz: a channel that still turns forward needs a larger correction
                cfoHz = std::max(-55000.0, std::min(55000.0, cfoHz));
            }
        }
        // ---- advance
        S += Lf + pendingJump; fnum++;
        if (pendingJump != 0) haveA = false;
        else { cleanA = cleanB; rawA = rawB; }
        pendingJump = 0;
        dropBefore(S - 2048);
        return true;
    }
    int pendingJump = 0;
    int takeoverRun = 0;
    uint64_t relocks = 0;

    // Samples lost on the way in (a radio whose buffers overflowed): the next header sits earlier than the grid says, by the number of samples
    // lost modulo a frame, and the frames that went with the gap move the phase of a rotating header on. The header is looked for over the
    // frame before the expected one, under every super-frame number up to kMaxLost frames on; the frame holding the gap and the lost ones are
    // erased in the decoder, which stays in step.
    static constexpr int kMaxLost = 200;   // 125 ms of PN945 frames, 111 ms of PN420
    std::vector<float> corrRe, corrIm;
    // After lost samples the new grid sits a fraction of a symbol off the old sampling instants (the gap is a whole number of input samples, not
    // of symbols); the timing loop would take tens of frames to pull it in. The symbols from shortly before the new header on are taken again
    // at instants moved by that fraction, from the input the front end still holds.
    void regrid(long newS, double frac) {
        if (std::fabs(frac) < 0.02) return;
        const long t0 = std::max(base, newS - 512);
        const long k = produced() - t0;
        if ((double)k * front.ratio() + 64 > (double)SrrcResampler::kHistory) return;
        buf.resize((size_t)(t0 - base));
        front.shift(-(double)k + frac);
        const double dph = -2.0 * kPi * cfoHz / symRate;
        derotPhase = std::fmod(derotPhase - dph * (double)k, 2.0 * kPi);
    }

    // Where the channel of b sits against that of a moved by `shift` symbols (lags, whatever the windows of the two): the best match (1 = the same shape) of the tap powers (summed over
    // three taps) over offsets of up to R (at most four) symbols, and that offset to a fraction of a symbol
    static double shapeOffset(const Taps& a, const Taps& b, int shift, double& off, int R = 4) {
        auto smooth = [](const Taps& t) {
            const int L = (int)t.v.size();
            std::vector<double> p((size_t)L, 0.0);
            for (int k = 0; k < L; k++) for (int d = -1; d <= 1; d++) if (k + d >= 0 && k + d < L) p[(size_t)k] += std::norm(t.v[(size_t)(k + d)]);
            return p;
        };
        const std::vector<double> pa = smooth(a), pb = smooth(b);
        double ea = 0, eb = 0;
        for (double x : pa) ea += x * x;
        for (double y : pb) eb += y * y;
        double c[9];
        R = std::max(0, std::min(4, R));
        for (int d = -R; d <= R; d++) {
            double num = 0;
            for (int k = 0; k < (int)pa.size(); k++) {
                const int k2 = k - a.pre + b.pre + shift + d;
                if (k2 >= 0 && k2 < (int)pb.size()) num += pa[(size_t)k] * pb[(size_t)k2];
            }
            c[d + R] = num / std::sqrt(std::max(1e-30, ea * eb));
        }
        int bi = 0;
        for (int i = 1; i <= 2 * R; i++) if (c[i] > c[bi]) bi = i;
        off = bi - R;
        if (bi > 0 && bi < 2 * R) {
            const double den = c[bi - 1] - 2 * c[bi] + c[bi + 1];
            if (std::fabs(den) > 1e-12) off += std::max(-0.5, std::min(0.5, 0.5 * (c[bi - 1] - c[bi + 1]) / den));
        }
        return c[bi];
    }
    long relockSpan() const { const int Lh = headerInfo(hdr).length; return 8 + Lh + kBody + 2 * Lh + 512 + std::max(est->post(), estWide->post()); }
    bool relock(long S2) {
        const HeaderInfo& hi = headerInfo(hdr);
        const int Lh = hi.length, Lf = Lh + kBody;
        // (Not reaching back into the header of this frame: it repeats itself as the new one does, and the header of frame f + 2 m is that of f
        // moved m chips, it would be taken for lost frames.)
        // Once a frame has been coasted, the header of this frame was not found either: the new one may sit where it was (lost samples that
        // were most of a frame), so the search starts a little before it.
        const long lo = lostRun > 0 ? S2 - Lf - 16 : S2 - Lf + Lh, hiPos = S2 + 8;
        if (lo - 512 < base) return false;
        if (!ensure(S2 + relockSpan())) return false;
        // correlation of the received samples with the core of each candidate header, at every position
        // over the whole header: a cyclic header is one periodic sequence, another phase is the same sequence shifted, and only its ends (where the
        // body starts) tell the two apart
        const int nPos = (int)(hiPos - lo + 1), clen = Lh, coff = 0;
        corrRe.resize((size_t)nPos + (size_t)clen); corrIm.resize(corrRe.size());
        for (int i = 0; i < nPos + clen; i++) { const cf32 v = *at(lo + coff + i); corrRe[(size_t)i] = v.real(); corrIm[(size_t)i] = v.imag(); }
        double pw = 0;
        for (int i = 0; i < nPos + clen; i++) pw += (double)corrRe[(size_t)i] * corrRe[(size_t)i] + (double)corrIm[(size_t)i] * corrIm[(size_t)i];
        pw /= (nPos + clen);
        // a cyclic header repeats itself 511 (PN945) or 255 (PN420) symbols on, whatever its phase: a sliding correlation of the samples with
        // themselves one period on finds where it starts, and the phases are then tried only around there (a search over all positions and phases
        // costs several frames of processing)
        int qLo = 0, qHi = nPos - 1;
        if (hi.cyclic()) {
            const int P = hi.core, W = Lh - P;
            auto cw = [&](int i) { return std::complex<double>(corrRe[(size_t)i], corrIm[(size_t)i]); };
            std::complex<double> acc = 0;
            for (int t = 0; t < W; t++) acc += cw(t) * std::conj(cw(t + P));
            double bestC = -1; int q0 = 0;
            for (int q = 0; q < nPos; q++) {
                const double c = std::norm(acc);
                if (c > bestC) { bestC = c; q0 = q; }
                if (q + W + P < nPos + clen) acc += cw(q + W) * std::conj(cw(q + W + P)) - cw(q) * std::conj(cw(q + P));
            }
            qLo = std::max(0, q0 - 48); qHi = std::min(nPos - 1, q0 + 48);
        }
        const int nm = rotates ? kMaxLost : 1;
        int bestM = -1; double best = 0;
        std::vector<double> mBest((size_t)nm, 0.0); std::vector<long> mPos((size_t)nm, 0);
        std::vector<float> ch((size_t)clen);
        int8_t chips[945];
        for (int m = 0; m < nm; m++) {
            pnHeader(hdr, phaseOf(fnum + 1 + m), chips);
            bool dup = false;   // a phase met already at a smaller m (the rotation turns back half way through the super-frame)
            for (int k = 0; k < m && !dup; k++) dup = phaseOf(fnum + 1 + k) == phaseOf(fnum + 1 + m);
            if (dup) continue;
            for (int t = 0; t < clen; t++) ch[(size_t)t] = (float)chips[coff + t];
            for (int q = qLo; q <= qHi; q++) {
                float sr = 0, si = 0;
                const float* re = &corrRe[(size_t)q]; const float* im = &corrIm[(size_t)q];
                for (int t = 0; t < clen; t++) { sr += re[t] * ch[(size_t)t]; si += im[t] * ch[(size_t)t]; }
                const double c = (double)sr * sr + (double)si * si;
                if (c > mBest[(size_t)m]) { mBest[(size_t)m] = c; mPos[(size_t)m] = lo + q; }
            }
            best = std::max(best, mBest[(size_t)m]);
        }
        // The phase of frame f + 2 is that of frame f moved by one chip, so one header fits m and m + 2 (one symbol apart) about equally. The
        // header after it tells them apart: the odd frames' phases count up and the even ones' down, so under m + 2 the channel in the next header
        // shows up two symbols from where it is in this one, under the other parity a good many. The offset is measured on the shape of the
        // channel (the power of its taps), not on its strongest tap, which two echoes of about the same strength take in turns. The candidate
        // with the channel in the same place in both headers wins; between those about as good, the fewest lost frames.
        if (best <= 30.0 * clen * pw) return false;   // a header stands far above the correlation of noise (clen * power per lag)
        Taps raw, clean, raw2, clean2;
        long newS = 0;
        double newFrac = 0, bestOff = 1e9;
        for (int m = 0; m < nm; m++) {
            if (mBest[(size_t)m] < 0.5 * best) continue;
            est->estimate(at(mPos[(size_t)m]), phaseOf(fnum + 1 + m), raw);
            clean = raw; HeaderEstimator::clean(clean, raw.tapVar, 12.f);
            if (clean.peakPower <= kPeakNoiseRatio * raw.tapVar) continue;
            const long s1 = mPos[(size_t)m] + clean.peakIndex;
            est->estimate(at(s1 + Lf), phaseOf(fnum + 2 + m), raw2);
            clean2 = raw2; HeaderEstimator::clean(clean2, raw2.tapVar, 12.f);
            if (clean2.peakPower <= kPeakNoiseRatio * raw2.tapVar) continue;
            // the tap at lag l of the first estimate is the one at lag l - peakIndex of the second
            double off = 0;
            if (shapeOffset(clean, clean2, -clean.peakIndex, off) < 0.6) continue;
            if (std::fabs(off) > 0.7) continue;
            if (std::fabs(off) < bestOff - 0.3) { bestOff = std::fabs(off); bestM = m; newS = s1; newFrac = clean.peakFrac; }
        }
        if (bestM < 0) return false;
        if (newS == S2 && bestM == 0) return false;   // the header is where it should be, only weak: not a gap
        // erase the frame that held the gap and the frames lost with it
        if (chain && siOk) for (int k = 0; k <= bestM; k++) chain->skipFrame();
        char b[160];
        snprintf(b, sizeof b, "samples lost: frame grid moved by %ld symbols, %d frame(s) erased", newS - S2 - (long)bestM * Lf, bestM + 1);
        log(b);
        relocks++;
        S = newS; fnum += 1 + bestM;
        haveA = false; timeJumpRun = 0; takeoverRun = 0;
        sinceWide = 1 << 20;   // the window may have grown to the channel seen across the gap: fit it again on the next frame
        regrid(S, newFrac);
        dropBefore(S - 2048);
        return true;
    }

    // The phase common to all carriers of the body, from the 36 system information symbols (their word is known once the system information is
    // decoded), taken out of data and system information: the carrier phase moves inside a frame (phase noise) and the headers only give its ends.
    void commonPhase(Body& b) {
        static const bool off = std::getenv("DTMB_NOCPE") != nullptr;
        if (off) return;
        uint8_t chips[kSiSymbols];
        siChips(siIndex, chips);
        const float r2 = 1.f / std::sqrt(2.f);
        cf32 acc(0, 0);
        double wsum = 0, psum = 0;
        for (int i = 0; i < kSiSymbols; i++) {
            const float w = 1.f / b.sivar[(size_t)i];
            const float sgn = chips[i] ? r2 : -r2;
            acc += w * b.si[(size_t)i] * cf32(sgn, -sgn);   // times the conjugate of the reference sgn (1 + j)
            wsum += w; psum += w * std::abs(b.si[(size_t)i]);
        }
        // reliable only when the symbols agree with the word (coherence) and are not wild
        if (wsum <= 0 || std::abs(acc) < 0.6 * psum) return;
        const float phi = std::arg(acc);
        if (std::fabs(phi) < 0.004f) return;
        const cf32 rot = std::polar(1.f, -phi);
        for (auto& v : b.data) v *= rot;
        for (auto& v : b.si) v *= rot;
    }

    // ------------------------------------------------------------------ system information
    // The gain of the body against the header, taken out of the symbols and their variances; the symbols have unit power afterwards
    void normalise(Body& b) {
        Prof::Scope pn(prof, 15);
        double pData = 0, nvData = 0;
        for (int i = 0; i < kDataSymbols; i++) { pData += std::norm(b.data[(size_t)i]); nvData += std::min(b.dvar[(size_t)i], 4.f); }
        pData /= kDataSymbols; nvData /= kDataSymbols;
        const double g = b.gain.update(std::sqrt(std::max(0.05, pData - nvData)));
        const float gi = (float)(1.0 / g), gi2 = gi * gi;
        for (int i = 0; i < kDataSymbols; i++) { b.data[(size_t)i] *= gi; b.dvar[(size_t)i] = std::max(1e-6f, b.dvar[(size_t)i] * gi2); }
        for (int i = 0; i < kSiSymbols; i++) { b.si[(size_t)i] *= gi; b.sivar[(size_t)i] = std::max(1e-6f, b.sivar[(size_t)i] * gi2); }
    }

    // ------------------------------------------------------------------ system information
    void updateSi(bool tryOfdm, bool trySingle) {
        const int idx0 = tryOfdm ? body[0].tracker.update(body[0].si.data(), body[0].sivar.data()) : -1;
        const int idx1 = trySingle ? body[1].tracker.update(body[1].si.data(), body[1].sivar.data()) : -1;
        // when both hypotheses produce a word the better fitting one wins
        int k = -1;
        if (idx0 > 0) k = 0;
        if (idx1 > 0 && (k < 0 || body[1].tracker.score > body[0].tracker.score)) k = 1;
        if (k >= 0) {
            const int idx = k == 0 ? idx0 : idx1, mode = k == 0 ? 3780 : 1;
            Profile p;
            // a decoder that runs is not thrown away for a word that won a few frames hit by a gap or an impulse
            const bool change = !siOk || ((siIndex != idx || carriers != mode) && body[k].tracker.run >= 40);
            if (change && profileFromSi(idx, p)) {
                siOk = true; siIndex = idx; profile = p; carriers = mode; siLost = 0;
                chain.reset();
                char b[200];
                snprintf(b, sizeof b, "system information: %s, rate %s, interleaver mode %d (index %d), %s", mappingName(p.map), rateName(p.rate), p.mode2 ? 2 : 1, siIndex, mode == 1 ? "single carrier" : "multi-carrier");
                log(b);
            }
        }
        // a signal that has changed under the lock (another mode, or the system information is gone): look again
        if (siOk) {
            if (body[carriers == 1 ? 1 : 0].tracker.score < 0.25f) siLost++; else siLost = 0;
            if (siLost >= 400) {
                log("system information lost");
                siOk = false; siLost = 0; carriers = 3780;
                for (Body& b : body) b.tracker.reset();
                chain.reset();
            }
        }
    }

    void makeChain() {
        chain = std::make_unique<FecChain>(profile, hdr, decThreads.load(), symRate);
        chain->setComputeMode(computeMode.load());
        lastStats = ChainStats();
    }

    // ------------------------------------------------------------------ displays
    void updateCells() {
        cells.clear();
        const Body& b = body[siOk && carriers == 1 ? 1 : 0];
        for (int i = 0; i < kDataSymbols; i += 2) cells.push_back(b.data[(size_t)i]);
    }

    // The impulse response of the display: the widest estimate the header allows (every 32 frames), else the tracking window
    void updateCir() {
        const bool wide = haveWide && cleanW.peakPower > kPeakNoiseRatio * rawW.tapVar;
        const Taps& t = wide ? cleanW : cleanB;
        const int L = (int)t.v.size();
        const int n = std::min(L, 512);
        const int first = -t.pre;   // lag of the first value shown
        cir.assign((size_t)n, -80.f);
        const float pk = std::max(t.peakPower, 1e-20f);
        for (int i = 0; i < n; i++) {
            const float p = std::norm(t.v[(size_t)i]);
            cir[(size_t)i] = p > 0 ? std::max(-80.f, 10.f * std::log10(p / pk)) : -80.f;
        }
        cirFirst = first;
        // span of the paths within 30 dB of the strongest
        int lo = L, hi = -1;
        for (int i = 0; i < L; i++) if (std::norm(t.v[(size_t)i]) > pk * 1e-3f) { lo = std::min(lo, i); hi = std::max(hi, i); }
        echoSpanUs = hi >= lo ? (float)(hi - lo) / (float)(symRate / 1e6) : 0.f;
    }

    // ------------------------------------------------------------------ the loop
    void run() {
        for (;;) {
            if (state == 0) { { Prof::Scope ps(prof, 12); search(); } if (state == 0) return; }
            else {
                { Prof::Scope pw(prof, 13); if (!processFrame()) { report(); return; } }
                pollChain();
                report();
            }
        }
    }

    void pollChain() {
        if (!chain) return;
        chain->setComputeMode(computeMode.load());
        chain->poll([this](const uint8_t* p, size_t n, double secs) {
            pktTotal += n;
            lastGoodAt = produced();
            if (pktCb) pktCb(p, n, secs);
        });
    }

    bool report() {
        if (produced() < reportAt) return false;
        reportAt = produced() + (long)(symRate * 0.25);
        DtmbTelemetry t;
        t.state = state == 0 ? 0 : ((chain && produced() - lastGoodAt < (long)(symRate * 0.7)) ? 2 : 1);
        t.cfoHz = cfoHz + mixer.hz;
        t.header = state == 0 ? -1 : (int)hdr;
        t.phaseRotates = rotates;
        t.siOk = siOk; t.siIndex = siOk ? siIndex : 0;
        t.siScore = siOk ? body[carriers == 1 ? 1 : 0].tracker.score : std::max(body[0].tracker.score, body[1].tracker.score);
        t.carriers = state == 0 ? 0 : siOk ? carriers : (hdr == Header::Pn595 ? 0 : 3780);
        if (siOk) { t.mapping = (int)profile.map; t.rate = (int)profile.rate; t.interleaver = profile.mode2 ? 2 : 1; t.netMbps = (float)(netBitrate(hdr, profile, symRate) / 1e6); }
        t.snrPnDb = state == 0 ? 0.f : (float)snrPn;
        t.snrDb = siOk && merAvg > 8.0 && merAvg < snrPn ? (float)merAvg : t.snrPnDb;
        t.merDb = siOk ? (float)merAvg : 0.f;
        t.clockPpm = front.spacing() * 1e6;
        t.echoSpanUs = echoSpanUs; t.peakOffsetUs = peakOffUs;
        t.levelDbfs = levelDb;
        t.frames = frames;
        t.frameLossPct = (float)(100.0 * lossAvg);
        if (state != 0) { t.cells = cells; t.cirDb = cir; t.cirFirst = cirFirst; }
        if (chain) {
            const ChainStats st = chain->stats();
            t.blocksOk = st.cwOk; t.blocksBad = st.cwBad;
            t.ldpcIter = (float)st.lastIterAvg;
            t.packets = st.packets;
            t.onGpu = st.onGpu; t.gpuWords = st.gpuWords;
            t.cwDropped = st.cwDropped; t.cwSkipped = st.cwSkipped; t.bchCorrected = st.bchCorrected;
            t.tsLock = produced() - lastGoodAt < (long)(symRate * 0.7);
            t.dataValid = t.tsLock;
            statsKeep = st;
        } else {
            t.blocksOk = statsKeep.cwOk; t.blocksBad = statsKeep.cwBad; t.packets = statsKeep.packets;
        }
        t.rateOk = rateOk;
        t.computeMode = computeMode.load(); t.gpuAvailable = FecChain::gpuUsable();
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        tel = std::move(t);
        return true;
    }
    ChainStats statsKeep;
};

// ---------------------------------------------------------------- public interface
DtmbReceiver::DtmbReceiver() : p_(std::make_unique<Impl>()) {
    p_->bins.resize((size_t)kBody); p_->var.resize((size_t)kBody);
    p_->sym1.resize((size_t)kBody); p_->var1.resize((size_t)kBody);
    for (auto& b : p_->body) { b.data.resize((size_t)kDataSymbols); b.dvar.resize((size_t)kDataSymbols); b.si.resize(kSiSymbols); b.sivar.resize(kSiSymbols); }
}
DtmbReceiver::~DtmbReceiver() {
    Impl& s = *p_;
    if (!s.prof.on) return;
    const double real = s.inRate > 0 ? s.inSamples / s.inRate : 0;
    fprintf(stderr, "DTMB_PROFILE: %.2f s of signal, feed() %.2f s (%.0f%% of real time)\n", real, s.prof.sec[6], real > 0 ? 100 * s.prof.sec[6] / real : 0);
    for (int i : {0, 8, 1, 14, 2, 15, 3, 4, 5, 7, 9, 10, 11, 12, 13}) fprintf(stderr, "  %-38s %7.3f s  %5.1f%% of real time\n", Prof::kName[i], s.prof.sec[i], real > 0 ? 100 * s.prof.sec[i] / real : 0);
    if (s.chain) {
        const ChainStats st = s.chain->stats();
        fprintf(stderr, "  FEC workers (LDPC %.3f s, BCH+descramble+TS %.3f s, summed over threads) %5.1f%% of real time; %.2f iterations/codeword, %llu codewords, slowest good word %llu, %llu iterations on failed words\n", st.ldpcSec, st.tsSec,
                real > 0 ? 100 * (st.ldpcSec + st.tsSec) / real : 0, st.cwOk + st.cwBad ? (double)st.iterSum / (double)(st.cwOk + st.cwBad) : 0.0, (unsigned long long)(st.cwOk + st.cwBad), (unsigned long long)st.okIterMax, (unsigned long long)st.badIterSum);
    }
}

void DtmbReceiver::configure(double inputRateHz, double bwMhz) {
    Impl& s = *p_;
    s.inRate = inputRateHz;
    s.symRate = symbolRateFor(bwMhz);
    s.acq.symRate = s.symRate;
    s.rateOk = s.front.configure(inputRateHz, s.symRate);
    s.mixer.set(0, inputRateHz); s.mirror = false; s.flips = 0; s.flipReq = false;
    s.locator.configure(inputRateHz, s.symRate);
    s.locator.setEdgeCentre(true);
    s.doReset();
}

bool DtmbReceiver::ready() const { return p_->rateOk; }

void DtmbReceiver::reset() { p_->resetReq = true; }

void DtmbReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    if (!s.rateOk || !n) return;
    if (s.resetReq.exchange(false)) s.doReset();
    // input level
    {
        Prof::Scope pl(s.prof, 10);
        double a = 0;
        for (size_t i = 0; i < n; i++) a += std::norm(x[i]);
        if (!std::isfinite(a)) {
            // NaN or infinite samples (a broken file or driver) would stay in the filters and the loops for good: replace them by silence
            s.scrub.assign(x, x + n);
            a = 0;
            for (auto& v : s.scrub) { if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0); a += std::norm(v); }
            x = s.scrub.data();
        }
        // a channel recorded beside the centre: until the system information is found, the spectrum says where it is
        if (!s.siOk) {
            double hz; bool found;
            if (s.locator.feed(x, n, hz, found) && found && std::fabs(hz - s.mixer.hz) > 100e3) {   // (the search covers +-54 kHz itself; a spectrum shaped by echoes moves the estimate by tens of kHz)
                s.log("channel found " + std::to_string((int)std::lround(hz / 1e3)) + " kHz from the centre");
                s.mixer.set(hz, s.inRate); s.doReset();
            }
        }
        if (s.mixer.hz != 0 || s.mirror) {
            if (x != s.scrub.data()) s.scrub.assign(x, x + n);
            s.mixer.apply(s.scrub.data(), n);
            if (s.mirror) for (auto& v : s.scrub) v = std::conj(v);
            x = s.scrub.data();
        }
        s.levelAcc += a; s.levelN += n;
        if (s.levelN >= (uint64_t)(s.inRate * 0.25)) { s.levelDb = (float)(10.0 * std::log10(std::max(1e-12, s.levelAcc / (double)s.levelN))); s.levelAcc = 0; s.levelN = 0; }
    }
    Prof::Scope pf(s.prof, 6);
    s.inSamples += (double)n;
    { Prof::Scope pg(s.prof, 0); s.front.push(x, n); }
    s.run();
    if (s.flipReq) { s.flipReq = false; s.mirror = !s.mirror; s.flips++; s.log(s.mirror ? "trying I and Q swapped" : "trying I and Q as they come"); s.doReset(); }
    { Prof::Scope pc(s.prof, 5); s.pollChain(); }
    // The system information survives a mirrored spectrum (its words are the same conjugated), the data does not: decoded but nothing comes
    // out of the LDPC after a good many codewords, try the other orientation
    if (s.chain && s.siOk) {
        const ChainStats st = s.chain->stats();
        if (st.cwOk > 0) s.flips = 0;
        else if (st.cwBad + st.cwSkipped >= (s.relocks ? 2000u : 60u) && s.flips < 4 && s.snrPn >= 10.0) s.flipReq = true;
    }
    if (s.flipReq) { s.flipReq = false; s.mirror = !s.mirror; s.flips++; s.log(s.mirror ? "trying I and Q swapped" : "trying I and Q as they come"); s.doReset(); }
}

bool DtmbReceiver::telemetry(DtmbTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void DtmbReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void DtmbReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->pktCb = std::move(cb); }
void DtmbReceiver::setComputeMode(int mode) {
    p_->computeMode = std::max(0, std::min(2, mode));   // the receiver thread hands it to the running chain at its next poll
}
void DtmbReceiver::setDecoderThreads(int n) { p_->decThreads = std::max(0, std::min(16, n)); }

void DtmbReceiver::flush() {
    Impl& s = *p_;
    if (!s.chain) return;
    s.chain->flush([&s](const uint8_t* p, size_t n, double secs) {
        s.pktTotal += n;
        s.lastGoodAt = s.produced();
        if (s.pktCb) s.pktCb(p, n, secs);
    });
}

ModeTuning dtmbTuning() {
    ModeTuning t;
    t.stdMode = 9; t.id = "dtmb"; t.name = "DTMB";
    t.minMhz = 170; t.maxMhz = 870; t.defMhz = 530.0;
    t.sampleRate = 10000000.0; t.basebandHz = 8000000.0; t.bandwidthMhz = 8;
    t.minSampleRate = 8000000.0;
    // the frame header carries the same PN bit on I and Q: with the carrier within a few hertz of the centre that is a fixed line in the IQ plane,
    // 20 % of the time, which the blind IQ estimate reads as a 6 degree phase error (the MER fell from 31 to 19 dB with the correction on)
    t.notCircular = true;
    return t;
}

} // namespace dect2
