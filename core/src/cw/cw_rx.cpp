// CW (Morse) skimmer (see cw_rx.h).
#include "dect2/cw/cw_rx.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/results/results_io.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/net/pskreporter.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/engine/widespan.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <mutex>
#include <utility>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kBase = marine::kBaseRate;      // 24 kHz complex
constexpr int kN = 2048, kHop = 1024;            // window spectrum
constexpr double kBinHz = kBase / kN;
constexpr double kWindowHz = 7000;               // the front end passes 8 kHz each side
constexpr int kHalf = (int)(kWindowHz / kBinHz);
constexpr int kR = 24;                           // 24 kHz -> 1 kHz
constexpr int kTri = 2 * kR - 1;                 // triangular (CIC of order 2) decimation filter
constexpr int kFir = 65;                         // +-50 Hz low-pass at 1 kHz
constexpr int kToneRate = 8000;   // the sidetone goes through hfdig::Listen (8 kHz in)

std::vector<float> lowpass1k() {
    std::vector<float> h(kFir);
    const double fc = 50.0 / 1000.0;
    double s = 0;
    for (int i = 0; i < kFir; i++) {
        const double m = i - (kFir - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (kFir - 1)) + 0.08 * std::cos(4 * kPi * i / (kFir - 1));
        h[(size_t)i] = (float)(sinc * w);
        s += sinc * w;
    }
    for (float& v : h) v = (float)(v / s);
    return h;
}

struct Chan {
    int id = 0;
    double f = 0;                // offset from the dial, Hz
    bool manual = false;
    std::complex<double> rot{1, 0}, step{1, 0};
    int rotCount = 0;
    cf32 tri[2 * kTri] = {};     // the mixed samples, twice so a window never wraps
    int triPos = 0, phase = 0;
    cf32 fir[2 * kFir] = {};
    int firPos = 0;
    // detector
    bool init = false, key = false, raw = false;
    float hi = 0, lo = 0;
    cw::Timing tm;
    cw::Keyer keyer;
    std::string text;
    std::vector<std::string> calls;
    double firstSec = 0, lastActive = 0, lastPeak = 0;
    uint64_t chars = 0, bad = 0;
    float tone = 0;              // sidetone envelope
    void setFreq(double hz) {
        f = hz;
        step = std::polar(1.0, -2 * kPi * hz / kBase);
    }
};

} // namespace

