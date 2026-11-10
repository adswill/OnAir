// PSK31 / PSK63 multi-signal receiver (see psk_rx.h), and fldigi's 8PSK125 / 8PSK250 without FEC.
#include "dect2/psk/psk_rx.h"
#include "dect2/autoparam/autoparam.h"
#include "dect2/results/results_io.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/psk/psk_varicode.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/engine/widespan.h"
#include "../audio/ham_listen.h"
#include "dect2/net/pskreporter.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <future>
#include <cmath>
#include <cstdlib>
#include <complex>
#include <mutex>
#include <utility>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kBase = marine::kBaseRate;      // 24 kHz complex
constexpr double kWideSpanMaxPsk = 100000;       // whole span: a receiver per 3.4 kHz of it (about 20 % of a core at 100 kHz)
static const bool kPskIsiBpsk = !std::getenv("PSK_NOISI");   // measuring only (the PSK_NO* switches)
static const bool kPskReplay = !std::getenv("PSK_NOREPLAY");
static const bool kPskSquelch = !std::getenv("PSK_NOSQL");
constexpr double kReplaySec = 1.5;
static const float kSqlA = std::getenv("PSK_SQLA") ? (float)std::atof(std::getenv("PSK_SQLA")) : 0.4f;
static const float kSqlT = std::getenv("PSK_SQLT") ? (float)std::atof(std::getenv("PSK_SQLT")) : 0.2f;
constexpr int kN = 4096, kHop = 2048;            // window spectrum
constexpr double kBinHz = kBase / kN;
constexpr double kLoHz = 100, kHiHz = 3900;      // the audio passband searched
constexpr int kLoBin = (int)(kLoHz / kBinHz) + 1, kHiBin = (int)(kHiHz / kBinHz);
constexpr int kR = 24;                           // 24 kHz -> 1 kHz
constexpr int kTri = 2 * kR - 1;
constexpr double kRate1 = kBase / kR;            // 1 kHz

struct Chan {
    int id = 0;
    double f = 0, f0 = 0;        // offset from the dial now, and where it was found
    bool manual = false;
    std::complex<double> rot{1, 0}, step{1, 0};
    int rotCount = 0;
    cf32 tri[2 * kTri] = {};
    int triPos = 0, phase = 0;
    std::vector<cf32> mf;        // matched filter history (twice, so a window never wraps)
    int mfPos = 0;
    // symbol clock
    std::vector<float> bins;
    int ph = 0, idx = 0, since = 0, syms = 0;
    std::vector<cf32> yh;        // the last symbol's worth of filter outputs (the Gardner mid sample)
    int yhPos = 0;
    cf32 lastSym{0, 0}, sym1{0, 0}, sym2{0, 0}, lastSym2{0, 0};   // the last symbols (the QPSK equaliser)
    float pw = 0, gacc = 0;
    std::complex<float> prevY{0, 0};
    bool havePrev = false;
    float q = 0.2f;              // phase decision quality: 1 clean, 0 noise
    float errVar = 1;            // mean square phase error: the SNR (Es/N0 = 1 / variance for differential detection)
    float errShown = 1;          // errVar of a second ago while text came out: what is shown (the noise after a signal ends would read ~1 dB)
    float errHist[32] = {};
    int errPos = 0;
    bool dcd = false;
    float fastPw = 0, dcdPw = 0;  // symbol power: quick, and slow while decoding (a signal that stops drops the DCD at once)
    psk::Viterbi vit;
    psk::VaricodeRx vrx;
    psk::MfskVaricodeRx mrx;     // 8PSK
    int run4 = 0;                // 8PSK: steady symbols in a row (the postamble carrier, not text)
    std::string text;
    std::vector<PskLine> lines;
    bool lineOpen = false;
    std::vector<std::string> calls;
    double firstSec = 0, lastActive = 0, lastPeak = 0;
    uint64_t chars = 0, bad = 0;
    void setFreq(double hz) { f = hz; step = std::polar(1.0, -2 * kPi * hz / kBase); }
};

} // namespace

