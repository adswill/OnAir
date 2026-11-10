// Link 11 CLEW message detector and frame-structure reader, and its test generator; see hfu_link11.h (what is and is not read, and why).
#include "dect2/hfutil/hfu_link11.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cf = std::complex<float>;
constexpr double kFs = 24000.0;
constexpr int kFastF = 320, kSlowF = 528;          // 13.33 ms and 22 ms at 24 kHz
constexpr int kFastW = 218, kSlowW = 436;          // 1/110 s and 2/110 s: the tones (110 Hz apart) are orthogonal over these
constexpr int kHop = 40;
constexpr int kTones = 15;
const double kNoiseHz[4] = {1430.0, 1650.0, 1870.0, 2090.0};   // between the data tones and far from the Doppler and sync tones: the noise floor
}  // namespace

std::vector<cf32> genLink11(const Link11GenCfg& c, const std::vector<std::array<uint8_t, 15>>& frames) {
    const int F = c.fast ? kFastF : kSlowF;
    const size_t lead = (size_t)(c.leadSec * kFs), tail = (size_t)(c.tailSec * kFs);
    const int nPre = c.preambleFrames, nAll = nPre + 1 + (int)frames.size();
    std::vector<cf32> out(lead + (size_t)nAll * F + tail, cf32(0, 0));
    double ph[kTones];
    for (int k = 0; k < kTones; k++) ph[k] = 0.7 * k;   // the phase reference frame's phases
    const double off = c.offsetDeg * kPi / 180;
    for (int fr = 0; fr < nAll; fr++) {
        if (fr > nPre) for (int k = 0; k < kTones; k++) ph[k] += off + (kPi / 2) * frames[(size_t)(fr - nPre - 1)][(size_t)k];
        for (int i = 0; i < F; i++) {
            const size_t n = lead + (size_t)fr * F + (size_t)i;
            const double t = (double)n / kFs;
            cf v((float)std::cos(2 * kPi * kL11Doppler * t), (float)std::sin(2 * kPi * kL11Doppler * t));
            v *= 1.4f;
            if (fr < nPre) {
                const double a = 2 * kPi * kL11Sync * t + ((fr & 1) ? kPi : 0);   // the sync tone turns over every preamble frame
                v += cf((float)std::cos(a), (float)std::sin(a)) * 1.4f;
            } else {
                for (int k = 0; k < kTones; k++) { const double a = 2 * kPi * l11DataTone(k) * t + ph[k]; v += cf((float)std::cos(a), (float)std::sin(a)); }
            }
            out[n] = v * 0.1f;
        }
    }
    return out;
}

struct Link11Rx::Impl {
    std::vector<cf> x;
    int64_t x0 = 0, nX = 0, pos = 0;   // pos: the next hop's window start (absolute)
    int state = 0, preHops = 0, syncSeen = 0;
    double dopSum = 0; int dopN = 0; double dop = 0;
    cf lastDop; bool haveLastDop = false;
    int64_t preStart = 0;
    // frames
    bool fast = true;
    int64_t b0 = 0;                    // start of the phase reference frame
    int frame = 0;
    std::vector<cf> prevX;
    float refEm = 0;
    int lowRun = 0;
    Link11Msg msg;
    std::vector<std::array<float, 15>> dph;
    double sigAcc = 0, noiseAcc = 0;
    uint64_t nMsg = 0;
    std::function<void(const Link11Msg&)> msgCb;
    std::function<void(const std::string&)> evCb;

    void event(const std::string& s) { if (evCb) evCb(s); }

    // DFT of the window [start, start + len) at f, referred to absolute time (a steady tone keeps its phase from window to window)
    cf dft(double f, int64_t start, int len) const {
        const int64_t a = start - x0;
        if (a < 0 || a + len > (int64_t)x.size()) return cf(0, 0);
        const double w = -2 * kPi * f / kFs;
        const double p0 = std::fmod(w * (double)start, 2 * kPi);
        std::complex<double> acc(0, 0), r(std::cos(p0), std::sin(p0));
        const std::complex<double> step(std::cos(w), std::sin(w));
        for (int n = 0; n < len; n++) { acc += std::complex<double>(x[(size_t)(a + n)]) * r; r *= step; }
        return cf((float)acc.real(), (float)acc.imag());
    }

