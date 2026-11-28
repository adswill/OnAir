// Meteor scatter detector parts (see mscatter_dsp.h). Written from the general idea of forward-scatter meteor radio detection (a fixed beacon,
// narrowband echoes in the spectrogram next to its line); no code from other meteor detectors.
#include "dect2/mscatter/mscatter_dsp.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace ms {

int decimation(double inputRate, double targetRate) { return std::max(1, (int)std::lround(inputRate / std::max(1.0, targetRate))); }

int clampFft(int n) {
    int best = 1024;
    for (int c : {1024, 2048, 4096, 8192}) if (std::abs(c - n) < std::abs(best - n)) best = c;
    return best;
}
int hopFor(int fftSize) { return std::max(16, fftSize / 8); }
double clampBandwidthHz(double hz) { return std::max(3000.0, std::min(48000.0, hz)); }
int wfHalfBins(int fftSize) { return std::max(8, (int)std::lround(85.0 * fftSize / kFft)); }
double wfBinsPerColumn(int fftSize) { return 2.0 * fftSize / kFft; }
int guardBins(int fftSize) { return std::max(1, (int)std::lround(3.0 * fftSize / kFft)); }

double cicNoiseGain(int d) {
    if (d <= 1) return 1.0;
    std::vector<double> h(1, 1.0 / d);
    std::vector<double> box((size_t)d, 1.0 / d);
    for (int s = 0; s < 2; s++) {
        std::vector<double> o(h.size() + box.size() - 1, 0.0);
        for (size_t i = 0; i < h.size(); i++) for (size_t j = 0; j < box.size(); j++) o[i + j] += h[i] * box[j];
        h.swap(o);
    }
    double e = 0, sum = 0;
    for (double v : h) { e += v * v; sum += v; }
    return e / (sum * sum);
}

// ---------------------------------------------------------------- Front
void Front::configure(double inputRate, double targetRate) {
    rate_ = inputRate;
    d_ = decimation(inputRate, targetRate);
    scale_ = 1.0 / ((double)d_ * d_ * d_);
    for (int s = 0; s < 3; s++) { bre_[s].assign((size_t)d_, 0.0); bim_[s].assign((size_t)d_, 0.0); }
    reset();
}
void Front::reset() {
    for (int s = 0; s < 3; s++) { std::fill(bre_[s].begin(), bre_[s].end(), 0.0); std::fill(bim_[s].begin(), bim_[s].end(), 0.0); sre_[s] = sim_[s] = 0; }
    idx_ = 0; cnt_ = 0;
}
size_t Front::process(const cf32* x, size_t n, std::complex<float>* out) {
    size_t no = 0;
    if (d_ <= 1) { for (size_t i = 0; i < n; i++) out[no++] = x[i]; return no; }
    for (size_t i = 0; i < n; i++) {
        double re = x[i].real(), im = x[i].imag();
        for (int s = 0; s < 3; s++) {
            sre_[s] += re - bre_[s][(size_t)idx_]; sim_[s] += im - bim_[s][(size_t)idx_];
            bre_[s][(size_t)idx_] = re; bim_[s][(size_t)idx_] = im;
            re = sre_[s]; im = sim_[s];
        }
        if (++idx_ >= d_) idx_ = 0;
        if (++cnt_ >= d_) { cnt_ = 0; out[no++] = std::complex<float>((float)(re * scale_), (float)(im * scale_)); }
    }
    return no;
}

// ---------------------------------------------------------------- Detector
namespace {
inline double db10(double v) { return v > 1e-30 ? 10.0 * std::log10(v) : -300.0; }
inline double parab(double a, double b, double c) {   // peak offset of three power values, in bins
    const double la = std::log(std::max(a, 1e-30)), lb = std::log(std::max(b, 1e-30)), lc = std::log(std::max(c, 1e-30));
    const double den = la - 2 * lb + lc;
    if (den >= -1e-9) return 0;
    return std::max(-0.5, std::min(0.5, 0.5 * (la - lc) / den));
}
}

void Detector::configure(double workRate, int fftSize) {
    rate_ = workRate;
    n_ = clampFft(fftSize); hop_ = hopFor(n_); guard_ = guardBins(n_);
    fft_ = Fft(n_);
    const int kN = n_;
    binHz_ = workRate / kN;
    win_.assign(kN, 0.f);
    sumw_ = 0;
    for (int i = 0; i < kN; i++) { const double wv = 0.5 - 0.5 * std::cos(2 * math::kPi * i / kN); win_[(size_t)i] = (float)wv; sumw_ += wv; }
    reset();
}

