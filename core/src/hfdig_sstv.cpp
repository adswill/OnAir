// SSTV decoder of the HF digital receiver (see hfdig_sstv.h).
//
// Front end: the 8 kHz audio is mixed down by 1900 Hz, low-passed and turned into an unwrapped phase, one value per sample, kept in a ring.
// Any mean frequency over any stretch of time (fractional sample positions) is then one subtraction: the phase difference over the
// stretch. That is what a pixel is (the mean tone over its time slot), what a sync pulse search is (the lowest mean over a window as wide
// as the pulse) and what a VIS bit is.
//
// Idle: a state machine on the 4 ms mean frequency looks for the VIS header: leader 1900 Hz, break 1200 Hz (10 ms), leader, start bit
// 1200 Hz, seven data bits (1100 Hz = 1, 1300 Hz = 0, least significant first), even parity, stop bit, 30 ms each. At the same time the
// 1200 Hz pulses are logged: five pulses at the line period of a known mode start that mode without a VIS header.
// Receiving: each line's sync pulse is searched near where the previous ones predict it; a straight line fitted through the line start
// times gives the line period of the sender against this clock (the slant, as a ratio applied to all times inside the line) and the
// position of each line. A line is cut into pixels when its time has passed.
#include "dect2/hfdig_sstv.h"
#include "dect2/hfdig_gen.h"
#include "hfdig_sstv_modes.h"
#include "hfdig_usb.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace dect2 {

using namespace sstv;

namespace {

constexpr double kFs = 8000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kCenter = 1900.0;           // the mixer frequency
constexpr int kTaps = 63;
constexpr double kLowpassHz = 700.0;         // of the complex baseband around 1900 Hz: passes 1200 .. 2600 Hz
constexpr size_t kRing = 1 << 17;            // samples of phase kept (16 s)
constexpr int kBox = 32;                     // the mean frequency used by the VIS and pulse search: 4 ms

std::atomic<uint64_t> gNextId{1};

} // namespace

struct HfdigSstv::Impl {
    // ------------------------------------------------------------ front end
    std::vector<float> taps;
    std::vector<std::complex<float>> osc;
    std::vector<float> hr, hi;               // mixed samples, twice the length for a contiguous window
    int hpos = 0;
    std::complex<float> zprev{0.f, 0.f};
    double phase = 0;
    std::vector<double> ph;
    uint64_t n = 0;                          // samples fed; ph[i & mask] is valid for i < n

    // ------------------------------------------------------------ state
    enum Vs { Idle, Lead1, Break, Lead2, Bits };
    Vs vs = Idle;
    int leadCount = 0, trans = 0;
    double leadSum = 0, leadMean = kCenter;
    double edge1 = 0, edge2 = 0, edge3 = 0;
    // pulse log (idle)
    bool inPulse = false;
    uint64_t runStart = 0;
    std::vector<double> events;

    struct Rx {
        const Spec* sp = nullptr;
        bool vis = false;
        double c0 = 0, off = 0;
        double periodS = 0;                  // nominal samples per line
        int detect = 0, extract = 0;
        int nFit = 0;
        double sk = 0, sc = 0, skk = 0, skc = 0, a = 0, b = 0;
        int missed = 0;
        double slant = 0;
        std::vector<uint8_t> rgb;
        int rows = 0;
        std::vector<double> yPend;           // Robot 36: luma and R-Y of the even line waiting for its B-Y
        std::vector<double> ryPend;
        int pendLine = -1;
        uint64_t id = 0;
        int64_t t0 = 0;
    } rx;
    bool active = false;

    // ------------------------------------------------------------ report
    HfdigSstvTelemetry tel;
    bool dirty = false;
    std::vector<std::shared_ptr<const HfdigSstvImage>> history;
    std::shared_ptr<const HfdigSstvImage> image;
    uint64_t imageSeq = 0, done = 0;
    std::string lastMode;
    int lastVis = -1, lastW = 0, lastH = 0;
    double lastSlant = 0, lastOff = 0;

    Impl() {
        const auto h = hfdig::lowpass(kTaps, kLowpassHz / kFs, 6.0);
        taps.resize(kTaps);
        for (int k = 0; k < kTaps; k++) taps[(size_t)k] = (float)h[(size_t)k];
        osc.resize(80);
        for (int i = 0; i < 80; i++) { const double a = -2 * kPi * kCenter * i / kFs; osc[(size_t)i] = {(float)std::cos(a), (float)std::sin(a)}; }
        hr.assign(2 * kTaps, 0.f); hi.assign(2 * kTaps, 0.f);
        ph.assign(kRing, 0.0);
    }