    void push(const cf32* in, size_t n) {
        x.insert(x.end(), in, in + n);
        nX += (int64_t)n;
        for (;;) {
            if (state < 2) {
                const int64_t look = state == 1 ? 9 * kSlowF + 2 * kSlowW + kFastW + 100 : kFastW + 2;   // state 1 may start the frames: room to measure the rate
                if (pos + look >= nX) break;
                hop();
                if (state < 2) pos += kHop;
            }
            else {
                const int F = fast ? kFastF : kSlowF, W = fast ? kFastW : kSlowW;
                const int64_t ws = b0 + (int64_t)frame * F + (F - W) / 2;
                if (ws + W + 2 >= nX) break;
                frameStep(ws, W);
            }
        }
        const int64_t keepFrom = std::min<int64_t>(nX - 30000, (state == 2 ? b0 : pos) - 2000);
        if (keepFrom > x0 + 30000) { x.erase(x.begin(), x.begin() + (long)(keepFrom - x0)); x0 = keepFrom; }
    }

    void energies(int64_t t, int W, double shift, double& ed, double& es, double& em, double& en) const {
        ed = std::norm(dft(kL11Doppler + shift, t, W));
        es = std::norm(dft(kL11Sync + shift, t, W));
        em = 0;
        for (int k = 0; k < 14; k++) em += std::norm(dft(l11DataTone(k) + shift, t, W));
        em /= 14;
        en = 0;
        for (double f : kNoiseHz) en += std::norm(dft(f + shift, t, W));
        en /= 4;
    }

    void hop() {
        double ed, es, em, en;
        energies(pos, kFastW, state == 1 ? dop : 0, ed, es, em, en);
        const double floor = std::max(en, 1e-12);
        if (state == 0) {
            // the Doppler tone alone decides each hop: a window across a turn-over of the sync tone can hold almost none of it
            const bool pre = ed > 8 * floor && em < 0.2 * ed;
            if (pre) {
                if (preHops == 0) { preStart = pos; dopSum = 0; dopN = 0; haveLastDop = false; syncSeen = 0; }
                preHops++;
                if (es > 8 * floor && em < 0.2 * es) syncSeen++;
                const cf d = dft(kL11Doppler, pos, kFastW);
                if (haveLastDop) { dopSum += std::arg(d * std::conj(lastDop)); dopN++; }
                lastDop = d; haveLastDop = true;
                if (preHops >= 12 && syncSeen >= 3) {   // 20 ms of the Doppler tone alone, the sync tone with it
                    dop = dopN ? dopSum / dopN * kFs / (2 * kPi * kHop) : 0;
                    state = 1;
                    event("preamble (Doppler and sync tones)");
                }
            } else preHops = 0;
            return;
        }
        // state 1: the preamble goes on until the data tones start
        if (em > 0.25 * ed) { startFrames(pos); return; }   // the data tones are about half the Doppler tone's power each; it also gains their leakage
        if (ed < 4 * floor) { state = 0; preHops = 0; }   // the tones went away without data
        else if (preHops < 1000) {
            const cf d = dft(kL11Doppler, pos, kFastW);
            if (haveLastDop) { dopSum += std::arg(d * std::conj(lastDop)); dopN++; dop = dopSum / dopN * kFs / (2 * kPi * kHop); }
            lastDop = d; haveLastDop = true;
            preHops++;
        }
    }

    // alignment and rate: the windows that fit inside the frames keep all the tone energy
    // a frame keeps each tone's phase: the first and the last 1/110 s of a frame (orthogonal windows) agree when the rate and the alignment
    // are right, and turn by the data when a frame boundary falls between them
    double score(int64_t b, bool f) const {
        const int F = f ? kFastF : kSlowF;
        double num = 0, den = 0;
        for (int i = 0; i < 8; i++) {
            const int64_t w1 = b + (int64_t)i * F + 4, w2 = b + (int64_t)(i + 1) * F - kFastW - 4;
            if (w2 + kFastW >= nX) break;
            for (int k = 0; k < kTones; k++) {
                const cf x1 = dft(l11DataTone(k) + dop, w1, kFastW), x2 = dft(l11DataTone(k) + dop, w2, kFastW);
                num += (x1 * std::conj(x2)).real();
                den += std::abs(x1) * std::abs(x2);
            }
        }
        return den > 0 ? num / den : 0;
    }