struct CwReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    CwTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    bool focusReq = false; double focusHz = 0;   // under mu
    std::atomic<float> thrDb{10.f}, toneHz{700.f};
    hfdig::Listen listen;          // the sidetone (the focused signal keying a tone) on the sound card: "Listen"
    // receiver thread
    CwTelemetry tel;
    double curRate = 0;
    bool running = false;
    int64_t nIn = 0, nBase = 0;
    double nextReport = 0;
    double power = 0; int64_t nPower = 0;
    marine::Front front;
    std::vector<cf32> base;
    std::vector<cf32> fftIn;     // samples waiting for the spectrum
    Fft fft{kN};
    std::vector<float> win, held, acc;
    int accN = 0, frames = 0;
    float floorLin = 0;
    std::vector<float> lp = bbf::overrideTaps("cw", lowpass1k(), 1000.0, 100.0, kFir, kFir);   // 65 taps always: the delay lines are that long
    float triW[kTri] = {};
    std::vector<Chan> chans;
    int nextId = 1, focusId = -1;
    std::vector<float> toneBuf;
    double tonePh = 0;
    // whole span
    std::atomic<bool> wideReq{false}, wideChanged{false};
    std::atomic<double> wideSpanReq{250000};
    std::atomic<double> spotShift{0};
    struct Wide {
        WideDecimator dec;
        std::vector<std::unique_ptr<CwReceiver>> subs;
        std::vector<double> centre;
        std::vector<cf32> buf;
        int focusSlice = -1;
    };
    std::unique_ptr<Wide> wide;
    static constexpr double kSlice = 12000;
    void buildWide() {
        wide.reset();
        if (!wideReq.load() || !running) return;
        auto w = std::make_unique<Wide>();
        w->dec.configure(curRate, wideSpanReq.load());
        w->dec.setOffsetHz(offsetNow());
        const int half = (int)std::ceil((w->dec.spanHz() / 2 - kSlice / 2) / kSlice);
        for (int k = -half; k <= half; k++) {
            auto r = std::make_unique<CwReceiver>();
            r->setSilent(true);
            r->setSpotShiftHz(k * kSlice);
            r->setThresholdDb(thrDb.load());
            r->configure(w->dec.outRate());
            r->setSignalOffset(k * kSlice);
            w->centre.push_back(k * kSlice);
            w->subs.push_back(std::move(r));
        }
        wide = std::move(w);
    }
    double offsetNow() { std::lock_guard<std::mutex> lk(mu); return offsetHz; }

    Impl() {
        win.resize(kN);
        for (int i = 0; i < kN; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kN));
        for (int j = 0; j < kTri; j++) triW[j] = (float)((kR - std::abs(j - (kR - 1))) / (double)(kR * kR));
    }

    // signals that are gone (silent too long, a change of mode, a reset) keep their text until Clear: listed with the live ones (not
    // decoding), and a new signal found where one of them was carries its text on. Feed thread; Clear and a loaded file reach them through
    // these requests (mu)
    std::vector<CwSignal> kept;       // oldest first, at most kKeptMax
    static constexpr size_t kKeptMax = 2 * kCwMaxSignals;
    bool keptClear = false, keptLoad = false;
    std::vector<CwSignal> keptIn;
    double keptInAt = 0;
    static void shiftSig(CwSignal& s, double dt) { s.lastActiveSec += dt; s.firstSec += dt; }
    void retire(const Chan& c) {
        if (c.chars == 0) return;
        CwSignal s;
        s.id = c.id; s.offsetHz = c.f;
        s.lastActiveSec = c.lastActive; s.firstSec = c.firstSec;
        s.chars = c.chars; s.text = c.text; s.calls = c.calls;
        kept.push_back(std::move(s));
        if (kept.size() > kKeptMax) kept.erase(kept.begin());
    }
    void seed(Chan& c, double hz) {   // a new signal where a gone one was: its text goes on
        for (size_t i = 0; i < kept.size(); i++) {
            const CwSignal& k = kept[i];
            if (std::fabs(k.offsetHz - hz) > 60) continue;
            c.id = k.id; c.text = k.text; c.calls = k.calls; c.chars = k.chars; c.firstSec = k.firstSec;
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
    void reportWide();
    double sigSec() const { return (double)nBase / kBase; }

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
        tel = CwTelemetry();
        tel.seq = s; tel.specSeq = ss;
        tel.inputRate = curRate;
        tel.windowHz = kHalf * kBinHz;
        nIn = nBase = 0; nextReport = 0; power = 0; nPower = 0;
        running = curRate >= cwTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        base.clear(); fftIn.clear();
        held.assign(2 * kHalf + 1, 0.f); acc.assign(2 * kHalf + 1, 0.f); accN = 0; frames = 0; floorLin = 0;
        chans.clear(); focusId = -1;
        listen.reset(); toneBuf.clear();
        buildWide();
        if (cb && curRate > 0 && wide) cb("CW: skimming the whole span (" + std::to_string((int)(wide->dec.spanHz() / 1000)) + " kHz)");
        else if (cb && curRate > 0) cb(running ? "CW: skimming +-7 kHz around the dial frequency" : "CW: the sample rate is too low (250 kHz at least)");
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
        c.setFreq(hz);
        c.firstSec = c.lastActive = c.lastPeak = sigSec();
        seed(c, hz);
        chans.push_back(c);
        return chans.back();
    }

    // the keying sidebands of a strong signal: a peak within 500 Hz of one 30 dB stronger is not a signal of its own
    bool skirt(int i, float v) const {
        const int span = (int)(500 / kBinHz), W = (int)held.size();
        for (int j = std::max(0, i - span); j < std::min(W, i + span + 1); j++) if (held[(size_t)j] > 1000.f * v) return true;
        return false;
    }

    // one row of the window spectrum: carriers found, signals added and refined
    void spectrumFrame() {
        std::vector<cf32> b(kN);
        for (int i = 0; i < kN; i++) b[(size_t)i] = fftIn[(size_t)i] * win[(size_t)i];
        fft.forward(b.data());
        const int W = 2 * kHalf + 1;
        std::vector<float> p(W);
        for (int k = -kHalf; k <= kHalf; k++) p[(size_t)(k + kHalf)] = std::norm(b[(size_t)((k + kN) % kN)]) * (4.f / ((float)kN * kN));
        for (int i = 0; i < W; i++) {
            float& h = held[(size_t)i];
            h += (p[(size_t)i] > h ? 0.6f : 0.04f) * (p[(size_t)i] - h);
            acc[(size_t)i] += p[(size_t)i];
        }
        accN++;
        std::vector<float> srt(held);
        std::nth_element(srt.begin(), srt.begin() + W / 2, srt.end());
        floorLin = srt[(size_t)W / 2];
        if (++frames < 12) return;   // the first half second: the front end and the held spectrum settle
        const float thr = floorLin * std::pow(10.f, thrDb.load() / 10.f);
        const double now = sigSec();
        constexpr int kSep = 5;   // a peak is the highest within +-58 Hz
        for (int i = kSep; i < W - kSep; i++) {
            const float v = held[(size_t)i];
            if (v < thr) continue;
            bool top = true;
            for (int j = i - kSep; j <= i + kSep && top; j++) if (j != i && held[(size_t)j] > v) top = false;
            if (!top) continue;
            const double a = 10 * std::log10(held[(size_t)i - 1] + 1e-20), bb = 10 * std::log10(v + 1e-20), c = 10 * std::log10(held[(size_t)i + 1] + 1e-20);
            const double den = a - 2 * bb + c;
            const double d = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
            const double hz = (i - kHalf + d) * kBinHz;
            if (Chan* ch = find(hz, 40)) {
                ch->lastPeak = now;
                if (!ch->manual) ch->setFreq(ch->f + 0.05 * (hz - ch->f));
            } else if (!find(hz, 60) && (int)chans.size() < kCwMaxSignals && !skirt(i, v)) {
                add(hz, false);
            }
        }
        // signals silent for 30 s are dropped (not the focus); two that drifted onto each other become one
        for (const Chan& c : chans) if (c.id != focusId && now - std::max(c.lastActive, c.lastPeak) > 30) retire(c);
        chans.erase(std::remove_if(chans.begin(), chans.end(), [&](const Chan& c) {
            return c.id != focusId && now - std::max(c.lastActive, c.lastPeak) > 30; }), chans.end());
        for (size_t i = 0; i < chans.size(); i++)
            for (size_t j = i + 1; j < chans.size();) {
                if (std::fabs(chans[i].f - chans[j].f) < 25 && chans[j].id != focusId && chans[j].chars <= chans[i].chars) chans.erase(chans.begin() + (long)j);
                else j++;
            }
    }

    // the detector and the timing decoder of one signal, once a millisecond
    void detect(Chan& c, float e) {
        if (!c.init) { c.init = true; c.lo = e; c.hi = e; }
        const float span = c.hi - c.lo;
        const bool open = c.hi > 2.8f * c.lo;
        const bool k = c.raw ? (e > c.lo + 0.35f * span) : (open && e > c.lo + 0.5f * span);
        if (e > c.hi) c.hi += 0.05f * (e - c.hi);
        if (c.raw) c.hi += 0.01f * (e - c.hi);
        else c.lo += (e < c.lo ? 0.05f : 0.004f) * (e - c.lo);
        c.hi -= 0.0005f * (c.hi - c.lo);
        c.raw = k;
        c.keyer.sample(k, c.tm);
        c.key = c.keyer.key();
        if (c.key) c.lastActive = sigSec();
        std::string s = c.tm.take();
        if (!s.empty()) {
            for (char ch : s) { if (ch == '*') c.bad++; if (ch != ' ') c.chars++; }
            c.text += s;
            if (c.text.size() > kCwTextMax) c.text.erase(0, c.text.size() - kCwTextMax);
            const std::vector<std::string> now = cw::callsigns(c.text);
            if (PskReporter::get().want())
                for (const std::string& k : now)
                    if (std::find(c.calls.begin(), c.calls.end(), k) == c.calls.end() && pskr::sendsInText(c.text, k))
                        PskReporter::get().spot("CW", c.f + spotShift.load(), c.lo > 0 ? (float)(20 * std::log10(std::max(1e-6f, c.hi) / c.lo)) : 0.f, k);
            c.calls = now;
        }
    }

    void sidetoneMs(const Chan* c) {
        const float target = c && c->key ? 1.f : 0.f;
        const double w = 2 * kPi * toneHz.load() / kToneRate;
        for (int i = 0; i < kToneRate / 1000; i++) {
            tone += (target > tone ? 1.f : -1.f) * std::min(std::fabs(target - tone), 1.f / 240);   // 5 ms edges
            tonePh = std::remainder(tonePh + w, 2 * kPi);
            const float v = 0.3f * tone * (float)std::sin(tonePh);
            toneBuf.push_back(v);
        }
    }
    float tone = 0;

    void flushTone() {
        if (toneBuf.empty()) return;
        listen.play(toneBuf.data(), toneBuf.size());
        toneBuf.clear();
    }

    void process(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            fftIn.push_back(x[i]);
            if (fftIn.size() >= (size_t)kN) { spectrumFrame(); fftIn.erase(fftIn.begin(), fftIn.begin() + kHop); }
        }
        const bool st = listen.on.load();
        for (size_t i = 0; i < n; i++) {
            const cf32 v = x[i];
            nBase++;
            for (Chan& c : chans) {
                const std::complex<double> m = std::complex<double>(v.real(), v.imag()) * c.rot;
                c.rot *= c.step;
                if (++c.rotCount >= 4096) { c.rot /= std::abs(c.rot); c.rotCount = 0; }
                const cf32 mf((float)m.real(), (float)m.imag());
                c.tri[c.triPos] = mf; c.tri[c.triPos + kTri] = mf;
                if (++c.triPos >= kTri) c.triPos = 0;
                if (++c.phase < kR) continue;
                c.phase = 0;
                const cf32* w = &c.tri[c.triPos];
                cf32 d(0, 0);
                for (int j = 0; j < kTri; j++) d += w[j] * triW[j];
                c.fir[c.firPos] = d; c.fir[c.firPos + kFir] = d;
                if (++c.firPos >= kFir) c.firPos = 0;
                const cf32* h = &c.fir[c.firPos];
                cf32 y(0, 0);
                for (int j = 0; j < kFir; j++) y += h[j] * lp[(size_t)j];
                detect(c, std::abs(y));
            }
            if (st && nBase % kR == 0) {
                const Chan* f = nullptr;
                for (const Chan& c : chans) if (c.id == focusId) f = &c;
                sidetoneMs(f);
            }
        }
        if (st) flushTone();
    }

    void report() {
        tel.seq++;
        tel.timeSec = curRate > 0 ? (double)nIn / curRate : 0;
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.floorDb = (float)(10 * std::log10(floorLin + 1e-20));
        if (accN > 0) {
            tel.spectrum.resize(acc.size());
            for (size_t i = 0; i < acc.size(); i++) { tel.spectrum[i] = (float)(10 * std::log10(acc[i] / accN + 1e-20)); acc[i] = 0; }
            accN = 0;
            tel.specSeq++;
        }
        tel.signals.clear();
        uint64_t ok = 0, bad = 0;
        int decoding = 0;
        float best = 0;
        const double now = sigSec();
        for (const Chan& c : chans) {
            CwSignal s;
            s.id = c.id; s.offsetHz = c.f; s.manual = c.manual;
            s.wpm = c.tm.locked() ? (float)c.tm.wpm() : 0.f;
            s.snrDb = c.lo > 0 ? (float)(20 * std::log10(std::max(1e-6f, c.hi) / c.lo)) : 0.f;
            s.key = c.key; s.locked = c.tm.locked(); s.farnsworth = c.tm.farnsworth();
            s.lastActiveSec = c.lastActive; s.firstSec = c.firstSec;
            s.chars = c.chars; s.text = c.text; s.calls = c.calls;
            ok += c.chars - c.bad; bad += c.bad;
            if (s.locked && now - c.lastActive < 5) decoding++;
            if (c.id == focusId || (focusId < 0 && s.snrDb > best)) { tel.snrDb = s.snrDb; best = s.snrDb; }
            tel.signals.push_back(std::move(s));
        }
        for (const CwSignal& k : kept) tel.signals.push_back(k);
        std::sort(tel.signals.begin(), tel.signals.end(), [](const CwSignal& a, const CwSignal& b) { return a.offsetHz < b.offsetHz; });
        tel.blocksOk = std::max(tel.blocksOk, ok); tel.blocksBad = std::max(tel.blocksBad, bad);
        tel.focusId = focusId;
        tel.state = decoding ? 2 : chans.empty() ? 0 : 1;
        tel.dataValid = decoding > 0;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }
};


