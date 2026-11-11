// W1A voice inversion scrambler: detector, descrambler and the speech-like test signal (see w1a_dsp.h).
// Full-band frequency inversion maps an audio frequency f to fc - f (a mixer with a carrier fc and a low-pass at fc). The detector measures the
// long-term spectrum of the voiced frames and compares it with the long-term average speech spectrum, as is, and flipped about every candidate
// fc (2.0 to 4.3 kHz in 25 Hz steps): a peak around 250 - 500 Hz falling about 7 dB per octave, a high-pass edge near 300 Hz and a low-pass
// edge near 3 kHz (telephone-band speech; the shape follows the published long-term speech spectra, Byrne et al. 1994, ITU-T P.50). Where the
// flipped spectrum fits, the audio is inverted again and handed to the audio sink.
#include "w1a_priv.h"
#include <cstdio>
#include <random>

namespace dect2::w1a {

namespace {

constexpr int kFrame = 240;            // 20 ms: the DFT bins are exactly 50 Hz apart
constexpr int kBins = 97;              // 0 .. 4800 Hz
constexpr double kBinHz = 50.0;

// the long-term average speech spectrum template, dB (telephone-band speech)
double ltass(double f) {
    if (f < 300) return -3 - 22 * (300 - f) / 150;
    if (f < 500) return -3 * (500 - f) / 200;
    double v = -7 * std::log2(f / 500);
    if (f > 3000) v -= 20 * (f - 3000) / 200;
    return v;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b) {
    double ma = 0, mb = 0;
    const size_t n = a.size();
    for (size_t i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= (double)n; mb /= (double)n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; i++) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
}

class InvertDec : public Decoder {
public:
    InvertDec() {
        win_.resize(kFrame);
        for (int i = 0; i < kFrame; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kFrame));
        // DFT tables: cos / sin of bin k at sample i (k * i mod 240)
        for (int i = 0; i < kFrame; i++) { cs_[(size_t)i] = (float)std::cos(2 * kPi * i / kFrame); sn_[(size_t)i] = (float)std::sin(2 * kPi * i / kFrame); }
        for (double f = 150; f <= 3450; f += 50) tmpl_.push_back(ltass(f));
        reset();
    }
    void reset() override {
        std::fill(P_, P_ + kBins, 0.0); std::fill(N_, N_ + kBins, 0.0); aw_ = 0; nInit_ = false;
        fill_ = 0; floor_ = 1e9; active_ = 0; frames_ = 0; sinceEval_ = 0; inverted_ = false; fc_ = 0; confirm_ = 0;
        prev_ = cf32(1, 0); dc_ = 0; ph_ = 0; snr_ = 0; corrC_ = corrI_ = 0; judged_ = 0; speech_ = false; spec_.assign(kBins, -100.f);
        lpf_.design(3400, 161); lpfCut_ = 3400;
        state_ = 0; lastMode_.clear();
    }
    void setForced(double fcHz) override { forced_ = fcHz; }
    void push(const cf32* x, size_t n) override {
        std::vector<float> out(n);
        for (size_t i = 0; i < n; i++) {
            cf32 z = x[i];
            if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) z = cf32(0, 0);
            // narrow-FM discriminator, 2.5 kHz deviation = 1.0
            float d = std::arg(z * std::conj(prev_)) * (float)(kRate / (2 * kPi * 2500.0));
            prev_ = z;
            if (!std::isfinite(d)) d = 0;
            dc_ += 0.002f * (d - dc_);
            d -= dc_;
            d = std::max(-2.f, std::min(2.f, d));
            frame_[fill_++] = d;
            if (fill_ == kFrame) { analyse(); fill_ = 0; }
            // descrambled audio
            double fcUse = forced_ > 0 ? forced_ : (inverted_ ? fc_ : 0);
            if (fcUse > 0) {
                ph_ += 2 * kPi * fcUse / kRate; if (ph_ > 2 * kPi) ph_ -= 2 * kPi;
                const double sw = fcUse < 3400 ? fcUse : 3400;
                if (std::fabs(sw - lpfCut_) > 1) { lpfCut_ = sw; lpf_.design(sw, 161); }
                out[i] = lpf_.process(d * 2.f * (float)std::cos(ph_)) * 0.5f;
            } else {
                if (std::fabs(3400 - lpfCut_) > 1) { lpfCut_ = 3400; lpf_.design(3400, 161); }
                out[i] = lpf_.process(d) * 0.5f;
            }
        }
        audio(out.data(), n);
    }
    Status status() const override {
        Status s;
        s.spec = spec_;
        s.good = judged_; s.snrDb = snr_;
        s.cfoHz = inverted_ ? fc_ : 0;
        s.state = state_;
        char m[160];
        if (forced_ > 0) snprintf(m, sizeof m, "descrambling with the carrier you set, %.0f Hz", forced_);
        else if (inverted_) snprintf(m, sizeof m, "inverted speech, carrier %.0f Hz, descrambled", fc_);
        else if (speech_) snprintf(m, sizeof m, "speech, not inverted");
        else snprintf(m, sizeof m, active_ > 0 ? "voice activity, no speech spectrum yet" : "searching");
        s.mode = m;
        char a[64], b[64];
        snprintf(a, sizeof a, "%.2f", corrC_); snprintf(b, sizeof b, "%.2f", corrI_);
        s.params = {{"scrambling", "full-band inversion (f to fc - f)"}, {"fit as heard", a}, {"fit inverted", b},
                    {"carrier", inverted_ ? std::to_string((int)std::lround(fc_)) + " Hz" : "-"}};
        return s;
    }
private:
    void analyse() {
        double e = 0;
        for (int i = 0; i < kFrame; i++) e += (double)frame_[i] * frame_[i];
        floor_ = std::min(e, floor_ * 1.003 + 1e-9);
        frames_++;
        const bool act = e > 1.5 * floor_ && e > 0.02;
        const bool quiet = e < 1.2 * floor_;
        if (act || quiet) {
            double pw[kBins];
            for (int k = 0; k < kBins; k++) {
                double re = 0, im = 0;
                for (int i = 0; i < kFrame; i++) { const int j = (int)(((long)k * i) % kFrame); const double v = (double)frame_[i] * win_[(size_t)i]; re += v * cs_[(size_t)j]; im -= v * sn_[(size_t)j]; }
                pw[k] = re * re + im * im;
            }
            if (act) {
                for (int k = 0; k < kBins; k++) P_[k] = P_[k] * 0.997 + pw[k];
                aw_ = aw_ * 0.997 + 1;
                active_++;
                snr_ = 0.9 * snr_ + 0.1 * 10 * std::log10(e / std::max(floor_, 1e-9));
            } else {   // the noise spectrum from the quiet frames, taken off the speech spectrum when it is judged
                for (int k = 0; k < kBins; k++) N_[k] = nInit_ ? 0.97 * N_[k] + 0.03 * pw[k] : pw[k];
                nInit_ = true;
            }
        }
        if (++sinceEval_ >= 25) { sinceEval_ = 0; evaluate(); }
    }
    double dbAt(double f) const {   // the long-term spectrum at f (mirrored below 0), interpolated
        f = std::fabs(f);
        if (f > (kBins - 1) * kBinHz - 1) f = (kBins - 1) * kBinHz - 1;
        const double u = f / kBinHz;
        const int i = (int)u;
        const double fr = u - i;
        return (1 - fr) * db_[(size_t)i] + fr * db_[(size_t)i + 1];
    }
    void evaluate() {
        // speech needs pauses (a noise spectrum to take off) and voiced frames clearly above them: steady noise or a tone sweep gives neither
        if (active_ < 60 || !nInit_ || snr_ < 1.0) { speech_ = false; return; }   // under a second of voiced audio: nothing to judge yet
        double tot = 0;
        double Pn[kBins];
        for (int k = 0; k < kBins; k++) { Pn[k] = std::max(P_[k] - (nInit_ ? aw_ * N_[k] : 0.0), 0.003 * P_[k]); tot += Pn[k]; }
        if (tot <= 0) return;
        db_.assign(kBins, 0.0);
        for (int k = 0; k < kBins; k++) { db_[(size_t)k] = 10 * std::log10(Pn[k] / tot + 1e-12); spec_[(size_t)k] = (float)db_[(size_t)k]; }
        // Two fits of the long-term spectrum with the speech template. The shape fit (450 - 2700 Hz, inside the speech band, where a flipped
        // and an unflipped spectrum differ by their tilt even at fc = 3300 Hz, which maps 300 - 3000 Hz onto itself) decides between heard
        // and inverted. The edge fit (the whole template, a flipped band exists only up to f = fc) then places fc.
        auto fit = [&](double fc, double f0, double f1) {
            std::vector<double> q, t;
            for (double f = f0; f <= f1 && (fc <= 0 || f <= fc); f += 50) {
                q.push_back(dbAt(fc > 0 ? fc - f : f));
                t.push_back(ltass(f));
            }
            return q.size() < 20 ? -2.0 : pearson(q, t);
        };
        const double cc = fit(0, 450, 2700);
        double best = -2, bestFc = 0;
        for (double fc = 2700; fc <= 3700; fc += 25) { const double c = fit(fc, 450, 2700); if (c > best) { best = c; bestFc = fc; } }
        const bool clear = cc >= 0.45 && cc > best + 0.1;
        const bool inv = best >= 0.45 && best > cc + 0.1;
        if (inv) {
            double eb = -2, ef = bestFc;
            for (double fc = 2700; fc <= 3700; fc += 25) { const double c = fit(fc, 150, 3450); if (c > eb) { eb = c; ef = fc; } }
            bestFc = ef;
            // the band's top edge (fc minus the original low-pass edge near 300 Hz): the highest bin within 12 dB of the band's median level
            std::vector<double> mid;
            for (int k = 10; k <= 40; k++) mid.push_back(db_[(size_t)k]);
            std::sort(mid.begin(), mid.end());
            const double ref = mid[mid.size() / 2];
            int top = 0;
            for (int k = kBins - 2; k > 10; k--) if (db_[(size_t)k] > ref - 12 && db_[(size_t)k + 1] > ref - 30) { top = k; break; }
            const double edgeFc = (top + 1) * kBinHz + 300;
            if (top > 0 && std::fabs(edgeFc - ef) < 450) bestFc = 0.5 * (edgeFc + ef);
        }
        corrC_ = cc; corrI_ = best;
        judged_++;
        speech_ = clear || inv;
        if (inv) {
            if (inverted_ && std::fabs(bestFc - fc_) <= 100) { fc_ = 0.8 * fc_ + 0.2 * bestFc; confirm_++; }
            else if (std::fabs(bestFc - pendFc_) <= 100) { if (++confirm_ >= 2) { inverted_ = true; fc_ = bestFc; } }
            else confirm_ = 1;
            pendFc_ = bestFc;
            state_ = inverted_ ? 2 : 1;
            if (inverted_ && lastMode_ != "inv") { lastMode_ = "inv"; char m[96]; snprintf(m, sizeof m, "inverted speech found, carrier about %.0f Hz", fc_); event(m); }
        } else if (clear) {
            confirm_ = 0; inverted_ = false; state_ = 1;
            if (lastMode_ != "clr") { lastMode_ = "clr"; event("speech, not inverted"); }
        } else {
            state_ = 1;
        }
    }
    std::vector<float> win_;
    float cs_[kFrame] = {}, sn_[kFrame] = {}, frame_[kFrame] = {};
    std::vector<double> tmpl_, db_;
    std::vector<float> spec_;
    double P_[kBins] = {}, N_[kBins] = {}, aw_ = 0;
    bool nInit_ = false;
    int fill_ = 0, frames_ = 0, sinceEval_ = 0, confirm_ = 0, state_ = 0;
    uint64_t active_ = 0, judged_ = 0;
    double floor_ = 1e9, fc_ = 0, pendFc_ = 0, forced_ = 0, ph_ = 0, snr_ = 0, corrC_ = 0, corrI_ = 0, lpfCut_ = 0;
    float dc_ = 0;
    bool inverted_ = false, speech_ = false;
    cf32 prev_{1.f, 0.f};
    Fir lpf_;
    std::string lastMode_;
};

}  // namespace

