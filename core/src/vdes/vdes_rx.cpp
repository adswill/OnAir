// VDES receiver: see vdes_rx.h. ASM goes through the AIS receiver (ais_rx.cpp), VDE-TER through a power detector per 25 kHz sub-channel and a
// symbol-statistics classifier.
#include "dect2/vdes/vdes_rx.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/results/results_io.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/net/feeds.h"
#include "dect2/vdes/vdes_gen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <deque>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kWork = 230400.0;          // 3 samples per symbol at 100 kHz, 12 at 25 kHz
constexpr int kNf = 256, kHop = 128;
constexpr double kDf = kWork / kNf;         // 900 Hz
constexpr size_t kRing = (size_t)1 << 17;
constexpr int kWarmFrames = 300;
constexpr double kOnRatio = 3.2, kOffRatio = 2.0;   // 5 dB / 3 dB above the floor
constexpr int kEndFrames = 4;               // quiet frames that end a burst
constexpr int kMinFrames = 12;              // shortest burst kept
constexpr int kMaxFrames = 900;             // longest: 0.5 s
constexpr double kReportSec = 0.25;

struct Band {
    bool inView = false;
    double fIn = 0;
    std::complex<double> rot{1, 0}, step{1, 0};
    ExactResampler rs;
    std::vector<cf32> mix, wk;               // scratch
    std::vector<cf32> pend; size_t pendHead = 0;
    std::vector<cf32> ring = std::vector<cf32>(kRing);
    uint64_t total = 0;                      // work samples so far
    uint64_t frame = 0;                      // FFT frames so far
    // per sub-channel
    double warm[4] = {}, nf[4] = {}; bool on[4] = {};
    int warmN = 0; bool nfKnown = false;
    double level = 0;
    // burst
    bool burst = false; uint64_t b0 = 0; int quiet = 0; int frames = 0, nAcc = 0;   // frames: from the start to the last active one; nAcc: the active ones
    double sumP[4] = {}, sumN[4] = {}; int act[4] = {};
    std::vector<double> bacc = std::vector<double>(kNf, 0.0);
    void reset() {
        rot = {1, 0};
        if (!rs.passthrough()) rs.reset();
        pend.clear(); pendHead = 0; total = 0; frame = 0;
        for (int s = 0; s < 4; s++) { warm[s] = nf[s] = 0; on[s] = false; }
        warmN = 0; nfKnown = false; level = 0;
        burst = false; quiet = 0; frames = 0;
    }
};

int subOfBin(int k) {
    const double f = (k < kNf / 2 ? k : k - kNf) * kDf;
    if (f < -50000.0 || f >= 50000.0) return -1;
    return (int)std::floor((f + 50000.0) / 25000.0);
}

std::vector<float> rrcTaps(double sps, double beta) {
    const int half = (int)std::lround(6 * sps);
    std::vector<float> h((size_t)(2 * half + 1));
    double e = 0;
    for (int i = -half; i <= half; i++) {
        const double t = i / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1.0 - beta + 4.0 * beta / kPi;
        else if (std::fabs(std::fabs(t) - 1.0 / (4.0 * beta)) < 1e-6) v = beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
        else v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) / (kPi * t * (1 - 16 * beta * beta * t * t));
        h[(size_t)(i + half)] = (float)v;
        e += v * v;
    }
    for (auto& v : h) v = (float)(v / std::sqrt(e));
    return h;
}

} // namespace