void Detector::reset() {
    const int kN = n_;
    buf_.assign(kN, cf32(0, 0));
    fill_ = 0; frames_ = 0;
    P_.assign(kN, 0.f); S_.assign(kN, 0.f); B_.assign(kN, 0.f);
    kb_ = -1; cand_ = -1; candCount_ = 0; beaconLost_ = 0;
    beaconHz_ = 0; beaconSnr_ = 0; beaconDbfs_ = -200; noiseDbfs_ = -200; noiseMean_ = 0;
    active_ = false; pend_ = 0; silent_ = 0; bbPrev_ = false;
    lv_.clear(); dop_.clear(); fr_.clear();
    wf_.assign((size_t)kMsWfRows * kMsWfCols, 0); row_.assign(kMsWfCols, 0); rowFrames_ = 0; wfRows_ = 0;
    specOut_.assign(kMsWfCols, 0.f);
    activeDb_ = 0;
}

std::vector<RawPing> Detector::take() { std::vector<RawPing> o; o.swap(done_); return o; }

void Detector::waterfall(std::vector<uint8_t>& wf, std::vector<float>& spec) const {
    wf.assign((size_t)kMsWfRows * kMsWfCols, 0);
    // rows are kept newest last; the first (kMsWfRows - wfRows_) rows stay empty
    const int have = std::min(wfRows_, kMsWfRows);
    for (int r = 0; r < have; r++)
        std::copy_n(wf_.begin() + (size_t)(kMsWfRows - have + r) * kMsWfCols, kMsWfCols, wf.begin() + (size_t)(kMsWfRows - have + r) * kMsWfCols);
    spec = specOut_;
}

void Detector::push(const std::complex<float>* x, size_t n) {
    if (rate_ <= 0) return;
    const int kN = n_, kHop = hop_;
    for (size_t i = 0; i < n; i++) {
        // sliding window kept in hist_ as two copies of a ring so the window is contiguous
        buf_[fill_ % kN] = x[i];
        fill_++;
        if (fill_ >= (size_t)kN && (fill_ - kN) % kHop == 0) frame();
    }
}