struct PskReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    PskTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    bool focusReq = false; double focusHz = 0;   // under mu
    std::atomic<float> thrDb{5.f};
    std::atomic<int> modeReq{0};
    // Auto (setAuto): the last seconds of the 24 kHz band go through a trial decoder of each modulation on another thread, the one with the most good text is kept
    std::atomic<bool> autoOn{false}, autoKick{false};
    bool trial = false;                       // a trial decoder: nothing is reported to the outside world
    uint64_t p8Odd = 0, p8All = 0;            // 8PSK decisions on an odd eighth turn, and all (Auto: BPSK and QPSK never use the odd ones)
    std::vector<cf32> autoRing;
    size_t autoPos = 0, autoFill = 0;
    uint64_t autoSamples = 0, autoLastRun = 0, autoCharsSeen = 0, autoGoodAt = 0;
    int autoFound = -1, autoCand = -1;
    bool autoLocked = false;
    std::future<std::pair<int, uint64_t>> autoJob;
    bool autoDiscard = false;
    static std::pair<int, uint64_t> trialAll(const std::vector<cf32>& band, float thr);
    static uint64_t trialMode(const std::vector<cf32>& band, int mode, float thr);
    void autoFeed(const cf32* x, size_t n);
    // receiver thread
    PskTelemetry tel;
    double curRate = 0;
    bool running = false;
    int mode = 0, sps = 32;
    std::vector<cf32> hist;      // the last kReplaySec of the 24 kHz band (replay)
    size_t histPos = 0;
    std::vector<float> h;        // matched filter
    int64_t nIn = 0, nBase = 0;
    double nextReport = 0;
    double power = 0; int64_t nPower = 0;
    marine::Front front;
    hfdig::Listen listen;        // "Listen": the sideband on the sound card (off by default)
    std::vector<cf32> base, fftIn;
    Fft fft{kN};
    std::vector<float> win, held, acc;
    int accN = 0, frames = 0;
    float floorLin = 0;
    float triW[kTri] = {};
    std::vector<Chan> chans;
    int nextId = 1, focusId = -1;
    // whole span
    std::atomic<bool> wideReq{false}, wideChanged{false};
    std::atomic<double> wideSpanReq{250000};
    std::atomic<double> spotShift{0};
    struct Wide {
        WideDecimator dec;
        std::vector<std::unique_ptr<PskReceiver>> subs;
        std::vector<double> centre;      // the dial of each slice, from the real dial
        std::vector<cf32> buf;
        int focusSlice = -1;
    };
    std::unique_ptr<Wide> wide;
    static constexpr double kSliceLo = 300, kSliceHi = 3700, kSliceStep = kSliceHi - kSliceLo;   // the part of a slice's 100 .. 3900 Hz that counts: 200 Hz from its edges
    void reportWide();
    void buildWide() {
        wide.reset();
        if (!wideReq.load() || !running) return;
        auto w = std::make_unique<Wide>();
        double off;
        { std::lock_guard<std::mutex> lk(mu); off = offsetHz; }
        w->dec.configure(curRate, std::min(wideSpanReq.load(), kWideSpanMaxPsk * wideSpanCapHz() / kWideMaxSpanHz));   // (the default 100 kHz scales with the span cap)
        w->dec.setOffsetHz(off);
        const double half = w->dec.spanHz() / 2;
        const int k0 = (int)std::floor((-half - kSliceLo) / kSliceStep), k1 = (int)std::ceil((half - kSliceLo) / kSliceStep);
        for (int k = k0; k <= k1; k++) {
            auto r = std::make_unique<PskReceiver>();
            const double c = k * kSliceStep;
            r->setSilent(true);
            r->setSpotShiftHz(c);
            r->setThresholdDb(thrDb.load());
            r->setMode(modeReq.load());
            r->configure(w->dec.outRate());
            r->setSignalOffset(c);
            w->centre.push_back(c);
            w->subs.push_back(std::move(r));
        }
        wide = std::move(w);
    }

    Impl() {
        win.resize(kN);
        for (int i = 0; i < kN; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kN));
        for (int j = 0; j < kTri; j++) triW[j] = (float)((kR - std::abs(j - (kR - 1))) / (double)(kR * kR));
    }

    // signals that are gone (silent too long, a change of mode, a reset) keep their text until Clear: listed with the live ones (not
    // decoding), and a new signal found where one of them was carries its text on. Feed thread; Clear and a loaded file reach them through
    // these requests (mu)
    std::vector<PskSignal> kept;       // oldest first, at most kKeptMax
    static constexpr size_t kKeptMax = 2 * kPskMaxSignals;
    bool keptClear = false, keptLoad = false;
    std::vector<PskSignal> keptIn;
    double keptInAt = 0;
    static void shiftSig(PskSignal& s, double dt) { s.lastActiveSec += dt; s.firstSec += dt; for (auto& l : s.lines) l.timeSec += dt; }
    void retire(const Chan& c) {
        if (c.chars == 0) return;
        PskSignal s;
        s.id = c.id; s.offsetHz = c.f;
        s.lastActiveSec = c.lastActive; s.firstSec = c.firstSec;
        s.chars = c.chars; s.text = c.text; s.calls = c.calls; s.lines = c.lines;
        kept.push_back(std::move(s));
        if (kept.size() > kKeptMax) kept.erase(kept.begin());
    }
    void seed(Chan& c, double hz) {   // a new signal where a gone one was: its text goes on
        for (size_t i = 0; i < kept.size(); i++) {
            const PskSignal& k = kept[i];
            if (std::fabs(k.offsetHz - hz) > 1.4 * baud()) continue;
            c.id = k.id; c.text = k.text; c.calls = k.calls; c.chars = k.chars; c.firstSec = k.firstSec; c.lines = k.lines;
            kept.erase(kept.begin() + (long)i);
            return;
        }
    }
    void takeKept() {
        std::lock_guard<std::mutex> lk(mu);
        if (keptClear) { kept.clear(); keptClear = false; }
        if (keptLoad) {
            kept = std::move(keptIn); keptIn.clear();
            for (auto& s : kept) { shiftSig(s, tel.timeSec - keptInAt); nextId = std::max(nextId, s.id + 1); }
            keptLoad = false;
        }
    }
    double sigSec() const { return (double)nBase / kBase; }
    double baud() const { return pskBaud(mode); }

    void setupMode(int m) {
        mode = std::max(0, std::min(kPskModes - 1, m));
        sps = (int)std::lround(kRate1 / baud());
        const int L = 2 * sps;
        h.assign((size_t)L, 0.f);
        double s = 0;
        for (int i = 0; i < L; i++) { h[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / L)); s += h[(size_t)i]; }
        for (float& v : h) v = (float)(v / s);
        for (const Chan& c : chans) retire(c);
        chans.clear(); focusId = -1;
        tel.mode = mode;
    }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        {   // the clock starts again: the signals heard so far stay (kept), moved back by the time it had reached
            const double t0 = sigSec();
            for (const Chan& c : chans) retire(c);
            chans.clear(); focusId = -1;
            for (auto& k : kept) shiftSig(k, -t0);
        }
        const uint64_t s = tel.seq, ss = tel.specSeq;
        tel = PskTelemetry();
        tel.seq = s; tel.specSeq = ss;
        tel.inputRate = curRate;
        tel.loHz = kLoBin * kBinHz; tel.hiHz = kHiBin * kBinHz;
        nIn = nBase = 0; nextReport = 0; power = 0; nPower = 0;
        running = curRate >= pskTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        listen.reset();
        base.clear(); fftIn.clear();
        const int W = kHiBin - kLoBin + 1;
        held.assign((size_t)W, 0.f); acc.assign((size_t)W, 0.f); accN = 0; frames = 0; floorLin = 0;
        hist.assign((size_t)(kReplaySec * kBase), cf32(0, 0)); histPos = 0;
        setupMode(modeReq.load());
        buildWide();
        if (cb && curRate > 0 && wide) cb("PSK: decoding the whole span (" + std::to_string((int)(wide->dec.spanHz() / 1000)) + " kHz)");
        else if (cb && curRate > 0) cb(running ? std::string("PSK: looking for ") + pskModeName(mode) + " signals from dial + 100 Hz to dial + 3.9 kHz"
                                          : "PSK: the sample rate is too low (250 kHz at least)");
    }

    Chan* find(double hz, double tol) {
        Chan* best = nullptr;
        double bd = tol;
        for (Chan& c : chans) if (std::fabs(c.f - hz) <= bd) { bd = std::fabs(c.f - hz); best = &c; }
        return best;
    }

    Chan& add(double hz, bool manual) {
        Chan c;
        c.id = nextId++;
        c.manual = manual;
        c.setFreq(hz); c.f0 = hz;
        c.mf.assign((size_t)(4 * sps), cf32(0, 0));
        c.bins.assign((size_t)sps, 0.f);
        c.yh.assign((size_t)sps, cf32(0, 0));
        c.firstSec = c.lastActive = c.lastPeak = sigSec();
        seed(c, hz);
        chans.push_back(std::move(c));
        return chans.back();
    }

    // the smoothed held spectrum at bin i (over the signal's width)
    float smooth(const std::vector<float>& v, int i, int w) const {
        float s = 0; int n = 0;
        for (int j = std::max(0, i - w); j <= std::min((int)v.size() - 1, i + w); j++) { s += v[(size_t)j]; n++; }
        return n ? s / n : 0.f;
    }

    void spectrumFrame() {
        std::vector<cf32> b(kN);
        for (int i = 0; i < kN; i++) b[(size_t)i] = fftIn[(size_t)i] * win[(size_t)i];
        fft.forward(b.data());
        const int W = kHiBin - kLoBin + 1;
        for (int i = 0; i < W; i++) {
            const float p = std::norm(b[(size_t)(kLoBin + i)]) * (4.f / ((float)kN * kN));
            float& hh = held[(size_t)i];
            hh += (p > hh ? 0.5f : 0.05f) * (p - hh);
            acc[(size_t)i] += p;
        }
        accN++;
        std::vector<float> srt(held);
        std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
        floorLin = srt[(size_t)W / 2];
        if (++frames < 3) return;
        const int w = std::max(1, (int)std::lround(0.6 * baud() / kBinHz));
        std::vector<float> sm((size_t)W);
        for (int i = 0; i < W; i++) sm[(size_t)i] = smooth(held, i, w);
        const float thr = floorLin * std::pow(10.f, thrDb.load() / 10.f);
        const double now = sigSec();
        const int sep = std::max(2, (int)std::lround(1.5 * baud() / kBinHz));
        for (int i = 1; i < W - 1; i++) {
            const float v = sm[(size_t)i];
            if (v < thr) continue;
            bool top = true;
            for (int j = std::max(0, i - sep); j <= std::min(W - 1, i + sep) && top; j++) if (j != i && (sm[(size_t)j] > v || (sm[(size_t)j] == v && j < i))) top = false;
            if (!top) continue;
            const double a = sm[(size_t)i - 1], bb = v, c = sm[(size_t)i + 1];
            const double den = a - 2 * bb + c;
            const double d = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
            const double hz = (kLoBin + i + d) * kBinHz;
            // the IMD products and keying sidebands of a much stronger signal are not signals of their own
            bool skirt = false;
            const int span = (int)(3 * baud() / kBinHz);
            for (int j = std::max(0, i - span); j <= std::min(W - 1, i + span); j++) if (sm[(size_t)j] > 300.f * v) skirt = true;
            if (Chan* ch = find(hz, 0.8 * baud())) {
                ch->lastPeak = now;
                if (!ch->manual && !ch->dcd) { ch->setFreq(ch->f + 0.1 * (hz - ch->f)); ch->f0 = ch->f; }
            } else if (!skirt && !find(hz, 1.4 * baud()) && (int)chans.size() < kPskMaxSignals) {
                replay(add(hz, false));
            }
        }
        for (const Chan& c : chans) if (c.id != focusId && now - std::max(c.lastActive, c.lastPeak) > 40) retire(c);
        chans.erase(std::remove_if(chans.begin(), chans.end(), [&](const Chan& c) {
            return c.id != focusId && now - std::max(c.lastActive, c.lastPeak) > 40; }), chans.end());
        for (size_t i = 0; i < chans.size(); i++)
            for (size_t j = i + 1; j < chans.size();) {
                if (std::fabs(chans[i].f - chans[j].f) < 0.5 * baud() && chans[j].id != focusId && chans[j].chars <= chans[i].chars) chans.erase(chans.begin() + (long)j);
                else if (std::fabs(chans[i].f - chans[j].f) < 0.5 * baud() && chans[i].id != focusId && chans[i].chars < chans[j].chars) { chans.erase(chans.begin() + (long)i); j = i + 1; }
                else j++;
            }
    }

    void putChar(Chan& c, int ch) {
        if (ch == -2) { c.bad++; return; }
        c.chars++;
        c.lastActive = sigSec();
        if (ch == '\r' || ch == '\n') {
            c.lineOpen = false;
            if (!c.text.empty() && c.text.back() != '\n') c.text += '\n';
        } else {
            if (ch == '\t') ch = ' ';
            if (ch < 32 || ch > 126) return;
            if (!c.lineOpen) {
                PskLine l; l.timeSec = sigSec();
                c.lines.push_back(l);
                if (c.lines.size() > kPskLinesMax) c.lines.erase(c.lines.begin());
                c.lineOpen = true;
            }
            c.lines.back().text += (char)ch;
            c.text += (char)ch;
        }
        if (c.text.size() > kPskTextMax) c.text.erase(0, c.text.size() - kPskTextMax);
        if (ch == ' ' || ch == '\r' || ch == '\n') {
            std::string flat = c.text;
            std::replace(flat.begin(), flat.end(), '\n', ' ');
            const std::vector<std::string> now = cw::callsigns(flat);
            if (!trial && PskReporter::get().want())
                for (const std::string& k : now)
                    if (std::find(c.calls.begin(), c.calls.end(), k) == c.calls.end() && pskr::sendsInText(flat, k))
                        PskReporter::get().spot(mode == 0 ? "PSK31" : mode == 1 ? "PSK63" : mode == 4 ? "PSK125" : mode == 5 ? "PSK250" : mode == 7 ? "8PSK125" : mode == 8 ? "8PSK250" : "PSK", c.f + spotShift.load(), (float)std::max(-5.0, std::min(40.0, 10 * std::log10(1.0 / std::max(1e-4f, c.errShown)))), k);
            c.calls = now;
        }
    }

    // one symbol of a signal
    void symbol(Chan& c, std::complex<float> y) {
        if (!c.havePrev) { c.prevY = y; c.havePrev = true; return; }
        const std::complex<float> d = y * std::conj(c.prevY);
        c.prevY = y;
        if (std::abs(d) < 1e-20f) return;
        const double a = std::arg(d);
        const bool qp = pskQpsk(mode), p8 = psk8psk(mode);
        // 8PSK: the AFC of the eighth power only pulls in +-baud / 16 and locks just as well one eighth turn per symbol off, so it acquires on
        // the preamble's reversals with the square (as BPSK, +-baud / 4) and changes to the eighth power once the DCD is on
        const int order = p8 ? (c.dcd ? 8 : 2) : qp ? 4 : 2;
        const double err = std::arg(std::pow(std::complex<double>(d), order)) / order;
        // quality: 1 for clean decisions, 0 for noise (the phase error spread over its whole range)
        const double en = err / (kPi / order);
        c.q += 0.1f * ((float)(1 - 3 * en * en) - c.q);
        c.errVar += 0.05f * ((float)(err * err) - c.errVar);
        const float old = c.errHist[c.errPos];
        c.errHist[c.errPos] = c.errVar;
        c.errPos = (c.errPos + 1) & 31;
        if (c.chars == 0) c.errShown = c.errVar;
        else if (c.dcd && old > 0) c.errShown = old;
        const float yp = std::norm(y);
        c.fastPw += kSqlA * (yp - c.fastPw);
        if (c.dcd) c.dcdPw += 0.02f * (yp - c.dcdPw); else c.dcdPw = c.fastPw;
        const bool gone = kPskSquelch && c.dcd && c.fastPw < kSqlT * c.dcdPw;
        if (!c.dcd && c.q > 0.5f) c.dcd = true;
        else if (c.dcd && (c.q < 0.25f || gone)) {
            c.dcd = false;
            c.q = std::min(c.q, 0.2f);
            c.lineOpen = false;
            c.vrx.reset();
            c.mrx.reset();
        }
        // AFC: the remaining phase step per symbol is the frequency error
        const double ferr = err * baud() / (2 * kPi);
        const double gain = (c.dcd ? 0.04 : 0.1) * std::min(1.0, 62.5 / baud());   // the faster modes: the same loop bandwidth in Hz as PSK63 (the decimator delay is a large part of their symbol)
        if ((!c.manual || c.dcd) && c.q > 0.2f) {
            double nf = c.f + gain * ferr;
            if (std::fabs(nf - c.f0) > 2 * baud()) nf = c.f0;   // pulled away: start over where it was found
            c.setFreq(nf);
        }
        if (p8) {
            const int e8 = (int)std::lround(a / (kPi / 4));
            const int sym = psk::psk8Symbol(((e8 % 8) + 8) % 8);
            c.run4 = sym == 4 ? c.run4 + 1 : 0;
            if (c.dcd) { p8All++; if (sym & 1) p8Odd++; }
            if (c.run4 >= 4) c.mrx.reset();   // the steady carrier would read as spaces
            else if (c.dcd)
                for (int b = 0; b < 3; b++) { const int ch = c.mrx.bit((sym >> b) & 1); if (ch >= 0 && ch != 0) putChar(c, ch); else if (ch == -2) putChar(c, -2); }
        } else if (qp) {
            float cq[4];
            for (int k = 0; k < 4; k++) cq[k] = (float)std::cos(a - k * kPi / 2);
            float b1[2] = {-2, -2}, b0[2] = {-2, -2};   // best correlation with bit 0 = 0/1, bit 1 = 0/1
            for (int k = 0; k < 4; k++) {
                const int sym = psk::qpskSymbol(k);
                b0[sym & 1] = std::max(b0[sym & 1], cq[k]);
                b1[(sym >> 1) & 1] = std::max(b1[(sym >> 1) & 1], cq[k]);
            }
            const int bit = c.vit.decode(b0[1] - b0[0], b1[1] - b1[0]);
            if (bit >= 0 && c.dcd) { const int ch = c.vrx.bit(bit); if (ch != -1) putChar(c, ch); }
        } else {
            const int bit = d.real() > 0 ? 1 : 0;
            if (c.dcd) { const int ch = c.vrx.bit(bit); if (ch != -1) putChar(c, ch); }
        }
        if (c.lineOpen && sigSec() - c.lastActive > 5) c.lineOpen = false;
    }

    // one 1 kHz sample of a signal: matched filter, symbol clock
    void sample1k(Chan& c, cf32 d) {
        const int L = (int)h.size();
        c.mf[(size_t)c.mfPos] = d; c.mf[(size_t)(c.mfPos + L)] = d;
        if (++c.mfPos >= L) c.mfPos = 0;
        const cf32* w = &c.mf[(size_t)c.mfPos];
        cf32 y(0, 0);
        for (int j = 0; j < L; j++) y += w[j] * h[(size_t)j];
        const float e = std::abs(y);
        float& bn = c.bins[(size_t)c.ph];
        bn += (c.syms < sps ? 0.3f : 0.08f) * (e - bn);
        c.yh[(size_t)c.yhPos] = y;
        c.yhPos = (c.yhPos + 1) % sps;
        c.since++;
        if (c.ph == c.idx && c.since > sps / 2) {
            c.since = 0;
            const cf32 mid = c.yh[(size_t)((c.yhPos - 1 - sps / 2 + 2 * sps) % sps)];
            c.pw += 0.05f * (std::norm(y) - c.pw);
            // Gardner: (previous - this) x the sample between them; > 0 when the samples come too early
            const float ge = std::real((c.lastSym - y) * std::conj(mid)) / std::max(1e-30f, c.pw);
            c.lastSym = y;
            if (pskQpsk(mode) || psk8psk(mode) || kPskIsiBpsk) {
                // the matched filter leaves 1/6 of each neighbour in a symbol (the PSK31 pulse is two symbols long): harmless for BPSK, where
                // it only changes the amplitude, but it turns QPSK phases, so take it out (one symbol late)
                c.sym2 = c.sym1; c.sym1 = y;
                symbol(c, c.sym2 * (1.f + 2.f / 36) - (1.f / 6) * (c.lastSym2 + y));
                c.lastSym2 = c.sym2;
            } else {
                symbol(c, y);
            }
            int best = 0;
            for (int k = 1; k < sps; k++) if (c.bins[(size_t)k] > c.bins[(size_t)best]) best = k;
            if (!c.dcd && c.syms < 3 * sps) { c.idx = best; c.gacc = 0; }   // acquiring: straight to the peak of the envelope
            else {
                c.gacc += ge;
                if (c.gacc > 2.f) { c.idx = (c.idx + 1) % sps; c.gacc = 0; }
                else if (c.gacc < -2.f) { c.idx = (c.idx + sps - 1) % sps; c.gacc = 0; }
            }
            c.syms++;
        }
        if (++c.ph >= sps) c.ph = 0;
    }

    void process(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            fftIn.push_back(x[i]);
            if (fftIn.size() >= (size_t)kN) { spectrumFrame(); fftIn.erase(fftIn.begin(), fftIn.begin() + kHop); }
        }
        for (size_t i = 0; i < n; i++) {
            const cf32 v = x[i];
            nBase++;
            for (Chan& c : chans) chanSample(c, v);
            hist[histPos] = v;
            if (++histPos >= hist.size()) histPos = 0;
        }
    }
    void chanSample(Chan& c, cf32 v) {
        const std::complex<double> m = std::complex<double>(v.real(), v.imag()) * c.rot;
        c.rot *= c.step;
        if (++c.rotCount >= 4096) { c.rot /= std::abs(c.rot); c.rotCount = 0; }
        const cf32 mf((float)m.real(), (float)m.imag());
        c.tri[c.triPos] = mf; c.tri[c.triPos + kTri] = mf;
        if (++c.triPos >= kTri) c.triPos = 0;
        if (++c.phase < kR) return;
        c.phase = 0;
        const cf32* w = &c.tri[c.triPos];
        cf32 d(0, 0);
        for (int j = 0; j < kTri; j++) d += w[j] * triW[j];
        sample1k(c, d);
    }
    // a signal found in the spectrum: the last seconds of the band go through its new receiver first, so that it locks during the idle
    // reversals before the text instead of losing the first words
    void replay(Chan& c) {
        if (!kPskReplay || hist.empty()) return;
        for (size_t k = 0; k < hist.size(); k++) chanSample(c, hist[(histPos + k) % hist.size()]);
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.floorDb = (float)(10 * std::log10(floorLin + 1e-20));
        tel.mode = mode;
        tel.autoOn = autoOn.load(); tel.autoLocked = autoLocked; tel.autoMode = autoFound;
        if (accN > 0) {
            tel.spectrum.resize(acc.size());
            for (size_t i = 0; i < acc.size(); i++) { tel.spectrum[i] = (float)(10 * std::log10(acc[i] / accN + 1e-20)); acc[i] = 0; }
            accN = 0;
            tel.specSeq++;
        }
        tel.signals.clear();
        uint64_t ok = 0, bad = 0;
        int decoding = 0;
        float best = -100;
        tel.cfoHz = 0;
        for (const Chan& c : chans) {
            PskSignal s;
            s.id = c.id; s.offsetHz = c.f; s.afcHz = c.f - c.f0; s.manual = c.manual;
            s.snrDb = (float)std::max(-5.0, std::min(40.0, 10 * std::log10(1.0 / std::max(1e-4f, c.errShown)))); s.quality = std::max(0.f, c.q); s.dcd = c.dcd;
            s.lastActiveSec = c.lastActive; s.firstSec = c.firstSec;
            s.chars = c.chars; s.text = c.text; s.lines = c.lines; s.calls = c.calls;
            ok += c.chars; bad += c.bad;
            if (c.dcd) decoding++;
            if (c.id == focusId || (focusId < 0 && s.snrDb > best)) { tel.snrDb = s.snrDb; best = s.snrDb; if (c.id == focusId) tel.cfoHz = s.afcHz; }
            tel.signals.push_back(std::move(s));
        }
        for (const PskSignal& k : kept) tel.signals.push_back(k);
        std::sort(tel.signals.begin(), tel.signals.end(), [](const PskSignal& a, const PskSignal& b) { return a.offsetHz < b.offsetHz; });
        tel.blocksOk = std::max(tel.blocksOk, ok); tel.blocksBad = std::max(tel.blocksBad, bad);
        tel.focusId = focusId;
        tel.state = decoding ? 2 : chans.empty() ? 0 : 1;
        tel.dataValid = decoding > 0;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};