CwReceiver::CwReceiver() : p_(std::make_unique<Impl>()) {}
CwReceiver::~CwReceiver() = default;

void CwReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void CwReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool CwReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= cwTuning().minSampleRate - 1; }
void CwReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    std::vector<CwSignal> sig = std::move(p_->pub.signals);   // the text stays (clearResults)
    p_->pub = CwTelemetry();
    p_->pub.seq = s;
    p_->pub.signals = std::move(sig);
}

void CwReceiver::feed(const cf32* x, size_t n) {
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
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.focusReq) {
            m.focusReq = false;
            if (m.wide) {
                int best = 0;
                for (size_t i = 0; i < m.wide->centre.size(); i++)
                    if (std::fabs(m.wide->centre[i] - m.focusHz) < std::fabs(m.wide->centre[(size_t)best] - m.focusHz)) best = (int)i;
                m.wide->focusSlice = best;
                m.wide->subs[(size_t)best]->setFocusHz(m.focusHz - m.wide->centre[(size_t)best]);
                m.focusReq = false;
            }
            Chan* c = m.wide ? nullptr : m.find(m.focusHz, 60);
            if (!c && !m.wide && (int)m.chans.size() < kCwMaxSignals + 4) c = &m.add(m.focusHz, true);
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
        m.process(m.base.data(), m.base.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        if (m.wide) m.reportWide(); else m.report();
    }
}