void Detector::frame() {
    const int kN = n_, kC = n_ / 2, kHop = hop_, kGuardBins = guard_;
    // window (oldest sample first) and FFT
    std::vector<cf32> w(kN);
    const size_t base = fill_ - kN;
    for (int i = 0; i < kN; i++) w[(size_t)i] = buf_[(base + (size_t)i) % kN] * win_[(size_t)i];
    fft_.forward(w.data());
    const float inv = (float)(1.0 / (sumw_ * sumw_));
    for (int k = 0; k < kN; k++) {
        const int s = (k + kC) % kN;
        P_[(size_t)s] = std::norm(w[(size_t)k]) * inv;
    }
    const uint64_t f = frames_;
    const double fps = rate_ / kHop;
    // long-term spectrum
    const float aS = f == 0 ? 1.f : (float)std::max(1.0 / (double)(f + 1), 1.0 / (2.0 * fps));
    for (int s = 0; s < kN; s++) S_[(size_t)s] += aS * (P_[(size_t)s] - S_[(size_t)s]);
    if (f == 0) B_ = P_;
    // noise: the median of the long-term spectrum
    if (f % 32 == 31 || f == 40) {
        std::vector<float> t(S_);
        std::nth_element(t.begin(), t.begin() + kN / 2, t.end());
        noiseMean_ = t[(size_t)kN / 2];
        noiseDbfs_ = (float)db10(noiseMean_);
    }
    const bool warm = (double)(f + 1) * kHop / rate_ >= kWarmSec;
    // the beacon line
    if (warm && f % 4 == 0 && noiseMean_ > 0) {
        const int lim = std::min(kC - 8, (int)std::lround(0.35 * rate_ / binHz_));
        int km = kC - lim;
        for (int s = kC - lim; s <= kC + lim; s++) if (S_[(size_t)s] > S_[(size_t)km]) km = s;
        const double snrBin = S_[(size_t)km] / noiseMean_;
        if (kb_ < 0) {
            if (snrBin > 16.0) { kb_ = km; candCount_ = 0; }
        } else {
            if (std::abs(km - kb_) >= 2 && S_[(size_t)km] > 1.4 * S_[(size_t)kb_]) { if (++candCount_ > 20) { kb_ = km; candCount_ = 0; } }
            else candCount_ = 0;
            if (S_[(size_t)kb_] < 3.0 * noiseMean_) { if (++beaconLost_ > 40 && !active_) { kb_ = -1; beaconLost_ = 0; } }
            else beaconLost_ = 0;
        }
        if (kb_ >= 0) {
            int pk = kb_;
            for (int s = kb_ - 2; s <= kb_ + 2; s++) if (S_[(size_t)s] > S_[(size_t)pk]) pk = s;
            beaconHz_ = (pk - kC + parab(S_[(size_t)pk - 1], S_[(size_t)pk], S_[(size_t)pk + 1])) * binHz_;
            double tot = 0;
            for (int s = pk - 3; s <= pk + 3; s++) tot += S_[(size_t)s];
            const double a2 = tot / 1.5;
            beaconSnr_ = db10(a2 / noiseMean_);
            beaconDbfs_ = db10(a2);
        } else { beaconSnr_ = 0; beaconDbfs_ = -200; }
    }
    const int W = (int)std::ceil(kEchoSpanHz / binHz_);
    bool bb = false;
    int best = -1;
    double bestR = 0;
    if (warm && kb_ >= 0 && kb_ - W - 2 >= 0 && kb_ + W + 2 < kN) {
        const double thrLin = std::pow(10.0, thr_ / 10), endLin = std::pow(10.0, (thr_ - 6.0) / 10);
        for (int j = kb_ - W; j <= kb_ + W; j++) {
            if (std::abs(j - kb_) <= kGuardBins) continue;
            const double r = P_[(size_t)j] / std::max(B_[(size_t)j], 1e-30f);
            if (r > bestR) { bestR = r; best = j; }
        }
        // broadband burst: many bins far from the peak above 10 dB
        if (best >= 0 && bestR > 10.0) {
            int nOut = 0;
            for (int j = kb_ - W; j <= kb_ + W; j++) {
                if (std::abs(j - kb_) <= kGuardBins || std::abs(j - best) <= 5) continue;
                if (P_[(size_t)j] > 10.0f * B_[(size_t)j]) nOut++;
            }
            bb = nOut > 8;
        }
        const double kbF = beaconHz_ / binHz_ + kC;
        auto dopAt = [&](int j) { return (j + parab(P_[(size_t)j - 1], P_[(size_t)j], P_[(size_t)j + 1]) - kbF) * binHz_; };
        if (bb) {
            if (!bbPrev_) rejected_++;
            bbPrev_ = true;
            if (!active_) pend_ = 0;
            else { lv_.push_back(-10.f); dop_.push_back(0.f); fr_.push_back(f); silent_++; }
        } else {
            bbPrev_ = false;
            if (!active_) {
                if (best >= 0 && bestR >= thrLin) {
                    if (pend_ > 0 && std::abs(best - pendBin_) <= 3) pend_++;
                    else { pend_ = 1; startFrame_ = f; lv_.clear(); dop_.clear(); fr_.clear(); }
                    pendBin_ = best;
                    lv_.push_back((float)db10(bestR)); dop_.push_back((float)dopAt(best)); fr_.push_back(f);
                    if (pend_ >= 2) { active_ = true; trackBin_ = best; lastAbove_ = f; silent_ = 0; }
                } else pend_ = 0;
            } else {
                int bj = -1; double bm = 0;
                for (int j = trackBin_ - 3; j <= trackBin_ + 3; j++) {
                    if (std::abs(j - kb_) <= kGuardBins || std::abs(j - kb_) > W) continue;
                    const double r = P_[(size_t)j] / std::max(B_[(size_t)j], 1e-30f);
                    if (r > bm) { bm = r; bj = j; }
                }
                lv_.push_back((float)std::max(-10.0, db10(bm))); dop_.push_back(bj >= 0 ? (float)dopAt(bj) : 0.f); fr_.push_back(f);
                activeDb_ = db10(bm);
                if (bj >= 0 && bm >= endLin) { lastAbove_ = f; trackBin_ = bj; silent_ = 0; }
                else silent_++;
                const double holdFrames = 0.26 * fps;
                if (silent_ > holdFrames || (double)(f - startFrame_) > 30.0 * fps) {
                    // finish
                    const size_t n = (size_t)(lastAbove_ - startFrame_ + 1);
                    active_ = false; pend_ = 0;
                    if (n >= 3 && n <= lv_.size()) {
                        RawPing p;
                        const double hopSec = kHop / rate_;
                        p.startSec = ((double)kN + (double)startFrame_ * kHop - kN / 2.0) / rate_;
                        p.durSec = (double)n * hopSec;
                        size_t ip = 0;
                        for (size_t i = 0; i < n; i++) if (lv_[i] > lv_[ip]) ip = i;
                        p.peakSnrDb = lv_[ip];
                        p.dopplerHz = dop_[ip];
                        // drift: least squares of the Doppler over the frames above the end level
                        double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
                        for (size_t i = 0; i < n; i++) if (lv_[i] >= thr_ - 6.0) {
                            const double tx = (double)(fr_[i] - startFrame_) * hopSec, y = dop_[i];
                            sx += tx; sy += y; sxx += tx * tx; sxy += tx * y; m++;
                        }
                        const double den = m * sxx - sx * sx;
                        p.driftHzPerSec = (m >= 4 && den > 1e-9) ? (m * sxy - sx * sy) / den : 0;
                        int nearBin = 0;
                        for (size_t i = 0; i < n; i++) if (lv_[i] >= lv_[ip] - 8.0f) nearBin++;
                        p.kind = (p.durSec >= 1.5 && nearBin * 2 >= (int)n) ? 1 : 0;
                        const size_t np = std::min<size_t>(n, kMsProfile);
                        p.profile.assign(np, -10.f);
                        for (size_t i = 0; i < n; i++) { float& q = p.profile[i * np / n]; q = std::max(q, lv_[i]); }
                        done_.push_back(std::move(p));
                    }
                    lv_.clear(); dop_.clear(); fr_.clear();
                }
            }
        }
    } else {
        pend_ = 0;
        if (active_ && kb_ < 0) { active_ = false; lv_.clear(); dop_.clear(); fr_.clear(); }
    }
    // the background of every bin: a slow mean that a rising echo cannot drag along (input clamped to 4x), frozen around a ping in progress
    if (!bb) {
        const float aB = f < 376 ? 1.f / (float)(f + 1) : 1.f / 376.f;
        const int freezeLo = active_ ? trackBin_ - 8 : (pend_ > 0 ? pendBin_ - 8 : 1 << 30), freezeHi = freezeLo + 16;
        for (int s = 0; s < kN; s++) {
            if (s >= freezeLo && s <= freezeHi) continue;
            const float p = std::min(P_[(size_t)s], 4.f * B_[(size_t)s] + 1e-30f);
            B_[(size_t)s] += aB * ((f < 376 ? P_[(size_t)s] : p) - B_[(size_t)s]);
        }
    }
    // waterfall row: +-hs bins around the beacon in kMsWfCols columns, 0 = at the background, 255 = 30 dB over it
    const int hs = wfHalfBins(kN);
    const double bpc = wfBinsPerColumn(kN);
    const int span = std::max(1, (int)std::ceil(bpc));
    if (warm && kb_ >= 0 && kb_ - hs - 5 >= 0 && kb_ + hs + 5 < kN) {
        for (int c = 0; c < kMsWfCols; c++) {
            uint8_t v = 0;
            const int lo = -hs + (int)std::floor(c * bpc);
            for (int o = lo; o <= lo + span - 1 && o <= hs; o++) {
                if (std::abs(o) <= kGuardBins) continue;
                const double r = P_[(size_t)(kb_ + o)] / std::max(B_[(size_t)(kb_ + o)], 1e-30f);
                const double d = std::max(0.0, std::min(30.0, db10(r)));
                v = std::max(v, (uint8_t)std::lround(d * 255.0 / 30.0));
            }
            row_[(size_t)c] = std::max(row_[(size_t)c], v);
        }
        if (f % 8 == 0) {
            for (int c = 0; c < kMsWfCols; c++) {
                double m = 0;
                const int lo = -hs + (int)std::floor(c * bpc);
                for (int o = lo; o <= lo + span - 1 && o <= hs; o++) m = std::max(m, (double)S_[(size_t)(kb_ + o)]);
                specOut_[(size_t)c] = (float)std::max(0.0, db10(m / std::max(noiseMean_, 1e-30)));
            }
        }
    }
    if (++rowFrames_ >= (int)std::lround(0.5 * fps)) {
        rowFrames_ = 0;
        std::copy(wf_.begin() + kMsWfCols, wf_.end(), wf_.begin());
        std::copy(row_.begin(), row_.end(), wf_.end() - kMsWfCols);
        std::fill(row_.begin(), row_.end(), 0);
        wfRows_++;
    }
    frames_++;
}