PskReceiver::PskReceiver() : p_(std::make_unique<Impl>()) {}
PskReceiver::~PskReceiver() = default;

void PskReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void PskReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool PskReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= pskTuning().minSampleRate - 1; }
void PskReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    std::vector<PskSignal> sig = std::move(p_->pub.signals);   // the text stays (clearResults)
    p_->pub = PskTelemetry();
    p_->pub.seq = s;
    p_->pub.signals = std::move(sig);
}

void PskReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.wideChanged.exchange(false)) m.resetReq = true;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.front.setOffsetHz(m.offsetHz);
        if (m.wide) m.wide->dec.setOffsetHz(m.offsetHz);
    }
    if (m.clearReq.exchange(false)) {
        m.chans.clear(); m.focusId = -1; m.kept.clear();
        if (m.wide) for (auto& r : m.wide->subs) r->clearSignals();
    }
    m.takeKept();
    if (m.modeReq.load() != m.mode) {
        m.setupMode(m.modeReq.load());
        if (m.wide) for (auto& r : m.wide->subs) r->setMode(m.modeReq.load());
    }
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.focusReq) {
            m.focusReq = false;
            if (m.wide) {
                int best = 0;
                const double wantC = m.focusHz - Impl::kSliceLo - 0.5 * Impl::kSliceStep;
                for (size_t i = 0; i < m.wide->centre.size(); i++)
                    if (std::fabs(m.wide->centre[i] - wantC) < std::fabs(m.wide->centre[(size_t)best] - wantC)) best = (int)i;
                m.wide->focusSlice = best;
                m.wide->subs[(size_t)best]->setFocusHz(m.focusHz - m.wide->centre[(size_t)best]);
            }
            Chan* c = m.wide ? nullptr : m.find(m.focusHz, 0.8 * m.baud());
            if (!c && !m.wide && (int)m.chans.size() < kPskMaxSignals + 4) c = &m.add(m.focusHz, true);
            if (c) m.focusId = c->id;
            for (Chan& o : m.chans) if (o.manual && o.id != m.focusId && o.chars == 0) o.manual = false;
        }
    }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running && m.wide) {
        m.wide->buf.clear();
        m.wide->dec.process(x, n, m.wide->buf);
        for (cf32& v : m.wide->buf) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        for (auto& r : m.wide->subs) r->feed(m.wide->buf.data(), m.wide->buf.size());
    } else if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.listen.playBase(m.base.data(), m.base.size());   // a copy to the sound card only while listening
        m.process(m.base.data(), m.base.size());
        m.autoFeed(m.base.data(), m.base.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        if (m.wide) m.reportWide(); else m.report();
    }
}