    void resetAll() {
        std::fill(hr.begin(), hr.end(), 0.f); std::fill(hi.begin(), hi.end(), 0.f);
        hpos = 0; zprev = {0.f, 0.f}; phase = 0; n = 0;
        std::fill(ph.begin(), ph.end(), 0.0);
        vs = Idle; leadCount = 0; trans = 0; leadSum = 0; inPulse = false; events.clear();
        active = false;
        image.reset(); dirty = false; lastMode.clear(); lastVis = -1; lastW = lastH = 0; lastSlant = lastOff = 0;
        imageSeq++;
        tel = HfdigSstvTelemetry();
    }

    // ------------------------------------------------------------ phase access
    double phaseAt(double t) const {
        const double fl = std::floor(t);
        const uint64_t i = (uint64_t)fl;
        const double a = ph[i & (kRing - 1)], b = ph[(i + 1) & (kRing - 1)];
        return a + (b - a) * (t - fl);
    }
    // mean tone (Hz) over [t0, t1] in samples
    double meanF(double t0, double t1) const { return kCenter + (phaseAt(t1) - phaseAt(t0)) * kFs / (2 * kPi * (t1 - t0)); }
    double boxF(uint64_t i) const { return kCenter + (ph[i & (kRing - 1)] - ph[(i - kBox) & (kRing - 1)]) * kFs / (2 * kPi * kBox); }

    // ------------------------------------------------------------ per sample
    void sample(float x) {
        const std::complex<float> m = osc[n % 80] * x;
        hr[(size_t)hpos] = hr[(size_t)(hpos + kTaps)] = m.real();
        hi[(size_t)hpos] = hi[(size_t)(hpos + kTaps)] = m.imag();
        if (++hpos >= kTaps) hpos = 0;
        const float* wr = &hr[(size_t)hpos];
        const float* wi = &hi[(size_t)hpos];
        float sr = 0, si = 0;
        for (int k = 0; k < kTaps; k++) { sr += taps[(size_t)k] * wr[k]; si += taps[(size_t)k] * wi[k]; }
        const std::complex<float> z(sr, si);
        if (n > 0) phase += std::arg(z * std::conj(zprev));
        zprev = z;
        ph[n & (kRing - 1)] = phase;
        n++;
        if (n < (uint64_t)kBox + 2) return;
        if (active) advanceRx();
        else idleStep();
    }

    // ------------------------------------------------------------ idle: VIS header and pulse log
    void idleStep() {
        const uint64_t i = n - 1;
        const double fb = boxF(i);
        pulseLog(i, fb);
        const double dev = fb - leadMean;
        switch (vs) {
        case Idle:
            if (std::fabs(fb - kCenter) < 130) { leadCount++; leadSum += fb; leadMean = leadSum / leadCount; }
            else { leadCount = 0; leadSum = 0; leadMean = kCenter; }
            if (leadCount >= 800) { vs = Lead1; trans = 0; }
            break;
        case Lead1:
            if (std::fabs(dev) < 130) { trans = 0; leadCount++; leadSum += fb; leadMean = leadSum / leadCount; }
            else if (dev < -350) { edge1 = (double)i - kBox / 2; vs = Break; }
            else if (dev < -130) { if (++trans > 48) vs = Idle; }
            else vs = Idle;
            break;
        case Break:
            if (dev > -350) {
                edge2 = (double)i - kBox / 2;
                const double d = edge2 - edge1;
                if (d >= 7 * 8 && d <= 13 * 8) { vs = Lead2; trans = 0; } else vs = Idle;
            } else if ((double)i - edge1 > 16 * 8) vs = Idle;
            break;
        case Lead2: {
            const double since = (double)i - edge2;
            if (since < 40) break;                                       // the box is still on the rising edge
            if (std::fabs(dev) < 130) { trans = 0; }
            else if (dev < -350 && since > 120 * 8) {
                edge3 = (double)i - kBox / 2;
                if (edge3 - edge2 <= 600 * 8) vs = Bits; else vs = Idle;
            } else if (dev < -130 && since > 120 * 8) { if (++trans > 48) vs = Idle; }
            else vs = Idle;
            if (since > 700 * 8) vs = Idle;
            break;
        }
        case Bits:
            if ((double)i >= edge3 + 300 * 8 + 8) { decodeVis(); if (!active) { vs = Idle; leadCount = 0; leadSum = 0; leadMean = kCenter; } }
            break;
        }
        if (vs == Idle && leadCount == 0) leadMean = kCenter;
    }