struct VdesReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0, offsetHz = 0, dialHz = 0;
    bool isReady = false, wallClock = false;
    bool forwardNmea = true;
    int nmeaSeq = 0;
    AisReceiver ais;
    bool asmInView = false;
    Band band[2];
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double wNorm = 1;
    uint64_t inSamples = 0;
    double now = 0, nextReport = kReportSec;
    uint64_t seq = 0, aisSeq = 0;
    VdesTelemetry t;                        // counters
    std::deque<VdesAsm> asms;
    std::deque<VdeBurst> bursts;
    double lastAsm = -1e9, lastEnergy = -1e9, lastCfo = 0; float lastSnr = 0;
    uint64_t aisBadSeen = 0;
    VdesTelemetry pub;
    std::vector<float> mf[3];               // matched filters for 25 / 50 / 100 kHz

    void say(const std::string& s) { if (log) log(s); }
    double dial() const { return dialHz > 0 ? dialHz : kVdesDefaultDialHz; }

    Impl() {
        for (int i = 0; i < 3; i++) mf[i] = rrcTaps(kWork / (kVdeSymRate25 * (1 << i)), kVdeRolloff);
        ais.setForwardFeeds(false);
        ais.setFrameCallback([this](char ch, const ais::Bits& bits, double, double snr, double cfo) { onAsm(ch, bits, snr, cfo); });
    }

    void plan() {
        const double aisO = offsetHz + (0.5 * (kVdesAsm1Hz + kVdesAsm2Hz) - dial());   // the AIS demodulator takes the pair as -25 / +25 kHz of this
        asmInView = std::fabs(aisO) + 70000.0 <= 0.46 * fs && fs >= 250000.0;
        if (asmInView) ais.setSignalOffset(aisO);
        for (int b = 0; b < 2; b++) {
            Band& B = band[b];
            B.fIn = offsetHz + (kVdeBandHz[b] - dial());
            B.inView = std::fabs(B.fIn) + 62000.0 <= 0.46 * fs;
            const double w = -2 * kPi * B.fIn / fs;
            B.step = std::complex<double>(std::cos(w), std::sin(w));
            B.rot = {1, 0};
        }
    }

    void configure(double rate) {
        fs = rate;
        isReady = fs >= vdesTuning().minSampleRate - 1;
        if (!isReady) return;
        ais.configure(fs);
        fft = std::make_unique<Fft>(kNf);
        win.resize(kNf);
        wNorm = 0;
        for (int i = 0; i < kNf; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kNf)); wNorm += (double)win[(size_t)i] * win[(size_t)i]; }
        for (auto& B : band) if (!B.rs.configure(fs, kWork)) isReady = false;
        plan();
        resetState();
    }

    void resetState() {
        const double shift = now;
        for (auto& a : asms) a.timeSec -= shift;
        for (auto& b : bursts) b.timeSec -= shift;
        for (auto& B : band) B.reset();
        ais.reset();
        aisSeq = 0; aisBadSeen = 0;
        inSamples = 0; now = 0; nextReport = kReportSec;
        lastAsm = lastEnergy = -1e9;
        publish();
    }

    // ---- ASM
    void onAsm(char ch, const ais::Bits& bits, double snr, double cfo) {
        VdesAsm a;
        a.timeSec = now; a.channel = ch; a.bits = (int)bits.size();
        a.snrDb = (float)snr; a.cfoHz = (float)cfo;
        a.wallTime = wallClock ? (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() : 0;
        static const char* hx = "0123456789ABCDEF";
        for (size_t i = 0; i < bits.size(); i += 8) {
            int v = 0;
            for (size_t k = 0; k < 8; k++) v = (v << 1) | (i + k < bits.size() ? (bits[i + k] & 1) : 0);
            a.hex += hx[v >> 4]; a.hex += hx[v & 15];
        }
        if (bits.size() >= 38) {
            int ty = 0;
            for (int k = 0; k < 6; k++) ty = (ty << 1) | (bits[(size_t)k] & 1);
            if (ty >= 1 && ty <= 27) {
                a.aisType = ty;
                uint32_t m = 0;
                for (int k = 8; k < 38; k++) m = (m << 1) | (bits[(size_t)k] & 1);
                a.aisMmsi = m;
            }
        }
        {   // an AIS-like payload: its position for the map, and the sentence for the AIS data feed (NMEA over UDP, the same as the AIS mode)
            ais::AisMsg am;
            const bool dec = ais::decodeMessage(bits, am);
            if (ais::acceptableAis(am, dec)) {
                if (am.hasPos) { a.hasPos = true; a.lat = am.lat; a.lon = am.lon; a.sogKt = am.sog; a.cogDeg = am.cog; }
                if (forwardNmea && Feeds::get().wantAis()) {
                    Feeds::get().ais(ais::toNmea(bits, ch == 'B' ? 'B' : 'A', nmeaSeq));
                    if (bits.size() > 6 * 60) nmeaSeq = (nmeaSeq + 1) % 10;
                }
            }
        }
        asms.push_front(a);
        while (asms.size() > kVdesAsmMax) asms.pop_back();
        t.blocksOk++; t.asmOk[ch == 'B' ? 1 : 0]++;
        lastAsm = now; lastCfo = cfo; lastSnr = (float)snr;
    }

    // ---- VDE-TER
    void classify(Band& B, int b, const Band& snap, uint64_t s0, uint64_t s1, int s_first, int s_last, double centreHz, int bwKhz, VdeBurst& out) {
        (void)snap; (void)b;
        const int bi = bwKhz == 25 ? 0 : bwKhz == 50 ? 1 : 2;
        const double sps = kWork / (kVdeSymRate25 * (1 << bi));
        (void)s_first; (void)s_last;
        if (s1 <= s0 + 64 || B.total < s1) return;
        if (B.total - s0 > kRing) return;
        const size_t n = (size_t)(s1 - s0);
        std::vector<cf32> x(n);
        const double w = -2 * kPi * centreHz / kWork;
        std::complex<double> r(1, 0), st(std::cos(w), std::sin(w));
        for (size_t i = 0; i < n; i++) {
            const cf32 v = B.ring[(size_t)((s0 + i) % kRing)];
            x[i] = cf32((float)(v.real() * r.real() - v.imag() * r.imag()), (float)(v.real() * r.imag() + v.imag() * r.real()));
            r *= st;
            if ((i & 1023) == 1023) r /= std::abs(r);
        }
        // the carrier offset is already out through the centroid the caller put into centreHz; matched filter
        const std::vector<float>& h = mf[bi];
        const size_t nt = h.size(), delay = nt / 2;
        if (n < nt + 8 * (size_t)sps) return;
        std::vector<cf32> y(n - nt + 1);
        for (size_t i = 0; i + nt <= n; i++) {
            float re = 0, im = 0;
            for (size_t k = 0; k < nt; k++) { re += h[k] * x[i + k].real(); im += h[k] * x[i + k].imag(); }
            y[i] = cf32(re, im);
        }
        (void)delay;
        // use the middle of the burst: the matched filter output is full only away from the edges
        size_t lo = (size_t)(2 * sps), hi = y.size() > (size_t)(2 * sps) ? y.size() - (size_t)(2 * sps) : 0;
        {   // the window holds up to a frame of quiet either side of the burst: keep the part where the power is within 3 dB of its peak
            const size_t w = (size_t)(4 * sps);
            if (y.size() > 3 * w) {
                std::vector<double> sm(y.size() - w + 1);
                double acc = 0, pk = 0;
                for (size_t i = 0; i < w; i++) acc += std::norm(y[i]);
                for (size_t i = 0; i < sm.size(); i++) { sm[i] = acc; pk = std::max(pk, acc); if (i + w < y.size()) acc += std::norm(y[i + w]) - std::norm(y[i]); }
                size_t f = 0, l = sm.size() - 1;
                while (f < l && sm[f] < 0.5 * pk) f++;
                while (l > f && sm[l] < 0.5 * pk) l--;
                lo = std::max(lo, f + w); hi = std::min(hi, l);
            }
        }
        if (hi <= lo + (size_t)(30 * sps)) return;
        std::complex<double> e(0, 0);
        for (size_t i = lo; i < hi; i++) {
            const double p = std::norm(y[i]);
            e += p * std::complex<double>(std::cos(2 * kPi * (double)i / sps), -std::sin(2 * kPi * (double)i / sps));
        }
        const double th = std::arg(e);                          // |y|^2 peaks at i = th / (2 pi) * sps (mod sps)
        double first = -th / (2 * kPi) * sps;   // |y|^2 peaks at i = -arg(e) / (2 pi) * sps (mod sps)
        while (first < 0) first += sps;
        while (first < (double)lo) first += sps;
        std::vector<cf32> s;
        for (double pos = first; pos + 2 < (double)hi; pos += sps) {   // cubic (Catmull-Rom) interpolation between the matched filter's samples
            const size_t i0 = (size_t)pos;
            if (i0 < 1) continue;
            const float u = (float)(pos - (double)i0);
            const cf32 p0 = y[i0 - 1], p1 = y[i0], p2 = y[i0 + 1], p3 = y[i0 + 2];
            s.push_back(p1 + 0.5f * u * ((p2 - p0) + u * ((2.f * p0 - 5.f * p1 + 4.f * p2 - p3) + u * (3.f * (p1 - p2) + p3 - p0))));
        }
        if (s.size() < 24) return;
        double mean = 0;
        for (auto& v : s) mean += std::abs(v);
        mean /= (double)s.size();
        if (mean <= 0) return;
        double var = 0;
        for (auto& v : s) { const double d = std::abs(v) / mean - 1.0; var += d * d; }
        const double cv = std::sqrt(var / (double)s.size());
        double psi = 0;
        for (size_t k = 1; k < s.size(); k++) psi += std::cos(4 * std::arg(s[k] * std::conj(s[k - 1])));
        psi /= (double)(s.size() - 1);
        out.cv = (float)cv; out.psi = (float)psi; out.nSym = (int)s.size();
        if (cv > 0.18) out.mod = VdeMod16Qam;
        else if (psi < -0.55) out.mod = VdeModPi4Qpsk;
        else if (std::fabs(psi) < 0.45) out.mod = VdeMod8Psk;
        else out.mod = VdeModUnknown;
    }

    void endBurst(int b) {
        Band& B = band[b];
        const int frames = B.frames, nAcc = std::max(1, B.nAcc);
        B.burst = false;
        if (frames < kMinFrames) return;
        // occupied sub-channels: active in at least 40 % of the frames
        int first = -1, last = -1, cnt = 0;
        for (int s = 0; s < 4; s++) if (B.act[s] >= std::max(2, (int)(0.4 * nAcc))) { if (first < 0) first = s; last = s; cnt++; }
        if (first < 0) return;
        const int span = last - first + 1;
        const int bw = span <= 1 ? 25 : span == 2 ? 50 : 100;
        const double centre = bw == 100 ? 0.0 : ((first + last) / 2.0 - 1.5) * 25000.0;
        double P = 0, N = 0;
        for (int s = first; s <= last; s++) { P += B.sumP[s]; N += B.sumN[s]; }
        const double act = (double)nAcc;
        VdeBurst v;
        v.band = b;
        v.timeSec = (double)B.b0 * kHop / kWork;
        v.wallTime = wallClock ? (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() : 0;
        v.bwKhz = bw;
        v.rssiDb = (float)(10 * std::log10(std::max(1e-12, P / act)));
        v.snrDb = N > 0 ? (float)(10 * std::log10(std::max(1e-3, (P - N) / N))) : 0.f;
        v.durMs = (float)(frames * kHop / kWork * 1000.0);
        // the carrier offset: centroid of the burst's spectrum above the floor, over the occupied band
        double sw = 0, sx = 0;
        const double lim = bw / 2.0 * 1000.0 * 1.15;
        double nbin = 0;
        { int nb = 0; for (int k = 0; k < kNf; k++) { const int s = subOfBin(k); if (s >= first && s <= last) nb++; } nbin = N / act / std::max(1, nb); }
        for (int k = 0; k < kNf; k++) {
            const double f = (k < kNf / 2 ? k : k - kNf) * kDf;
            if (std::fabs(f - centre) > lim) continue;
            const double val = B.bacc[(size_t)k] / act / (double)kNf / wNorm - nbin;
            if (val > 0) { sw += val; sx += val * (f - centre); }
        }
        const double cfo = sw > 0 ? sx / sw : 0.0;
        v.cfoHz = (float)cfo;
        v.centreMhz = (kVdeBandHz[b] + centre + cfo) / 1e6;
        const uint64_t s0 = B.b0 * kHop, s1 = (B.b0 + (uint64_t)frames) * kHop + kNf;
        classify(B, b, B, s0, s1, first, last, centre + cfo, bw, v);
        bursts.push_front(v);
        while (bursts.size() > kVdeBurstsMax) bursts.pop_back();
        t.vdeBursts++; t.vdeByMod[v.mod]++; t.vdeByBw[bw == 25 ? 0 : bw == 50 ? 1 : 2]++;
        lastEnergy = now;
        t.lastVdeSec = now;
    }

    void onFrame(int b, const cf32* fr) {
        Band& B = band[b];
        std::vector<cf32> w(kNf);
        for (int k = 0; k < kNf; k++) w[(size_t)k] = fr[k] * win[(size_t)k];
        fft->forward(w.data());
        const double per = 1.0 / ((double)kNf * wNorm);
        double p[4] = {};
        double pk[kNf];
        for (int k = 0; k < kNf; k++) {
            pk[k] = (double)std::norm(w[(size_t)k]);
            const int s = subOfBin(k);
            if (s >= 0) p[s] += pk[k] * per;
        }
        B.frame++;
        double tot = p[0] + p[1] + p[2] + p[3];
        B.level += 0.02 * (tot - B.level);
        if (!B.nfKnown) {
            for (int s = 0; s < 4; s++) B.warm[s] += p[s];
            if (++B.warmN >= kWarmFrames) {
                double mn = 1e30;
                for (int s = 0; s < 4; s++) { B.warm[s] /= B.warmN; mn = std::min(mn, B.warm[s]); }
                for (int s = 0; s < 4; s++) B.nf[s] = std::min(B.warm[s], mn * 1.26);
                B.nfKnown = true;
            }
            return;
        }
        bool any = false;
        for (int s = 0; s < 4; s++) {
            if (p[s] < B.nf[s]) B.nf[s] += 0.2 * (p[s] - B.nf[s]);
            else if (!B.on[s] && p[s] < 2.5 * B.nf[s]) B.nf[s] += 0.004 * (p[s] - B.nf[s]);
            if (!B.on[s] && p[s] > kOnRatio * B.nf[s]) B.on[s] = true;
            else if (B.on[s] && p[s] < kOffRatio * B.nf[s]) B.on[s] = false;
            any = any || B.on[s];
        }
        if (any) {
            if (!B.burst) {
                B.burst = true; B.b0 = B.frame - 1; B.quiet = 0; B.frames = 0; B.nAcc = 0;
                for (int s = 0; s < 4; s++) { B.sumP[s] = B.sumN[s] = 0; B.act[s] = 0; }
                std::fill(B.bacc.begin(), B.bacc.end(), 0.0);
            }
            B.quiet = 0;
        } else if (B.burst) B.quiet++;
        if (B.burst) {
            if (any) {
                B.nAcc++; B.frames = (int)(B.frame - B.b0);
                for (int s = 0; s < 4; s++) { B.sumP[s] += p[s]; B.sumN[s] += B.nf[s]; if (B.on[s]) B.act[s]++; }
                for (int k = 0; k < kNf; k++) B.bacc[(size_t)k] += pk[k];
            }
            if (B.quiet >= kEndFrames || (int)(B.frame - B.b0) >= kMaxFrames) endBurst(b);
        }
    }

    void feedBand(int b, const cf32* x, size_t n) {
        Band& B = band[b];
        B.mix.resize(n);
        std::complex<double> r = B.rot;
        for (size_t i = 0; i < n; i++) {
            const double re = x[i].real() * r.real() - x[i].imag() * r.imag();
            const double im = x[i].real() * r.imag() + x[i].imag() * r.real();
            B.mix[i] = cf32((float)re, (float)im);
            r *= B.step;
        }
        B.rot = r / std::abs(r);
        B.wk.clear();
        B.rs.process(B.mix.data(), n, B.wk);
        for (const cf32& v : B.wk) { B.ring[(size_t)(B.total % kRing)] = v; B.total++; }
        B.pend.insert(B.pend.end(), B.wk.begin(), B.wk.end());
        while (B.pend.size() - B.pendHead >= (size_t)kNf) {
            onFrame(b, B.pend.data() + B.pendHead);
            B.pendHead += kHop;
        }
        if (B.pendHead > 65536) { B.pend.erase(B.pend.begin(), B.pend.begin() + (long)B.pendHead); B.pendHead = 0; }
    }

    void publish() {
        VdesTelemetry o = t;
        o.timeSec = now; o.inputRate = fs; o.dialMhz = dialHz / 1e6;
        o.asmInView = asmInView;
        for (int b = 0; b < 2; b++) {
            o.bandInView[b] = band[b].inView;
            if (band[b].inView && band[b].nfKnown) {
                o.vdeLevelDb[b] = (float)(10 * std::log10(std::max(1e-12, band[b].level)));
                double n = 0; for (int s = 0; s < 4; s++) n += band[b].nf[s];
                o.vdeNoiseDb[b] = (float)(10 * std::log10(std::max(1e-12, n)));
            }
        }
        AisTelemetry at;
        if (asmInView && ais.telemetry(at, 0)) {
            for (int c = 0; c < 2; c++) { o.asmLevelDb[c] = at.levelDbfs[c]; o.asmNoiseDb[c] = at.noiseDbfs[c]; o.asmBad[c] = at.channelBad[c]; }
            o.blocksBad = at.blocksBad;
            if (at.blocksBad > aisBadSeen) { lastEnergy = now; aisBadSeen = at.blocksBad; }
        }
        o.dataValid = now - lastAsm < 30;
        o.state = o.dataValid ? 2 : (now - lastEnergy < 30 ? 1 : 0);
        o.cfoHz = o.dataValid ? lastCfo : 0;
        o.snrDb = o.dataValid ? lastSnr : 0;
        o.asms.assign(asms.begin(), asms.end());
        o.bursts.assign(bursts.begin(), bursts.end());
        std::lock_guard<std::mutex> lk(telMu);
        o.seq = ++seq;
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        if (asmInView) ais.feed(x, n);
        for (int b = 0; b < 2; b++) if (band[b].inView) feedBand(b, x, n);
        inSamples += n;
        now = (double)inSamples / fs;
        while (now >= nextReport) { nextReport += kReportSec; publish(); }
    }
};

VdesReceiver::VdesReceiver() : p_(std::make_unique<Impl>()) {}
VdesReceiver::~VdesReceiver() = default;

void VdesReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->configure(inputRateHz); }
void VdesReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->offsetHz = hz; if (p_->isReady) p_->plan(); }
void VdesReceiver::setDialHz(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); if (hz == p_->dialHz) return; p_->dialHz = hz; if (p_->isReady) { p_->plan(); p_->resetState(); } }
void VdesReceiver::setForwardNmea(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->forwardNmea = on; }
void VdesReceiver::setWallClock(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->wallClock = on; }
bool VdesReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void VdesReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->isReady) p_->resetState(); }
void VdesReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
bool VdesReceiver::telemetry(VdesTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void VdesReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->asms.clear(); p_->bursts.clear();
    p_->t = VdesTelemetry();
    p_->ais.clearResults();
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.asms.clear(); p_->pub.bursts.clear();
    p_->pub.blocksOk = 0; p_->pub.vdeBursts = 0;
    p_->pub.seq = ++p_->seq;
}
std::string VdesReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return results::pack(p_->now, p_->asms, p_->bursts);
}
bool VdesReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::deque<VdesAsm> a;
    std::deque<VdeBurst> b;
    if (!results::unpack(blob, t0, a, b)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    for (auto& v : a) v.timeSec += p_->now - t0;
    for (auto& v : b) v.timeSec += p_->now - t0;
    p_->asms = std::move(a); p_->bursts = std::move(b);
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.asms.assign(p_->asms.begin(), p_->asms.end());
    p_->pub.bursts.assign(p_->bursts.begin(), p_->bursts.end());
    p_->pub.seq = ++p_->seq;
    return true;
}
void VdesReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

ModeTuning vdesTuning() {
    ModeTuning t;
    t.stdMode = 65; t.id = "vdes"; t.name = "VDES";
    t.minMhz = 150; t.maxMhz = 165; t.defMhz = 161.9;
    t.sampleRate = 1000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 400000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
