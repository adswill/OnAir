// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP burst receiver (see kapp_rx.h).
#include "dect2/kapp/kapp_rx.h"
#include <algorithm>
#include <cmath>

namespace dect2::kapp {

namespace {
using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

const std::vector<float>& matchedTaps() {
    static const std::vector<float> g = [] {
        std::vector<float> t = rrcShape();
        for (float& v : t) v /= (float)kSps;   // transmit taps have sum of squares kSps: this gives unit gain at the symbol instant
        return t;
    }();
    return g;
}

const std::vector<cf>& zcHalf(int half) {
    static const std::vector<cf> a = kris::zadoffChu(kZcRoot1, kZcLen);
    static const std::vector<cf> b = kris::zadoffChu(kZcRoot2, kZcLen);
    return half == 0 ? a : b;
}
double zcEnergy(int half) {
    static const double e[2] = {
        [] { double s = 0; for (const cf& v : zcHalf(0)) s += std::norm(v); return s; }(),
        [] { double s = 0; for (const cf& v : zcHalf(1)) s += std::norm(v); return s; }()};
    return e[half];
}

bool headerPlausible(const BurstHeader& h) {
    const Profile* p = profileById(h.profile);
    if (!p) return false;
    if (h.blocks < 1) return false;
    if (h.flags & kFlagAck) return h.profile == 0 && h.blocks == 1;
    return h.blocks <= std::max(1, maxBlocksForMs(*p, 1000.0));
}
}  // namespace

BurstReceiver::BurstReceiver() : BurstReceiver(Config{}) {}

BurstReceiver::BurstReceiver(const Config& cfg) : cfg_(cfg) {
    if (std::fabs(cfg_.sampleRate - kInternalRate) > 1e-6) rs_ = std::make_unique<Resampler>(cfg_.sampleRate, kInternalRate);
}

BurstReceiver::~BurstReceiver() = default;

void BurstReceiver::reset() {
    raw_.clear();
    mf_.clear();
    cs_.clear();
    absBase_ = 0;
    scan_ = 0;
    havePeak_ = false;
    needMf_ = 0;
    queue_.clear();
    st_ = RxStats{};
    if (std::fabs(cfg_.sampleRate - kInternalRate) > 1e-6) rs_ = std::make_unique<Resampler>(cfg_.sampleRate, kInternalRate);
}

std::vector<RxBurst> BurstReceiver::takeBursts() {
    std::vector<RxBurst> out;
    out.swap(queue_);
    return out;
}

void BurstReceiver::feed(const cf* x, size_t n) {
    if (n == 0) return;
    if (rs_) {
        const std::vector<cf> y = rs_->process(x, n);
        raw_.insert(raw_.end(), y.begin(), y.end());
        st_.samples += y.size();
    } else {
        raw_.insert(raw_.end(), x, x + n);
        st_.samples += n;
    }
    process();
}

void BurstReceiver::process() {
    extendFilter();
    for (;;) {
        if (!havePeak_ && !scanForPeak()) break;
        if (mf_.size() < needMf_) break;   // waiting for the rest of the header or payload
        decodeAtPeak();
        if (!havePeak_) trim();
    }
}

void BurstReceiver::extendFilter() {
    const std::vector<float>& g = matchedTaps();
    const long L = (long)g.size();
    const long c = (L - 1) / 2;
    if (raw_.size() <= (size_t)c) return;
    const size_t target = raw_.size() - (size_t)c;
    const size_t n0 = mf_.size();
    if (target <= n0) return;
    mf_.resize(target);
    cs_.resize(target);
    const float* x = reinterpret_cast<const float*>(raw_.data());
    for (size_t n = n0; n < target; ++n) {
        long lo = (long)n + c - (L - 1);
        if (lo < 0) lo = 0;
        const long hi = (long)n + c;
        float re = 0.f, im = 0.f;
        for (long i = lo; i <= hi; ++i) {
            const float gv = g[(size_t)((long)n + c - i)];
            re += gv * x[2 * i];
            im += gv * x[2 * i + 1];
        }
        mf_[n] = cf{re, im};
        const double p = (double)re * re + (double)im * im;
        cs_[n] = p + (n >= 8 ? cs_[n - 8] : 0.0);
    }
}

// normalised correlation of one Zadoff-Chu half starting at mf_ index n (half 1 starts 127 symbols later)
float BurstReceiver::corrHalf(size_t n, int half) const {
    const std::vector<cf>& z = zcHalf(half);
    const size_t base = n + (size_t)(kSps * kZcLen * half);
    float re = 0.f, im = 0.f;
    for (int k = 0; k < kZcLen; ++k) {
        const cf& m = mf_[base + (size_t)(kSps * k)];
        const cf& r = z[(size_t)k];
        re += m.real() * r.real() + m.imag() * r.imag();
        im += m.imag() * r.real() - m.real() * r.imag();
    }
    const double e = cs_[base + (size_t)(kSps * (kZcLen - 1))] - (base >= (size_t)kSps ? cs_[base - (size_t)kSps] : 0.0);
    if (e < 1e-12) return 0.f;
    return (float)(((double)re * re + (double)im * im) / (e * zcEnergy(half)));
}

float BurstReceiver::metricAt(size_t n) const { return corrHalf(n, 0) + corrHalf(n, 1); }

namespace {
constexpr int kBlk = 8;                        // symbols per block
constexpr int kNBlk = (2 * kZcLen) / kBlk;     // 31 blocks of the 254 preamble symbols
constexpr int kHalfBlk = kZcLen / kBlk;        // 15 blocks in the first half
}

bool BurstReceiver::blockSums(size_t n, std::complex<double>* b) const {
    const std::vector<cf>& pre = preambleSymbols();
    if (n + (size_t)(kSps * (kNBlk * kBlk)) >= mf_.size()) return false;
    for (int j = 0; j < kNBlk; ++j) {
        cd acc{0, 0};
        for (int i = 0; i < kBlk; ++i) {
            const int k = j * kBlk + i;
            acc += cd(mf_[n + (size_t)(kSps * k)]) * std::conj(cd(pre[(size_t)k]));
        }
        b[j] = acc;
    }
    return true;
}

static double stridedEnergy(const std::vector<double>& cs, size_t n, int count) {
    const size_t last = n + (size_t)(kSps * (count - 1));
    return cs[last] - (n >= (size_t)kSps ? cs[n - (size_t)kSps] : 0.0);
}

double BurstReceiver::blockMetricHalf(size_t n) const {
    const std::vector<cf>& pre = preambleSymbols();
    const int syms = kHalfBlk * kBlk;
    double sum = 0;
    for (int j = 0; j < kHalfBlk; ++j) {
        cd acc{0, 0};
        for (int i = 0; i < kBlk; ++i) {
            const int k = j * kBlk + i;
            acc += cd(mf_[n + (size_t)(kSps * k)]) * std::conj(cd(pre[(size_t)k]));
        }
        sum += std::norm(acc);
    }
    const double e = stridedEnergy(cs_, n, syms);
    if (e < 1e-12) return 0.0;
    return sum / e;   // noise only: mean 1 (a block of kBlk symbols adds kBlk times the sample power, kHalfBlk blocks cover kHalfBlk x kBlk symbols)
}

double BurstReceiver::blockMetricFull(size_t n) const {
    cd b[kNBlk];
    if (!blockSums(n, b)) return 0.0;
    double sum = 0;
    for (int j = 0; j < kNBlk; ++j) sum += std::norm(b[j]);
    const double e = stridedEnergy(cs_, n, kNBlk * kBlk);
    if (e < 1e-12) return 0.0;
    return sum / e;
}

double BurstReceiver::blockOmega(size_t n) const {
    cd b[kNBlk];
    if (!blockSums(n, b)) return 0.0;
    cd acc{0, 0};
    for (int j = 0; j + 1 < kNBlk; ++j) acc += b[j + 1] * std::conj(b[j]);
    return std::arg(acc) / (double)kBlk;
}

double BurstReceiver::cohMetric(size_t n, double wh) const {
    const std::vector<cf>& pre = preambleSymbols();
    if (n + (size_t)(kSps * kPreambleSyms) >= mf_.size()) return 0.0;
    cd acc{0, 0};
    for (int k = 0; k < kPreambleSyms; ++k)
        acc += cd(mf_[n + (size_t)(kSps * k)]) * std::conj(cd(pre[(size_t)k])) * std::polar(1.0, -wh * k);
    const double e = stridedEnergy(cs_, n, kPreambleSyms);
    if (e < 1e-12) return 0.0;
    return std::norm(acc) / (e * (double)kPreambleSyms);
}

void BurstReceiver::refinePeak(size_t coarse) {
    // Two hypotheses, no offset and the offset read from the blocks; the one that correlates the whole preamble better wins (the block estimate is noisy
    // at very low Es/N0, where the coherent halves already see a small offset).
    auto search = [&](double wh, size_t& best, double& bestV) {
        bestV = -1;
        best = coarse;
        for (long d = -4; d <= 4; ++d) {
            if ((long)coarse + d < 8) continue;
            const size_t k = (size_t)((long)coarse + d);
            const double v = cohMetric(k, wh);
            if (v > bestV) { bestV = v; best = k; }
        }
    };
    size_t b0, b1;
    double v0, v1;
    search(0.0, b0, v0);
    const double whBlk = blockOmega(coarse);
    search(whBlk, b1, v1);
    size_t best = b0;
    double wh = 0.0;
    if (v1 > 1.5 * v0) {   // a real offset: refine it at the better position
        best = b1;
        wh = blockOmega(b1);
    }
    double fr = 0.0;
    if (best > 8) {
        const double mm = cohMetric(best - 1, wh), m0 = cohMetric(best, wh), mp = cohMetric(best + 1, wh);
        const double d = mm - 2.0 * m0 + mp;
        if (d < 0.0) fr = std::max(-0.5, std::min(0.5, 0.5 * (mm - mp) / d));
    }
    peak_ = best;
    frac_ = fr;
    wh_ = wh;
}

bool BurstReceiver::scanForPeak() {
    const size_t need = (size_t)(kSps * (2 * kZcLen - 1) + 48);   // furthest sample one evaluation (and the peak search after it) touches
    const float th = cfg_.detectThreshold;
    size_t n = scan_;
    while (n + need < mf_.size()) {
        bool found = false;
        size_t best = n;
        const float a = corrHalf(n, 0);
        if (a > 0.4f * th && a + corrHalf(n, 1) > th) {
            found = true;
            float bestM = metricAt(n);
            for (size_t k = n + 1; k <= n + 24; ++k) {
                const float m = metricAt(k);
                if (m > bestM) {
                    bestM = m;
                    best = k;
                }
            }
        } else if ((n & 1) == 0 && blockMetricHalf(n) > 2.2 && blockMetricFull(n) > 2.4) {
            // a carrier offset too large for the coherent halves: found by the block powers (noise-only mean 1; the signal gives about 8 at full strength)
            found = true;
            double bestM = 0;
            for (size_t k = n; k <= n + 24; ++k) {
                const double m = blockMetricFull(k);
                if (m > bestM) {
                    bestM = m;
                    best = k;
                }
            }
        }
        if (found) {
            refinePeak(best);
            havePeak_ = true;
            needMf_ = peak_ + (size_t)(kSps * (kPreambleSyms + kHeaderSyms + 4)) + 8;
            scan_ = peak_;
            st_.detections++;
            return true;
        }
        ++n;
    }
    scan_ = n;
    return false;
}

// cubic (Lagrange) interpolation of the matched-filter output
cf BurstReceiver::symAt(double pos) const {
    const long i = (long)std::floor(pos);
    const double t = pos - (double)i;
    auto get = [&](long k) -> cf {
        if (k < 0 || k >= (long)mf_.size()) return cf{0.f, 0.f};
        return mf_[(size_t)k];
    };
    const float cm1 = (float)(-t * (t - 1.0) * (t - 2.0) / 6.0);
    const float c0 = (float)((t + 1.0) * (t - 1.0) * (t - 2.0) / 2.0);
    const float c1 = (float)(-(t + 1.0) * t * (t - 2.0) / 2.0);
    const float c2 = (float)((t + 1.0) * t * (t - 1.0) / 6.0);
    return get(i - 1) * cm1 + get(i) * c0 + get(i + 1) * c1 + get(i + 2) * c2;
}

void BurstReceiver::trim() {
    const size_t L = rrcShape().size();
    const size_t keep = scan_ > 2 * L ? scan_ - 2 * L : 0;
    if (keep < 65536) return;
    raw_.erase(raw_.begin(), raw_.begin() + (long)keep);
    absBase_ += keep;
    scan_ -= keep;
    mf_.clear();
    cs_.clear();
    extendFilter();
}

void BurstReceiver::decodeAtPeak() {
    const double step = (double)kSps * (1.0 + cfg_.clockPpm * 1e-6);
    const double pos0 = (double)peak_ + frac_;
    const int KN = kPreambleSyms + kHeaderSyms;   // 942 known symbols
    const int kc = KN / 2;                        // reference index of the known-symbol fit
    auto at = [&](long k) { return cf(cd(symAt(pos0 + step * (double)k)) * std::polar(1.0, -wh_ * (double)(k - kZcLen))); };   // the offset found at detection is removed here

    std::vector<cf> r((size_t)KN);
    for (int k = 0; k < KN; ++k) r[(size_t)k] = at(k);
    const std::vector<cf>& pre = preambleSymbols();

    // coarse carrier offset from the phase between the two halves
    cd s1{0, 0}, s2{0, 0};
    for (int k = 0; k < kZcLen; ++k) s1 += cd(r[(size_t)k]) * std::conj(cd(pre[(size_t)k]));
    for (int k = kZcLen; k < kPreambleSyms; ++k) s2 += cd(r[(size_t)k]) * std::conj(cd(pre[(size_t)k]));
    const double omega0 = std::arg(s2 * std::conj(s1)) / (double)kZcLen;

    // header: a few trial offsets around the coarse estimate, the first whose CRC passes wins
    static const int kTrial[] = {0, 1, -1, 2, -2, 3, -3};
    const double dOm = 2.0 * kPi * 10.0 / kSymRate;
    BurstHeader hdr;
    bool ok = false;
    double omega = omega0;
    uint64_t attempts = 0;
    for (int t : kTrial) {
        const double om = omega0 + t * dOm;
        cd acc{0, 0};
        for (int k = 0; k < kPreambleSyms; ++k) acc += cd(r[(size_t)k]) * std::conj(cd(pre[(size_t)k])) * std::polar(1.0, -om * (k - kZcLen));
        const double A = std::abs(acc) / (double)kPreambleSyms;
        if (A < 1e-9) continue;
        const double ph = std::arg(acc);
        double nv = 0;
        for (int k = 0; k < kPreambleSyms; ++k) {
            const cd y = cd(r[(size_t)k]) * std::polar(1.0, -om * (k - kZcLen) - ph);
            nv += std::norm(y - A * cd(pre[(size_t)k]));
        }
        nv /= (double)kPreambleSyms;
        const double n0 = std::max(nv / (A * A), 1e-3);
        std::vector<float> comb((size_t)kHeaderCodedBits, 0.f);
        for (int i = 0; i < kHeaderSyms; ++i) {
            const int k = kPreambleSyms + i;
            const cd y = cd(r[(size_t)k]) * std::polar(1.0, -om * (k - kZcLen) - ph);
            comb[(size_t)(i % kHeaderCodedBits)] += (float)(4.0 * (y.real() / A) / n0);
        }
        ++attempts;
        const std::vector<uint8_t> bits = kris::viterbiDecode(0, comb, true);
        if (bits.size() < (size_t)kHeaderInfoBits) continue;
        BurstHeader h;
        if (parseHeaderBits(bits.data(), h) && headerPlausible(h)) {
            hdr = h;
            omega = om;
            ok = true;
            break;
        }
    }

    if (!ok) {
        st_.headerAttempts += attempts;
        st_.headerFail++;
        havePeak_ = false;
        scan_ = peak_ + 20;
        return;
    }

    const Profile& P = *profileById(hdr.profile);
    const int nB = hdr.blocks;
    const int spb = symbolsPerBlock(P);
    const int Ptot = payloadSymbols(P, nB);
    const double lastPos = pos0 + step * (double)(KN + Ptot + 2);
    needMf_ = (size_t)lastPos + 4;
    if (mf_.size() < needMf_) return;   // wait for the payload (the header work is repeated then; counters are only committed below)

    st_.headerAttempts += attempts;
    st_.headerOk++;

    // fine carrier offset, phase, gain and noise from the 942 known symbols
    std::vector<cf> known(pre);
    {
        const std::vector<cf> hs = headerSymbols(hdr);
        known.insert(known.end(), hs.begin(), hs.end());
    }
    auto power = [&](double om) {
        cd acc{0, 0}, ph{1, 0};
        const cd rot = std::polar(1.0, -om);
        for (int k = 0; k < KN; ++k) {
            acc += cd(r[(size_t)k]) * std::conj(cd(known[(size_t)k])) * ph;
            ph *= rot;
        }
        return std::norm(acc);
    };
    {
        const double dStep = 2.0 * kPi * 1.0 / kSymRate;
        const int J = 24;
        std::vector<double> pw((size_t)(2 * J + 1));
        int bj = 0;
        for (int j = -J; j <= J; ++j) {
            pw[(size_t)(j + J)] = power(omega + j * dStep);
            if (pw[(size_t)(j + J)] > pw[(size_t)(bj + J)]) bj = j;
        }
        double off = 0.0;
        if (bj > -J && bj < J) {
            const double ym = pw[(size_t)(bj - 1 + J)], y0 = pw[(size_t)(bj + J)], yp = pw[(size_t)(bj + 1 + J)];
            const double d = ym - 2.0 * y0 + yp;
            if (d < 0.0) off = std::max(-0.5, std::min(0.5, 0.5 * (ym - yp) / d));
        }
        omega += (bj + off) * dStep;
    }
    cd a{0, 0};
    for (int k = 0; k < KN; ++k) a += cd(r[(size_t)k]) * std::conj(cd(known[(size_t)k])) * std::polar(1.0, -omega * (k - kc));
    a /= (double)KN;
    double nv = 0;
    for (int k = 0; k < KN; ++k) {
        const cd y = cd(r[(size_t)k]) * std::polar(1.0, -omega * (k - kc));
        nv += std::norm(y - a * cd(known[(size_t)k]));
    }
    nv /= (double)KN;
    const double a2 = std::max(std::norm(a), 1e-12);
    const double snr = 10.0 * std::log10(a2 / std::max(nv, 1e-12));
    const double n0 = std::max(nv / a2, 2e-4);

    // payload symbols, derotated and normalised by the known-symbol fit
    std::vector<cd> rp((size_t)Ptot);
    for (int i = 0; i < Ptot; ++i) {
        const int k = KN + i;
        rp[(size_t)i] = cd(at(k)) * std::polar(1.0, -omega * (k - kc)) / a;
    }

    // pilots: global residual frequency, then a sliding complex mean for phase and gain
    const int nP = (Ptot + 1) / kPilotPeriod;
    double theta = 0.0;   // residual phase step per pilot
    std::vector<cd> G((size_t)std::max(nP, 1), cd{1, 0});
    if (nP > 0) {
        std::vector<cd> q((size_t)nP);
        for (int j = 0; j < nP; ++j) q[(size_t)j] = rp[(size_t)(kPilotPeriod * j + kPilotGroup)];
        if (nP >= 8) {
            const double sstep = std::min(0.001, 0.25 * 2.0 * kPi / (double)nP);
            const int M = (int)std::ceil(0.02 / sstep);
            double best = -1.0;
            for (int m = -M; m <= M; ++m) {
                const double th = m * sstep;
                cd acc{0, 0}, ph{1, 0};
                const cd rot = std::polar(1.0, -th);
                for (int j = 0; j < nP; ++j) {
                    acc += q[(size_t)j] * ph;
                    ph *= rot;
                }
                const double pw = std::norm(acc);
                if (pw > best) {
                    best = pw;
                    theta = th;
                }
            }
        }
        std::vector<cd> pref((size_t)nP + 1, cd{0, 0});
        for (int j = 0; j < nP; ++j) pref[(size_t)j + 1] = pref[(size_t)j] + q[(size_t)j] * std::polar(1.0, -theta * j);
        const int K = P.bitsPerSym <= 2 ? 48 : (P.bitsPerSym == 4 ? 16 : (P.bitsPerSym == 6 ? 8 : 5));
        for (int j = 0; j < nP; ++j) {
            const int lo = std::max(0, j - K), hi = std::min(nP, j + K + 1);
            cd g = (pref[(size_t)hi] - pref[(size_t)lo]) / (double)(hi - lo);
            if (std::abs(g) < 1e-3) g = cd{1, 0};
            G[(size_t)j] = g;
        }
    }

    // equalise the data symbols (everything but the pilots)
    std::vector<cf> ds;
    ds.reserve((size_t)dataSymbols(P, nB));
    for (int i = 0; i < Ptot; ++i) {
        if (i % kPilotPeriod == kPilotGroup) continue;
        const double pp = (double)(i - kPilotGroup) / (double)kPilotPeriod;   // position in pilot units
        cd g{1, 0};
        if (nP > 0) {
            if (pp <= 0.0) g = G[0];
            else if (pp >= (double)(nP - 1)) g = G[(size_t)(nP - 1)];
            else {
                const int j = (int)std::floor(pp);
                const double f = pp - (double)j;
                g = G[(size_t)j] * (1.0 - f) + G[(size_t)j + 1] * f;
            }
        }
        const cd e = rp[(size_t)i] * std::polar(1.0, -theta * pp) / g;
        ds.push_back(cf{(float)e.real(), (float)e.imag()});
    }

    // blocks
    RxBurst out;
    out.callsign = hdr.callsign;
    out.profile = hdr.profile;
    out.seq = hdr.seq;
    out.flags = hdr.flags;
    out.nBlocks = nB;
    out.snrDb = (float)snr;
    out.cfoHz = (float)((omega + wh_) * kSymRate / (2.0 * kPi));
    out.startSample = absBase_ + (uint64_t)peak_;
    {   // a sample of the equalised symbols for the display
        const size_t stride = std::max<size_t>(1, ds.size() / 400);
        for (size_t i = 0; i < ds.size() && out.symbols.size() < 400; i += stride) out.symbols.push_back(ds[i]);
    }
    out.blockOk.assign((size_t)nB, 0);
    out.payload.assign((size_t)nB * (size_t)P.payloadBytes, 0);
    const int K = infoBitsPerBlock(P);
    std::vector<float> llr((size_t)spb * (size_t)P.bitsPerSym);
    std::vector<uint8_t> info((size_t)K);
    for (int b = 0; b < nB; ++b) {
        if ((size_t)(b + 1) * (size_t)spb > ds.size()) break;
        kris::qamDemapSoft(ds.data() + (size_t)b * (size_t)spb, (size_t)spb, P.mod, (float)n0, llr.data());
        const std::vector<uint8_t> pn = scramblerSequence(b, kBlockN);
        for (int i = 0; i < kBlockN; ++i) {
            if (pn[(size_t)i]) llr[(size_t)i] = -llr[(size_t)i];
        }
        std::fill(info.begin(), info.end(), (uint8_t)0);
        kris::ldpcDecode(kBlockN, P.rate, llr.data(), info.data(), 50);
        const std::vector<uint8_t> bytes = packBits(info.data(), (size_t)P.payloadBytes * 8);
        uint16_t rxCrc = 0;
        for (int i = 0; i < 16; ++i) rxCrc = (uint16_t)((rxCrc << 1) | (info[(size_t)(P.payloadBytes * 8 + i)] & 1u));
        if (kris::crc16(bytes.data(), bytes.size()) == rxCrc) {
            out.blockOk[(size_t)b] = 1;
            std::copy(bytes.begin(), bytes.end(), out.payload.begin() + (long)((size_t)b * (size_t)P.payloadBytes));
            st_.blocksOk++;
        } else {
            st_.blocksBad++;
        }
    }
    st_.bursts++;
    scan_ = std::min(raw_.size(), (size_t)(pos0 + step * (double)(KN + Ptot + kRampSyms)) + 1);
    havePeak_ = false;
    if (cb_) cb_(out);
    queue_.push_back(std::move(out));
}

}  // namespace dect2::kapp