    void decodeVis() {
        const double off = leadMean - kCenter;
        int code = 0, ones = 0;
        for (int j = 0; j < 10; j++) {
            const double t0 = edge3 + (30 * j + 6) * 8, t1 = edge3 + (30 * j + 24) * 8;
            const double f = meanF(t0, t1);
            if (j == 0) { if (std::fabs(f - (1200 + off)) > 110) return; continue; }
            if (j == 9) break;
            const int b = f < 1200 + off ? 1 : 0;
            ones += b;
            if (j <= 7) code |= b << (j - 1);
        }
        if (ones & 1) return;
        const auto& tab = table();
        for (const Spec& s : tab) if (s.info.vis == code) { startRx(s, true, edge3 + 300 * 8, off); return; }
    }

    // the log of 1200 Hz pulses: five at the period of a mode start that mode
    void pulseLog(uint64_t i, double fb) {
        if (fb < 1330) { if (!inPulse) { inPulse = true; runStart = i; } return; }
        if (!inPulse) return;
        inPulse = false;
        const double len = (double)(i - runStart);
        if (len < 6 || len > 40 * 8) return;
        const double centre = ((double)runStart + (double)i) * 0.5 - kBox / 2;
        events.push_back(centre);
        if (events.size() > 8) events.erase(events.begin());
        if (events.size() < 5) return;
        const size_t m = events.size();
        const double d = (events[m - 1] - events[m - 5]) / 4;
        for (size_t k = m - 4; k < m; k++) if (std::fabs((events[k] - events[k - 1]) - d) > 0.006 * d) return;
        for (const Spec& s : table()) {
            if (std::fabs(d - s.info.lineMs * 8) < 0.01 * d) {
                const double startOfPulse = events[m - 5] - s.info.syncMs * 4;
                startRx(s, false, startOfPulse - s.syncOffMs * 8, 0);
                return;
            }
        }
    }

    // ------------------------------------------------------------ receiving
    void startRx(const Spec& s, bool vis, double c0, double off) {
        rx = Rx();
        rx.sp = &s; rx.vis = vis; rx.c0 = c0; rx.off = off;
        rx.periodS = s.info.lineMs * 8;
        rx.rgb.assign((size_t)s.info.width * s.info.height * 3, 0);
        rx.id = gNextId++;
        rx.t0 = (int64_t)std::time(nullptr);
        active = true; vs = Idle; leadCount = 0; leadSum = 0; leadMean = kCenter; events.clear(); inPulse = false;
        lastMode = s.info.name; lastVis = vis ? s.info.vis : -1; lastW = s.info.width; lastH = s.info.height;
        lastSlant = 0; lastOff = off;
        dirty = true;
        tel.state = 1;
    }

    double ratio() const { return rx.nFit >= 8 && rx.b > 0 ? rx.b / rx.periodS : 1.0; }
    // where line k starts (samples)
    double lineStart(int k) const {
        if (rx.nFit == 0) return rx.c0 + k * rx.periodS;
        if (rx.nFit >= 8 && rx.b > 0) return rx.c0 + rx.a + rx.b * k;
        return rx.c0 + rx.a + k * rx.periodS;     // too few lines for the slope: offset only
    }

    void advanceRx() {
        const Spec& sp = *rx.sp;
        const double now = (double)n - 2;
        for (;;) {
            const double r = ratio();
            if (rx.detect < sp.totalLines) {
                const double hw = (rx.detect == 0 && rx.vis ? 40.0 : 10.0) * 8;
                const double s = lineStart(rx.detect) + sp.syncOffMs * 8 * r;
                if (now >= s + hw + sp.info.syncMs * 8 + 4) { detectSync(rx.detect, s, hw); rx.detect++; if (!active) return; continue; }
            }
            if (rx.extract < rx.detect && rx.extract < sp.totalLines) {
                if (now >= lineStart(rx.extract) + rx.periodS * r + 8) {
                    extractLine(rx.extract);
                    rx.extract++;
                    if (rx.extract >= sp.totalLines) { finish(true); return; }
                    continue;
                }
            }
            break;
        }
    }