std::string PskReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.signals);
}
bool PskReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<PskSignal> sig;
    if (!results::unpack(blob, t0, sig)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<PskSignal> in;
    for (auto& s : sig) {
        if (s.chars == 0) continue;
        PskSignal k;
        k.id = s.id; k.offsetHz = s.offsetHz; k.lastActiveSec = s.lastActiveSec; k.firstSec = s.firstSec;
        k.chars = s.chars; k.text = s.text; k.calls = s.calls; k.lines = s.lines;
        Impl::shiftSig(k, p_->pub.timeSec - t0);
        in.push_back(std::move(k));
    }
    if (in.size() > Impl::kKeptMax) in.erase(in.begin(), in.end() - (long)Impl::kKeptMax);
    p_->pub.signals = in;
    p_->keptIn = std::move(in); p_->keptInAt = p_->pub.timeSec;
    p_->keptLoad = true; p_->keptClear = false;
    p_->pub.seq++;
    return true;
}
void PskReceiver::clearResults() { clearSignals(); }
bool PskReceiver::telemetry(PskTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void PskReceiver::setListen(bool on) { p_->listen.on = on; }
bool PskReceiver::listening() const { return p_->listen.on.load(); }
void PskReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void PskReceiver::setMuted(bool m) { p_->listen.muted = m; }
void PskReceiver::setSilent(bool s) { p_->listen.silent = s; }
void PskReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void PskReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
// One modulation over the band: the good characters less the bad codes, zero when it is not clearly better than nothing
uint64_t PskReceiver::Impl::trialMode(const std::vector<cf32>& band, int mode, float thr) {
    auto t = std::make_unique<Impl>();
    t->trial = true;
    t->thrDb = thr;
    t->modeReq = mode;
    { std::lock_guard<std::mutex> lk(t->mu); t->rate = pskTuning().minSampleRate; }
    t->resetState();
    t->resetReq = false;
    for (size_t i = 0; i < band.size(); i += 2400) t->process(band.data() + i, std::min<size_t>(2400, band.size() - i));
    uint64_t ok = 0, bad = 0;
    t->report();
    ok = t->tel.blocksOk; bad = t->tel.blocksBad;
    if (!(ok >= 16 && ok >= 4 * bad)) return 0;
    // a BPSK or QPSK signal read as 8PSK gives text too (its steps are 8PSK symbols), but never an odd eighth turn
    if (psk8psk(mode) && (t->p8All < 64 || t->p8Odd * 5 < t->p8All)) return 0;
    // a too-fast modulation still produces characters from a slower signal (the short codes of noise); only text that reads counts
    std::string text;
    for (const PskSignal& s : t->tel.signals) if (s.chars > 0) text += s.text;
    const double r = autoparam::readableFraction(text.substr(text.size() / 3));
    if (r < 0.85) return 0;
    return (uint64_t)(1000.0 * (double)(ok - 3 * bad) * r * r / pskBaud(mode));   // per baud: a too-fast decoder runs off short codes, its text per symbol is what is low
}
std::pair<int, uint64_t> PskReceiver::Impl::trialAll(const std::vector<cf32>& band, float thr) {
    int best = -1;
    uint64_t bestS = 0;
    for (int m = 0; m < kPskModes; m++) {
        const uint64_t s = trialMode(band, m, thr);
        if (s > bestS) { bestS = s; best = m; }
    }
    return {best, bestS};
}
// The Auto rule: no text for a while and something in the window: try every modulation. A band that keeps giving text is left alone.
void PskReceiver::Impl::autoFeed(const cf32* x, size_t n) {
    if (autoKick.exchange(false)) { autoLocked = false; autoLastRun = 0; autoFill = 0; autoPos = 0; autoCharsSeen = tel.blocksOk; autoGoodAt = autoSamples; }
    if (!autoOn.load()) { if (autoJob.valid()) autoDiscard = true; else return; }
    const size_t cap = (size_t)(10 * kBase);
    if (autoRing.size() != cap) autoRing.assign(cap, cf32(0, 0));
    for (size_t i = 0; i < n; i++) { autoRing[autoPos] = x[i]; if (++autoPos >= cap) autoPos = 0; }
    autoFill = std::min(cap, autoFill + n);
    autoSamples += n;
    if (tel.blocksOk > autoCharsSeen) {
        autoCharsSeen = tel.blocksOk;
        std::string t;   // the newest text of the band: garbage keeps the counter running, so the count alone does not say the lock is good
        for (const PskSignal& s : tel.signals) if (s.chars > 0) t += s.text.substr(s.text.size() > 80 ? s.text.size() - 80 : 0);
        if (t.size() < 24 || autoparam::readableFraction(t) >= 0.8) autoGoodAt = autoSamples;
    }
    if (autoJob.valid()) {
        if (autoJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const auto r = autoJob.get();
        const bool drop = autoDiscard || !autoOn.load();
        autoDiscard = false;
        if (drop) return;
        if (r.first < 0) { if (!autoLocked) autoFound = -1; autoCand = -1; return; }
        // while locked, a different answer must repeat before the decode is moved: a burst of noise must not throw away a working lock
        if (autoLocked && r.first != autoFound) {
            if (autoCand == r.first) { autoCand = -1; autoFound = r.first; autoGoodAt = autoSamples; modeReq = r.first; }
            else autoCand = r.first;
            return;
        }
        autoCand = -1;
        autoLocked = true; autoFound = r.first;
        autoGoodAt = autoSamples;
        modeReq = r.first;
        return;
    }
    if (autoLocked) {
        if (autoSamples - autoGoodAt >= (uint64_t)(10 * kBase)) { autoLocked = false; autoLastRun = 0; }
        else if (autoSamples - autoLastRun < (uint64_t)(10 * kBase)) return;   // a working decode is re-checked now and then: a slower modulation can still give letters
    }
    if (autoSamples < autoLastRun + (uint64_t)(4 * kBase) || autoFill < (size_t)(6 * kBase)) return;
    if (chans.empty()) return;   // nothing in the window: the trial would have nothing to read
    autoLastRun = autoSamples;
    const size_t take = std::min(autoFill, (size_t)(8 * kBase));
    std::vector<cf32> a(take);
    const size_t start = (autoPos + cap - take) % cap;
    for (size_t i = 0; i < take; i++) a[i] = autoRing[(start + i) % cap];
    const float thr = thrDb.load();
    autoJob = std::async(std::launch::async, [v = std::move(a), thr]() { return trialAll(v, thr); });
}
void PskReceiver::setAuto(bool on) { if (on && !p_->autoOn.load()) p_->autoKick = true; p_->autoOn = on; }
bool PskReceiver::autoOn() const { return p_->autoOn.load(); }
void PskReceiver::setMode(int m) { p_->modeReq = std::max(0, std::min(kPskModes - 1, m)); }
void PskReceiver::setFocusHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->focusReq = true; p_->focusHz = hz; }
void PskReceiver::clearSignals() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.signals.clear(); p_->keptLoad = false; p_->keptIn.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}
void PskReceiver::setThresholdDb(float db) { p_->thrDb = std::max(4.f, std::min(30.f, db)); }