// ---------------------------------------------------------------- Counts, showers, CSV
void Counts::add(int64_t utc) {
    hour_[utc >= 0 ? utc / 3600 : -((-utc + 3599) / 3600)]++;
    day_[utc >= 0 ? utc / 86400 : -((-utc + 86399) / 86400)]++;
}

void Counts::fill(int64_t nowUtc, MsTelemetry& t) const {
    const int64_t h = nowUtc / 3600, d = nowUtc / 86400;
    t.hourly.assign(24, 0); t.daily.assign(30, 0);
    for (const auto& kv : hour_) { const int64_t a = h - kv.first; if (a >= 0 && a < 24) t.hourly[(size_t)(23 - a)] = kv.second; }
    for (const auto& kv : day_) { const int64_t a = d - kv.first; if (a >= 0 && a < 30) t.daily[(size_t)(29 - a)] = kv.second; }
    t.hourStart = (h - 23) * 3600;
    t.dayStart = (d - 29) * 86400;
}

static void civil(int64_t utc, int& y, int& m, int& d) {
    int64_t z = (utc >= 0 ? utc / 86400 : -((-utc + 86399) / 86400)) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}

static int doyOf(int m, int d) {   // day number in a common year
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return cum[m - 1] + d;
}

int msDayOfYear(int64_t utc) {
    int y, m, d;
    civil(utc, y, m, d);
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return doyOf(m, d) + (leap && m > 2 ? 1 : 0);
}