    void detectSync(int k, double sPred, double hw) {
        const Spec& sp = *rx.sp;
        const double W = sp.info.syncMs * 8;
        const bool afterVis = k == 0 && rx.vis;
        const double lo = afterVis ? sPred - 4 * 8 : sPred - hw;      // after the VIS header the bits before the stop bit must stay out (1100 Hz is lower than 1200)
        const int steps = (int)(sPred + hw - lo);
        std::vector<double> m((size_t)steps + 1);
        int best = 0;
        for (int j = 0; j <= steps; j++) {
            const double t = lo + j;
            m[(size_t)j] = meanF(t, t + W);
            if (m[(size_t)j] < m[(size_t)best]) best = j;
        }
        // the first line after a VIS header: the stop bit (1200 Hz) runs into the sync pulse, so the minimum is a plateau; the pulse
        // is the box at its far end
        if (afterVis) {
            for (int j = steps; j > best; j--) if (m[(size_t)j] <= m[(size_t)best] + 8) { best = j; break; }
        }
        double tb = lo + best, mv = m[(size_t)best];
        if (best > 0 && best < steps && !afterVis) {
            const double a = m[(size_t)best - 1], c = m[(size_t)best + 1], d = a - 2 * mv + c;
            if (d > 1e-9) tb += std::max(-0.5, std::min(0.5, 0.5 * (a - c) / d));
        }
        const double r = ratio();
        const double cm = tb - sp.syncOffMs * 8 * r;            // measured line start
        const bool found = mv < 1200 + rx.off + 110 && mv > 1000;
        const bool gated = rx.nFit >= 3 && std::fabs(cm - lineStart(k)) > 3 * 8;
        if (!found || gated) {
            if (++rx.missed >= 12) finish(false);
            return;
        }
        rx.missed = 0;
        if (!rx.vis) rx.off = rx.nFit == 0 ? mv - 1200 - 12 : 0.9 * rx.off + 0.1 * (mv - 1200 - 12);   // no leader to measure: the sync pulse (its mean sits about 12 Hz high, the filter blurs its edges)
        const double cc = cm - rx.c0;
        rx.nFit++; rx.sk += k; rx.sc += cc; rx.skk += (double)k * k; rx.skc += k * cc;
        const double nn = rx.nFit;
        const double den = nn * rx.skk - rx.sk * rx.sk;
        if (rx.nFit >= 8 && den > 1e-9) {
            double b = (nn * rx.skc - rx.sk * rx.sc) / den;
            b = std::max(0.98 * rx.periodS, std::min(1.02 * rx.periodS, b));
            rx.b = b;
            rx.a = (rx.sc - b * rx.sk) / nn;
            rx.slant = (b / rx.periodS - 1.0) * 1e6;
        } else {
            rx.a = (rx.sc - rx.periodS * rx.sk) / nn;
        }
        lastSlant = rx.slant; lastOff = rx.off;
    }

    uint8_t valueOf(double f) const { return clamp8((f - rx.off - 1500.0) * 255.0 / 800.0); }
    double meanOver(double c, double r, double startMs, double lenMs) const {
        const double t0 = c + startMs * 8 * r, t1 = c + (startMs + lenMs) * 8 * r;
        return meanF(t0, t1);
    }
    void scan(double c, double r, const Scan& s, std::vector<double>& out) const {
        out.resize((size_t)s.px);
        const double px = s.lenMs / s.px;
        for (int i = 0; i < s.px; i++) out[(size_t)i] = valueOf(meanOver(c, r, s.startMs + i * px, px));
    }
    uint8_t* row(int y) { return &rx.rgb[(size_t)y * rx.sp->info.width * 3]; }