ModeTuning pskTuning() {
    ModeTuning t;
    t.stdMode = 38; t.id = "psk"; t.name = "PSK31";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.070;   // the 20 m PSK31 dial frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const PskPreset* pskPresets(int& count) {
    static const PskPreset kP[] = {{"80 m", 3.580}, {"40 m", 7.040}, {"40 m", 7.070}, {"30 m", 10.142}, {"20 m", 14.070}, {"17 m", 18.100}, {"15 m", 21.070}, {"10 m", 28.120}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2

namespace dect2 {

void PskReceiver::Impl::reportWide() {
    Impl& m = *this;
    auto& W = *m.wide;
    m.tel.seq++;
    m.tel.timeSec = m.curRate > 0 ? (double)m.nIn / m.curRate : 0;
    m.tel.levelDb = m.nPower ? (float)(10 * std::log10(m.power / (double)m.nPower + 1e-20)) : -200.f;
    m.power = 0; m.nPower = 0;
    m.tel.inputRate = m.curRate;
    m.tel.mode = m.mode;
    m.tel.signals.clear();
    std::vector<float> spec;
    uint64_t ok = 0, bad = 0;
    int decoding = 0;
    float floorSum = 0; int nFloor = 0;
    m.tel.focusId = -1; m.tel.cfoHz = 0;
    for (size_t i = 0; i < W.subs.size(); i++) {
        PskTelemetry t;
        if (!W.subs[i]->telemetry(t, 0)) continue;
        for (PskSignal& s : t.signals) {
            if (s.offsetHz < kSliceLo || s.offsetHz >= kSliceHi) continue;
            s.id += (int)i * 100000;
            s.offsetHz += W.centre[i];
            ok += s.chars;
            if (s.dcd) decoding++;
            m.tel.signals.push_back(std::move(s));
        }
        bad += t.blocksBad;
        if (t.floorDb > -190) { floorSum += t.floorDb; nFloor++; }
        if ((int)i == W.focusSlice && t.focusId >= 0) { m.tel.focusId = t.focusId + (int)i * 100000; m.tel.cfoHz = t.cfoHz; }
        const size_t b0 = (size_t)std::max(0.0, (kSliceLo - t.loHz) / kBinHz), b1 = (size_t)std::max(0.0, (kSliceHi - t.loHz) / kBinHz);
        for (size_t b = b0; b < b1 && b < t.spectrum.size(); b++) spec.push_back(t.spectrum[b]);
    }
    if (!spec.empty()) {
        const size_t P = (spec.size() + 3999) / 4000;
        m.tel.spectrum.assign((spec.size() + P - 1) / P, -200.f);
        for (size_t b = 0; b < spec.size(); b++) m.tel.spectrum[b / P] = std::max(m.tel.spectrum[b / P], spec[b]);
        m.tel.specSeq++;
    }
    m.tel.loHz = W.centre.front() + kSliceLo; m.tel.hiHz = W.centre.back() + kSliceHi;
    m.tel.floorDb = nFloor ? floorSum / (float)nFloor : -200.f;
    m.tel.snrDb = -100;
    for (const PskSignal& s : m.tel.signals) if (s.id == m.tel.focusId || (m.tel.focusId < 0 && s.snrDb > m.tel.snrDb)) m.tel.snrDb = s.snrDb;
    if (m.tel.signals.empty()) m.tel.snrDb = 0;
    for (const PskSignal& k : m.kept) m.tel.signals.push_back(k);
    std::sort(m.tel.signals.begin(), m.tel.signals.end(), [](const PskSignal& a, const PskSignal& b) { return a.offsetHz < b.offsetHz; });
    m.tel.blocksOk = std::max(m.tel.blocksOk, ok); m.tel.blocksBad = std::max(m.tel.blocksBad, bad);
    m.tel.state = decoding ? 2 : m.tel.signals.empty() ? 0 : 1;
    m.tel.dataValid = decoding > 0;
    std::lock_guard<std::mutex> lk(m.mu);
    m.pub = m.tel;
}

void PskReceiver::setWide(bool on, double spanHz) {
    p_->wideSpanReq = std::max(1000.0, std::min(wideSpanCapHz(), spanHz));
    if (p_->wideReq.exchange(on) != on) p_->wideChanged = true;
}
bool PskReceiver::wide() const { return p_->wideReq.load(); }
void PskReceiver::setSpotShiftHz(double hz) { p_->spotShift = hz; }

} // namespace dect2