std::unique_ptr<Decoder> makeInvertDecoder() { return std::make_unique<InvertDec>(); }

// ---------------------------------------------------------------- generators
std::vector<float> genSpeechLike(const GenOpts& o) {
    const size_t N = (size_t)(o.seconds * kRate);
    std::vector<float> x(N, 0.f);
    std::mt19937 rng(o.seed * 2654435761u + 1);
    auto U = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    std::normal_distribution<float> gauss(0, 1);
    static const double V[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {570, 840, 2410}};
    const double bw[3] = {90, 110, 160}, wt[3] = {1.0, 0.55, 0.3};
    size_t pos = 0;
    double phi = 0, hp = 0;
    const double f0base = U(105, 190);
    while (pos < N) {
        const bool pause = U(0, 1) < 0.15;
        size_t len = (size_t)(U(0.14, 0.26) * kRate);
        if (pause) { pos += (size_t)(U(0.2, 0.45) * kRate); continue; }
        const int v = (int)U(0, 5) % 5;
        const double f0a = f0base * U(0.9, 1.15), f0b = f0a * U(0.85, 1.12);
        const double amp = U(0.6, 1.0);
        for (size_t i = 0; i < len && pos + i < N; i++) {
            const double u = (double)i / (double)len;
            const double f0 = f0a + (f0b - f0a) * u;
            const double env = std::sin(kPi * u);   // syllable envelope
            phi += 2 * kPi * f0 / kRate;
            if (phi > 2 * kPi) phi -= 2 * kPi;
            double s = 0;
            for (int k = 1; k * f0 < 3200; k++) {
                const double f = k * f0;
                double g = 0.04;
                for (int j = 0; j < 3; j++) { const double dd = (f - V[v][j]) / bw[j]; g += wt[j] / (1 + dd * dd); }
                s += g / std::pow((double)k, 1.0) * std::cos(k * phi);
            }
            x[pos + i] += (float)(amp * env * s * 0.15);
        }
        pos += len;
        if (U(0, 1) < 0.35) {   // a fricative
            const size_t fl = (size_t)(U(0.05, 0.1) * kRate);
            for (size_t i = 0; i < fl && pos + i < N; i++) {
                const float nz = gauss(rng);
                const float d = nz - (float)hp; hp = nz;   // first difference: tilts the noise up
                x[pos + i] += (float)(0.12 * std::sin(kPi * (double)i / (double)fl)) * d * 0.5f;
            }
            pos += fl;
        }
    }
    // telephone band 300 - 3000 Hz: low-pass then high-pass (as the difference of two low-passes)
    Fir hi(3000, 255), lo(300, 255);
    std::vector<float> y(N);
    const int dly = hi.delay();
    for (size_t i = 0; i < N + (size_t)dly; i++) {
        const float s = i < N ? x[i] : 0.f;
        const float a = hi.process(s), b = lo.process(s);
        if (i >= (size_t)dly) y[i - (size_t)dly] = a - b;
    }
    float pk = 1e-6f;
    for (float v : y) pk = std::max(pk, std::fabs(v));
    for (float& v : y) v = v / pk * 0.5f;
    return y;
}

std::vector<float> invertAudio(const std::vector<float>& x, double fcHz) {
    Fir lp(std::min(fcHz, 3400.0), 161);
    std::vector<float> y(x.size());
    double ph = 0;
    const int dly = lp.delay();
    for (size_t i = 0; i < x.size() + (size_t)dly; i++) {
        const float s = i < x.size() ? x[i] * 2.f * (float)std::cos(ph) : 0.f;
        ph += 2 * kPi * fcHz / kRate; if (ph > 2 * kPi) ph -= 2 * kPi;
        const float o = lp.process(s);
        if (i >= (size_t)dly) y[i - (size_t)dly] = o;
    }
    return y;
}

std::vector<cf32> genInvert(const GenOpts& o) {
    std::vector<float> a = genSpeechLike(o);
    if (o.invertHz > 0) a = invertAudio(a, o.invertHz);
    std::vector<cf32> z(a.size());
    double ph = 0;
    for (size_t i = 0; i < a.size(); i++) {
        ph += 2 * kPi * o.deviationHz * (double)a[i] * 2.0 / kRate;   // 0.5 peak audio = full deviation
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
        z[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.5f;
    }
    return z;
}

}  // namespace dect2::w1a