    void startFrames(int64_t t) {
        // the window [t, t + W) was the first to hold the data tones: their onset is in it
        double best = -1, bestOf[2] = {-1, -1};
        int64_t bb = t;
        bool bf = true;
        for (bool f : {true, false}) for (int64_t b = t - 40; b <= t + kFastW + 40; b += 4) {
            const double s = score(b, f);
            bestOf[f ? 0 : 1] = std::max(bestOf[f ? 0 : 1], s);
            if (s > best) { best = s; bb = b; bf = f; }
        }
        char ev[120];
        snprintf(ev, sizeof ev, "data tones start (phase hold: fast %.2f, slow %.2f)", bestOf[0], bestOf[1]);
        event(ev);
        fast = bf; b0 = bb; frame = 0;
        prevX.assign(kTones + 1, cf(0, 0));
        dph.clear();
        refEm = 0; lowRun = 0; sigAcc = 0; noiseAcc = 0;
        msg = Link11Msg();
        msg.fast = fast; msg.dopplerHz = dop; msg.startSec = (double)preStart / kFs;
        msg.preambleFrames = (int)std::lround((double)(b0 - preStart) / (fast ? kFastF : kSlowF));
        state = 2;
    }

    void frameStep(int64_t ws, int W) {
        std::vector<cf> X(kTones + 1);
        double em = 0;
        for (int k = 0; k < kTones; k++) { X[(size_t)k] = dft(l11DataTone(k) + dop, ws, W); em += std::norm(X[(size_t)k]); }
        X[kTones] = dft(kL11Doppler + dop, ws, W);
        em /= kTones;
        double en = 0;
        for (double f : kNoiseHz) en += std::norm(dft(f + dop, ws, W));
        en /= 4;
        if (frame == 0) refEm = (float)em;
        const bool low = em < 0.15 * refEm;
        lowRun = low ? lowRun + 1 : 0;
        if (lowRun >= 2 || frame > 20000) { finish(); return; }
        if (frame > 0 && !low) {
            std::array<float, 15> d{};
            const double dd = std::arg(X[kTones] * std::conj(prevX[kTones]));   // the Doppler tone's turn: common to all tones
            for (int k = 0; k < kTones; k++) d[(size_t)k] = (float)std::remainder(std::arg(X[(size_t)k] * std::conj(prevX[(size_t)k])) - dd, 2 * kPi);
            dph.push_back(d);
            sigAcc += em; noiseAcc += en;
        }
        if (!low) prevX = X;
        frame++;
    }

    void finish() {
        const int F = fast ? kFastF : kSlowF;
        // the constellation offset from the fourth power of all phase changes
        std::complex<double> acc(0, 0);
        for (const auto& d : dph) for (float v : d) acc += std::polar(1.0, 4.0 * v);
        const double off = std::arg(acc) / 4;
        msg.offsetDeg = std::fmod(off * 180 / kPi + 90, 90);
        for (const auto& d : dph) {
            std::array<uint8_t, 15> q{};
            for (int k = 0; k < kTones; k++) q[(size_t)k] = (uint8_t)(((int)std::lround((d[(size_t)k] - off) / (kPi / 2)) % 4 + 4) % 4);
            msg.phases.push_back(q);
        }
        msg.frames = (int)dph.size();
        msg.durationSec = (double)(b0 + (int64_t)(frame - lowRun) * F - preStart) / kFs;
        // per-tone energy over the noise per tone bin
        msg.snrDb = noiseAcc > 0 ? 10 * std::log10(sigAcc / noiseAcc) : 0;
        nMsg++;
        char b[160];
        snprintf(b, sizeof b, "message: %s frames (%s), %d data frames, %.2f s, Doppler %+.1f Hz", fast ? "fast" : "slow", fast ? "13.33 ms" : "22 ms", msg.frames, msg.durationSec, msg.dopplerHz);
        event(b);
        if (msgCb) msgCb(msg);
        state = 0; preHops = 0;
        pos = b0 + (int64_t)frame * F;
    }
};

Link11Rx::Link11Rx() : p_(new Impl) {}
Link11Rx::~Link11Rx() = default;
void Link11Rx::reset() { auto m = std::move(p_->msgCb); auto e = std::move(p_->evCb); p_.reset(new Impl); p_->msgCb = std::move(m); p_->evCb = std::move(e); }
void Link11Rx::push(const cf32* x, size_t n) { p_->push(x, n); }
void Link11Rx::setMessageSink(std::function<void(const Link11Msg&)> cb) { p_->msgCb = std::move(cb); }
void Link11Rx::setEventSink(std::function<void(const std::string&)> cb) { p_->evCb = std::move(cb); }
int Link11Rx::state() const { return p_->state; }
uint64_t Link11Rx::messages() const { return p_->nMsg; }

}  // namespace dect2::hfu