std::string CwReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.signals);
}
bool CwReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<CwSignal> sig;
    if (!results::unpack(blob, t0, sig)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<CwSignal> in;
    for (auto& s : sig) {
        if (s.chars == 0) continue;
        CwSignal k;
        k.id = s.id; k.offsetHz = s.offsetHz; k.lastActiveSec = s.lastActiveSec; k.firstSec = s.firstSec;
        k.chars = s.chars; k.text = s.text; k.calls = s.calls;
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
void CwReceiver::clearResults() { clearSignals(); }
bool CwReceiver::telemetry(CwTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CwReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void CwReceiver::setFocusHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->focusReq = true; p_->focusHz = hz; }
void CwReceiver::clearSignals() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.signals.clear(); p_->keptLoad = false; p_->keptIn.clear(); p_->pub.seq++; }
    p_->clearReq = true;
}
void CwReceiver::setThresholdDb(float db) { p_->thrDb = std::max(4.f, std::min(30.f, db)); }
void CwReceiver::setSidetone(bool on) { p_->listen.on = on; }
void CwReceiver::setSidetoneHz(float hz) { p_->toneHz = std::max(400.f, std::min(1000.f, hz)); }
void CwReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void CwReceiver::setMuted(bool m) { p_->listen.muted = m; }
void CwReceiver::setSilent(bool s) { p_->listen.silent = s; }
void CwReceiver::setSidetoneTap(std::function<void(const float*, size_t)> cb) { p_->listen.setTap(std::move(cb)); }