namespace {
struct ShowerDef { const char* name; int pm, pd, sm, sd, em, ed, zhr; bool day; };
// Approximate dates and rates from the IMO working list of meteor showers; the radio rates of the daytime showers are real, the eye cannot see them.
const ShowerDef kShowers[] = {
    {"Quadrantids", 1, 4, 12, 28, 1, 12, 110, false},
    {"Lyrids", 4, 22, 4, 14, 4, 30, 18, false},
    {"Eta Aquariids", 5, 6, 4, 19, 5, 28, 50, false},
    {"Arietids", 6, 7, 5, 22, 7, 2, 30, true},
    {"Zeta Perseids", 6, 9, 5, 20, 7, 5, 40, true},
    {"Beta Taurids", 6, 28, 6, 5, 7, 17, 25, true},
    {"Southern Delta Aquariids", 7, 30, 7, 12, 8, 23, 25, false},
    {"Perseids", 8, 12, 7, 17, 8, 24, 100, false},
    {"Daytime Sextantids", 9, 27, 9, 9, 10, 9, 5, true},
    {"Orionids", 10, 21, 10, 2, 11, 7, 20, false},
    {"Southern Taurids", 11, 5, 9, 28, 12, 2, 5, false},
    {"Northern Taurids", 11, 12, 10, 20, 12, 10, 5, false},
    {"Leonids", 11, 17, 11, 6, 11, 30, 15, false},
    {"Geminids", 12, 14, 12, 4, 12, 20, 150, false},
    {"Ursids", 12, 22, 12, 17, 12, 26, 10, false},
};
int wrap(int v) { v = ((v % 365) + 365) % 365; return v > 182 ? v - 365 : v; }
}

void msShowers(int64_t nowUtc, std::vector<MsShower>& out) {
    out.clear();
    int y, m, d;
    civil(nowUtc, y, m, d);
    const int today = doyOf(m, d);
    for (const auto& s : kShowers) {
        MsShower o;
        o.name = s.name; o.zhr = s.zhr; o.daytime = s.day;
        o.peakDayDelta = wrap(doyOf(s.pm, s.pd) - today);
        const int sd = doyOf(s.sm, s.sd), len = ((doyOf(s.em, s.ed) - sd) % 365 + 365) % 365;
        const int off = ((today - sd) % 365 + 365) % 365;
        o.active = off <= len;
        if (o.active || std::abs(o.peakDayDelta) <= 45) out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const MsShower& a, const MsShower& b) { return std::abs(a.peakDayDelta) < std::abs(b.peakDayDelta); });
}

static std::string iso(int64_t utc) {
    int y, m, d;
    civil(utc, y, m, d);
    const int sod = (int)(((utc % 86400) + 86400) % 86400);
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, sod / 3600, sod / 60 % 60, sod % 60);
    return b;
}

std::string csvHeader() { return "id,utc,duration_s,peak_snr_db,doppler_hz,drift_hz_per_s,type,beacon_hz\n"; }

std::string csvLine(const MsPing& p, double beaconHz) {
    char b[200];
    snprintf(b, sizeof b, "%u,%s,%.2f,%.1f,%+.1f,%+.1f,%s,%.0f\n", p.id, iso(p.wallTime).c_str(), (double)p.durationSec, (double)p.peakSnrDb, (double)p.dopplerHz,
             (double)p.driftHzPerSec, p.kind ? "overdense" : "underdense", beaconHz);
    return b;
}

} // namespace ms
} // namespace dect2