    void extractLine(int k) {
        const Spec& sp = *rx.sp;
        const int w = sp.info.width;
        const double c = lineStart(k), r = ratio();
        std::vector<double> v[4];
        std::vector<double> comp[7];
        switch (sp.kind) {
        case Kind::Seq:
            for (const Scan& s : sp.scans) scan(c, r, s, comp[s.comp]);
            for (int x = 0; x < w; x++) { uint8_t* o = row(k) + x * 3; o[0] = (uint8_t)comp[kR][(size_t)x]; o[1] = (uint8_t)comp[kG][(size_t)x]; o[2] = (uint8_t)comp[kB][(size_t)x]; }
            rx.rows = k + 1;
            break;
        case Kind::Robot72:
            for (const Scan& s : sp.scans) scan(c, r, s, comp[s.comp]);
            for (int x = 0; x < w; x++) yuvToRgb(comp[kY][(size_t)x], comp[kRY][(size_t)x / 2], comp[kBY][(size_t)x / 2], row(k) + x * 3);
            rx.rows = k + 1;
            break;
        case Kind::Robot36: {
            for (const Scan& s : sp.scans) scan(c, r, s, comp[s.comp]);
            const double sep = meanOver(c, r, sp.fixed[0].startMs + 0.5, sp.fixed[0].lenMs - 1.0) - rx.off;
            const bool odd = sep > kCenter;
            const std::vector<double>& chroma = comp[kRY];
            if (!odd) {
                rx.yPend = comp[kY]; rx.ryPend = chroma; rx.pendLine = k;
                for (int x = 0; x < w; x++) yuvToRgb(comp[kY][(size_t)x], chroma[(size_t)x / 2], 128, row(k) + x * 3);
            } else {
                for (int x = 0; x < w; x++) {
                    const double ry = rx.pendLine == k - 1 ? rx.ryPend[(size_t)x / 2] : 128;
                    if (rx.pendLine == k - 1) yuvToRgb(rx.yPend[(size_t)x], ry, chroma[(size_t)x / 2], row(k - 1) + x * 3);
                    yuvToRgb(comp[kY][(size_t)x], ry, chroma[(size_t)x / 2], row(k) + x * 3);
                }
                rx.pendLine = -1;
            }
            rx.rows = k + 1;
            break;
        }
        case Kind::PD:
            for (const Scan& s : sp.scans) scan(c, r, s, comp[s.comp]);
            for (int x = 0; x < w; x++) {
                yuvToRgb(comp[kY][(size_t)x], comp[kRY][(size_t)x], comp[kBY][(size_t)x], row(2 * k) + x * 3);
                yuvToRgb(comp[kY2][(size_t)x], comp[kRY][(size_t)x], comp[kBY][(size_t)x], row(2 * k + 1) + x * 3);
            }
            rx.rows = 2 * (k + 1);
            break;
        }
        dirty = true;
    }

    std::shared_ptr<HfdigSstvImage> snapshot(bool complete) const {
        auto im = std::make_shared<HfdigSstvImage>();
        im->id = rx.id; im->mode = rx.sp->info.name; im->width = rx.sp->info.width; im->height = rx.sp->info.height;
        im->lines = rx.rows; im->complete = complete; im->slantPpm = rx.slant; im->unixTime = rx.t0; im->rgb = rx.rgb;
        return im;
    }

    void finish(bool complete) {
        active = false;
        if (rx.rows > 4 || complete) {
            auto im = snapshot(complete);
            image = im; imageSeq++;
            history.insert(history.begin(), im);
            if (history.size() > (size_t)kSstvHistory) history.resize((size_t)kSstvHistory);
            done++;
        }
        dirty = false;
        tel.state = 2;
        vs = Idle; leadCount = 0; leadSum = 0; leadMean = kCenter; events.clear(); inPulse = false;
    }

    void publish(HfdigSstvTelemetry& out) {
        if (active && dirty) { image = snapshot(false); imageSeq++; dirty = false; }
        tel.mode = lastMode; tel.visCode = lastVis; tel.width = lastW; tel.height = lastH;
        tel.lines = active ? rx.rows : (image ? image->lines : 0);
        tel.slantPpm = lastSlant; tel.offsetHz = lastOff;
        tel.imageSeq = imageSeq; tel.picturesDone = done; tel.image = image; tel.history = history;
        out = tel;
    }
};

HfdigSstv::HfdigSstv() : p_(std::make_unique<Impl>()) {}
HfdigSstv::~HfdigSstv() = default;
void HfdigSstv::reset() { p_->resetAll(); }
void HfdigSstv::feedAudio(const float* x, size_t n) {
    for (size_t i = 0; i < n; i++) p_->sample(x[i]);
    p_->tel.audioSamples += n;
}
void HfdigSstv::telemetry(HfdigSstvTelemetry& out) const { p_->publish(out); }

std::unique_ptr<HfdigSstv> makeSstvDecoder() { return std::make_unique<HfdigSstv>(); }

} // namespace dect2