ModeTuning cwTuning() {
    ModeTuning t;
    t.stdMode = 36; t.id = "cw"; t.name = "CW";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.020;   // the middle of the 20 m CW segment: the window covers 14.013 - 14.027 MHz
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.014;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const CwBand* cwBands(int& count) {
    // IARU Region 1 band plan: the CW (narrow band) segments of the HF bands
    static const CwBand kB[] = {
        {"160 m", 1.810, 1.838}, {"80 m", 3.500, 3.570}, {"40 m", 7.000, 7.040}, {"30 m", 10.100, 10.130}, {"20 m", 14.000, 14.070},
        {"17 m", 18.068, 18.095}, {"15 m", 21.000, 21.070}, {"12 m", 24.890, 24.915}, {"10 m", 28.000, 28.070},
    };
    count = (int)(sizeof kB / sizeof *kB);
    return kB;
}

} // namespace dect2

namespace dect2 {

void CwReceiver::Impl::reportWide() {
    Impl& m = *this;
    auto& W = *m.wide;
    m.tel.seq++;
    m.tel.timeSec = m.curRate > 0 ? (double)m.nIn / m.curRate : 0;
    m.tel.levelDb = m.nPower ? (float)(10 * std::log10(m.power / (double)m.nPower + 1e-20)) : -200.f;
    m.power = 0; m.nPower = 0;
    m.tel.inputRate = m.curRate;
    m.tel.windowHz = W.dec.spanHz() / 2;
    m.tel.signals.clear();
    std::vector<float> spec;
    uint64_t ok = 0, bad = 0;
    int decoding = 0;
    float floorSum = 0; int nFloor = 0;
    m.tel.focusId = -1;
    for (size_t i = 0; i < W.subs.size(); i++) {
        CwTelemetry t;
        if (!W.subs[i]->telemetry(t, 0)) continue;
        for (CwSignal& s : t.signals) {
            if (std::fabs(s.offsetHz) > kSlice / 2 + (i == 0 || i + 1 == W.subs.size() ? 1000 : 0)) continue;
            s.id += (int)i * 100000;
            s.offsetHz += W.centre[i];
            ok += s.chars;
            if (s.locked && t.timeSec - s.lastActiveSec < 5) decoding++;
            m.tel.signals.push_back(std::move(s));
        }
        bad += t.blocksBad;
        if (t.floorDb > -190) { floorSum += t.floorDb; nFloor++; }
        if ((int)i == W.focusSlice && t.focusId >= 0) m.tel.focusId = t.focusId + (int)i * 100000;
        // the slice's middle 12 kHz of its spectrum, in the order of the slices
        const size_t nb = t.spectrum.size();
        if (nb) {
            const size_t mid = nb / 2, hw = (size_t)(kSlice / 2 / kBinHz);
            for (size_t b = mid - hw; b < mid + hw && b < nb; b++) spec.push_back(t.spectrum[b]);
        }
    }
    if (!spec.empty()) {
        const size_t P = (spec.size() + 3999) / 4000;
        m.tel.spectrum.assign((spec.size() + P - 1) / P, -200.f);
        for (size_t b = 0; b < spec.size(); b++) m.tel.spectrum[b / P] = std::max(m.tel.spectrum[b / P], spec[b]);
        m.tel.specSeq++;
    }
    m.tel.floorDb = nFloor ? floorSum / (float)nFloor : -200.f;
    m.tel.snrDb = 0;
    for (const CwSignal& s : m.tel.signals) {
        if (s.id == m.tel.focusId || (m.tel.focusId < 0 && s.snrDb > m.tel.snrDb)) m.tel.snrDb = s.snrDb;
    }
    for (const CwSignal& k : m.kept) m.tel.signals.push_back(k);
    std::sort(m.tel.signals.begin(), m.tel.signals.end(), [](const CwSignal& a, const CwSignal& b) { return a.offsetHz < b.offsetHz; });
    m.tel.blocksOk = std::max(m.tel.blocksOk, ok); m.tel.blocksBad = std::max(m.tel.blocksBad, bad);
    m.tel.state = decoding ? 2 : m.tel.signals.empty() ? 0 : 1;
    m.tel.dataValid = decoding > 0;
    std::lock_guard<std::mutex> lk(m.mu);
    m.pub = m.tel;
}

void CwReceiver::setWide(bool on, double spanHz) {
    p_->wideSpanReq = std::max(1000.0, std::min(wideSpanCapHz(), spanHz));
    if (p_->wideReq.exchange(on) != on) p_->wideChanged = true;
}
bool CwReceiver::wide() const { return p_->wideReq.load(); }
void CwReceiver::setSpotShiftHz(double hz) { p_->spotShift = hz; }

} // namespace dect2
