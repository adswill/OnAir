// Loran-C detector: matched filter on the envelope, comb over the 8 pulses, fold over the GRI, peel the stations.
#include "dect2/eloran/eloran_rx.h"
#include <algorithm>
#include <cmath>

namespace dect2::loran {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kStationThresholdSigma = 6.5;   // peak of the comb over the median, in robust sigmas
constexpr double kGriZMin = 5.7;                 // the GRI fold must stand this far above its own spread
constexpr int kMaxStations = 8;
constexpr double kMinCodeScore = 0.35;           // a station must be explained by one of the four phase patterns this clearly

using cf = std::complex<float>;

double medianOf(std::vector<float> v) {
    if (v.empty()) return 0;
    const size_t k = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

struct Fold {
    std::vector<float> a;
    double z = 0;
};
}  // namespace

const char* phasePattern(bool master, char code) {
    if (master) return code == 'B' ? "+--+++++" : "++--+-+-";
    return code == 'B' ? "+-+-++--" : "+++++--+";
}

const char* chainName(int griUs) {
    static const struct { int gri; const char* name; } kChains[] = {
        {4970, "North-Western Russia Chayka"}, {5543, "Calcutta"}, {5930, "Canadian East Coast"}, {5960, "Northern Russia Chayka (closed)"}, {5970, "South Ural Chayka"}, {5980, "Russian-American (Chayka and Loran-C)"}, {5990, "Canadian West Coast"}, {6042, "Bombay"}, {6731, "Lessay"},
        {6780, "China South Sea"}, {7001, "Bo"}, {7030, "Saudi Arabia South"}, {7270, "Newfoundland East Coast"}, {7430, "China North Sea"}, {7499, "Sylt"},
        {7950, "Eastern Russia Chayka"}, {7960, "Gulf of Alaska"}, {7980, "Southeast U.S."}, {7990, "Mediterranean Sea"}, {8000, "Western Russia Chayka"}, {8290, "North Central U.S."},
        {8390, "China East Sea"}, {8830, "Saudi Arabia North"}, {8930, "North West Pacific"}, {8970, "Great Lakes"}, {9007, "Eidi (Faroe Islands)"},
        {9610, "South Central U.S."}, {9930, "East Asia"}, {9940, "U.S. West Coast"}, {9960, "Northeast U.S."}, {9990, "North Pacific"},
    };
    for (const auto& c : kChains) if (c.gri * 10 == griUs) return c.name;
    return nullptr;
}

double ninthPulseDelayUs(int state) {
    if (state < 0 || state > 31) return -1;
    return 50.625 * (state / 8) + 1.25 * (state % 8);
}

int ninthPulseState(double delayUs) {
    if (delayUs < -1.0 || delayUs > 165.0) return -1;
    int best = 0;
    double bestD = 1e9;
    for (int k = 0; k < 32; k++) {
        const double d = std::fabs(ninthPulseDelayUs(k) - delayUs);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

double pulseEnvelope(double t) {
    if (t <= 0 || t > 600) return 0;
    return t * t * std::exp(-2.0 * t / kTauUs) / (kTauUs * kTauUs * std::exp(-2.0));
}

Detector::Detector(double fs) : fs_(fs) {}

Result Detector::analyze(const cf* x, size_t n, int hintGriUs) const {
    Result res;
    const double fs = fs_;
    auto off = [&](double us) { return (long)std::lround(us * 1e-6 * fs); };
    const long D1 = off(1000.0);
    const int Lh = (int)off(kEnvLengthUs) + 1;
    if (fs < 100000.0 || n < (size_t)(0.45 * fs) || n <= (size_t)(Lh + 8 * D1)) return res;

    // matched filter on the complex baseband (the carrier is at 0 Hz): |sum h[t] x[n+t]| with the pulse envelope as template
    std::vector<float> h((size_t)Lh);
    double sumh = 0, sumh2 = 0;
    for (int t = 0; t < Lh; t++) {
        h[(size_t)t] = (float)pulseEnvelope((double)t / fs * 1e6);
        sumh += h[(size_t)t];
        sumh2 += (double)h[(size_t)t] * h[(size_t)t];
    }
    const double gainA = sumh2 / sumh;   // matched output for a pulse of peak amplitude 1
    const size_t nm = n - (size_t)Lh;
    std::vector<float> m(nm);
    std::vector<cf> mc(nm);
    double mmean = 0, npow = 0;
    for (size_t i = 0; i < nm; i++) {
        float sr = 0, si = 0;
        const cf* e = &x[i];
        for (int t = 0; t < Lh; t++) { sr += h[(size_t)t] * e[t].real(); si += h[(size_t)t] * e[t].imag(); }
        mc[i] = cf((float)(sr / sumh), (float)(si / sumh));
        m[i] = std::abs(mc[i]);
        mmean += m[i];
        npow += (double)m[i] * m[i];
    }
    mmean /= (double)nm;
    npow /= (double)nm;   // mean power of the matched output: the noise floor (the pulses fill a small part of the time)
    for (size_t i = 0; i < nm; i++) m[i] -= (float)mmean;

    // comb over the 8 pulses at 1000 us
    long offk[kGroupPulses + 1];
    for (int k = 0; k < kGroupPulses; k++) offk[k] = off(k * kPulseSpacingUs);
    offk[kGroupPulses] = off(9 * kPulseSpacingUs);   // the master's 9th pulse
    const size_t nc = nm - (size_t)offk[7];
    std::vector<float> c(nc);
    for (size_t i = 0; i < nc; i++) {
        float s = 0;
        for (int k = 0; k < kGroupPulses; k++) s += m[i + (size_t)offk[k]];
        c[i] = s;
    }

    // coarse scan of the GRI on the comb pooled to about 40 us
    const size_t W = (size_t)std::max(1L, off(40.0));
    const size_t np = nc / W;
    std::vector<float> cp(np, 0.f);
    for (size_t b = 0; b < np; b++) {
        float s = 0;
        for (size_t q = 0; q < W; q++) s += c[b * W + q];
        cp[b] = s / (float)W;
    }
    const double dtUs = (double)W / fs * 1e6;
    auto foldZ = [&](int griCode, std::vector<float>& F) -> double {
        const double step = griCode * 10.0 / dtUs;
        const size_t Lb = (size_t)std::floor(step);
        if (Lb < 8 || Lb >= np) return 0;
        long M = (long)std::floor(((double)np - (double)Lb) / step) + 1;
        while (M > 0 && (size_t)std::lround((double)(M - 1) * step) + Lb > np) M--;
        if (M < 4) return 0;
        F.assign(Lb, 0.f);
        for (long i = 0; i < M; i++) {
            const float* src = &cp[(size_t)std::lround((double)i * step)];
            for (size_t j = 0; j < Lb; j++) F[j] += src[j];
        }
        double s1 = 0, s2 = 0, mx = -1e30;
        for (size_t j = 0; j < Lb; j++) { s1 += F[j]; s2 += (double)F[j] * F[j]; mx = std::max(mx, (double)F[j]); }
        const double mu = s1 / (double)Lb;
        const double sd = std::sqrt(std::max(1e-30, s2 / (double)Lb - mu * mu));
        return (mx - mu) / sd;
    };
    int gri = 0;
    double zBest = 0;
    std::vector<float> Ftmp;
    if (hintGriUs >= 40000 && hintGriUs <= 99990) {
        zBest = foldZ(hintGriUs / 10, Ftmp);
        if (zBest >= kGriZMin) gri = hintGriUs;
    }
    if (gri == 0) {
        zBest = 0;
        int best = 0;
        for (int p = 4000; p <= 9999; p++) {
            const double z = foldZ(p, Ftmp);
            if (z > zBest) { zBest = z; best = p; }
        }
        // a GRI also folds at twice its period (Chayka 4970 = 9940 / 2): take the half when it folds nearly as well. A true 9940 folded at 4970
        // puts its groups over empty halves and scores about 0.7 of its own fold
        if (best % 2 == 0 && best / 2 >= 4000) {
            const double zh = foldZ(best / 2, Ftmp);
            if (zh >= 0.85 * zBest) { best /= 2; zBest = zh; }
        }
        if (best != 0 && zBest >= kGriZMin) gri = best * 10;
    }
    res.zScore = zBest;
    if (gri == 0) return res;
    res.griUs = gri;

    // fine fold at full rate, with the fractional position of every period interpolated
    const double stepS = gri * 1e-6 * fs;
    const size_t Lf = (size_t)std::lround(stepS);
    long M = 0;
    while ((size_t)std::floor((double)M * stepS) + Lf + 2 <= nm) M++;
    if (M < 4) { res.griUs = 0; return res; }
    std::vector<float> Fm(Lf, 0.f);
    for (long i = 0; i < M; i++) {
        const double pos = (double)i * stepS;
        const size_t b = (size_t)std::floor(pos);
        const float f = (float)(pos - (double)b);
        for (size_t j = 0; j < Lf; j++) {
            Fm[j] += (1.f - f) * m[b + j] + f * m[b + j + 1];
        }
    }
    auto wrap = [&](long v) { v %= (long)Lf; return v < 0 ? v + (long)Lf : v; };
    auto comb = [&](std::vector<float>& C) {
        C.assign(Lf, 0.f);
        for (size_t j = 0; j < Lf; j++) {
            float s = 0;
            for (int k = 0; k < kGroupPulses; k++) s += Fm[(size_t)wrap((long)j + offk[k])];
            C[j] = s;
        }
    };
    std::vector<float> C;
    comb(C);
    const double med0 = medianOf(C);
    std::vector<float> dev(Lf);
    for (size_t j = 0; j < Lf; j++) dev[j] = std::fabs(C[j] - (float)med0);
    const double sig0 = std::max(1e-12, 1.4826 * medianOf(dev));
    const double thr = med0 + kStationThresholdSigma * sig0;
    const long peelHalf = off(350.0);
    const double firstPeak = *std::max_element(C.begin(), C.end());

    struct Raw { double pos; bool ninth; float margin; double wT; };
    std::vector<Raw> raws;
    for (int s = 0; s < kMaxStations; s++) {
        size_t jb = 0;
        for (size_t j = 1; j < Lf; j++) if (C[j] > C[jb]) jb = j;
        if (C[jb] < thr || C[jb] < 0.05 * firstPeak) break;
        const double y0 = C[(size_t)wrap((long)jb - 1)], y1 = C[jb], y2 = C[(size_t)wrap((long)jb + 1)];
        const double den = y0 - 2 * y1 + y2;
        double delta = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0;
        delta = std::max(-0.5, std::min(0.5, delta));
        const double pos = (double)jb + delta;
        // the master's 9th pulse
        const double vmean = y1 / kGroupPulses;
        float v9 = -1e30f;
        for (long d = -3; d <= 3; d++) v9 = std::max(v9, Fm[(size_t)wrap((long)jb + offk[8] + d)]);
        const double sig1 = sig0 / std::sqrt((double)kGroupPulses);
        const bool ninth = v9 > std::max(0.5 * vmean, 3.0 * sig1);
        // the tuning error rotates the carrier from pulse to pulse (omega x 1 ms): the product of neighbouring pulses has that rotation and a sign of +-1, which the square removes,
        // so half the angle of the summed squares is the rotation per pulse (unambiguous within +-250 Hz)
        double wT = 0;
        {
            std::complex<double> acc(0, 0);
            const long pI0 = (long)std::lround(pos);
            for (long i = 0; i < M; i++) {
                const long bi = (long)std::lround((double)i * stepS) + pI0;
                for (int k = 0; k < kGroupPulses - 1; k++) {
                    const long i0 = bi + offk[k], i1 = bi + offk[k + 1];
                    if (i0 < 0 || i1 >= (long)nm) continue;
                    const std::complex<double> d = std::complex<double>(mc[(size_t)i1]) * std::conj(std::complex<double>(mc[(size_t)i0]));
                    acc += d * d;
                }
            }
            if (std::abs(acc) > 0) wT = 0.5 * std::arg(acc);
        }
        raws.push_back({pos, ninth, (float)(20.0 * std::log10((y1 - med0) / (kStationThresholdSigma * sig0))), wT});
        for (int k = 0; k < (ninth ? 9 : 8); k++)
            for (long d = -peelHalf; d <= peelHalf; d++) Fm[(size_t)wrap((long)jb + offk[k == 8 ? 8 : k] + d)] = 0.f;
        comb(C);
    }
    if (raws.empty()) { res.griUs = 0; return res; }

    // coherent refinement of every station: sum the 8 pulses with the sign pattern of each hypothesis (master / secondary, A / B in
    // alternate groups) and search the position over +-2.5 ms, which also repairs a comb that locked one or two pulses off.
    // The best hypothesis gives the code, the best position the arrival time.
    const long Q = off(2500.0);
    struct Ref { double pos; bool ninth; float margin; bool codeMaster; int a; double acc; double second; double wT; };
    std::vector<Ref> refs;
    std::vector<double> qacc((size_t)(2 * Q + 1));
    for (const Raw& r : raws) {
        const long pI = (long)std::lround(r.pos);
        cf rot[kGroupPulses];
        std::complex<double> rotD[kGroupPulses];
        for (int k = 0; k < kGroupPulses; k++) { rotD[k] = std::polar(1.0, -(double)k * r.wT); rot[k] = cf((float)rotD[k].real(), (float)rotD[k].imag()); }
        double bestAcc = -1, secondAcc = 0;
        long bestQ = 0;
        int bestMst = 0, bestA = 0;
        std::vector<double> bestCurve;
        for (int mst = 0; mst < 2; mst++)
            for (int a = 0; a < 2; a++) {
                std::fill(qacc.begin(), qacc.end(), 0.0);
                for (long i = 0; i < M; i++) {
                    const long bi = (long)std::lround((double)i * stepS) + pI;
                    const int cl = (int)(i & 1);
                    const char* pat = phasePattern(mst == 1, (cl == 0 ? a : 1 - a) ? 'B' : 'A');
                    for (long q = -Q; q <= Q; q++) {
                        const long i0 = bi + q;
                        if (i0 < 0 || i0 + offk[7] >= (long)nm) continue;
                        float sr = 0, si = 0;
                        for (int k = 0; k < kGroupPulses; k++) {
                            const cf v = mc[(size_t)(i0 + offk[k])] * rot[k];
                            if (pat[k] == '+') { sr += v.real(); si += v.imag(); } else { sr -= v.real(); si -= v.imag(); }
                        }
                        qacc[(size_t)(q + Q)] += (double)sr * sr + (double)si * si;
                    }
                }
                for (size_t qi = 0; qi < qacc.size(); qi++) {
                    if (qacc[qi] > bestAcc) {
                        if (mst != bestMst || a != bestA) secondAcc = std::max(secondAcc, bestAcc);
                        bestAcc = qacc[qi]; bestQ = (long)qi - Q; bestMst = mst; bestA = a;
                        bestCurve = qacc;
                    }
                }
            }
        if (bestAcc <= 0 || bestCurve.empty()) continue;
        // fine search with the winning hypothesis: the position moves in steps of a quarter sample, mc is interpolated with a cubic
        auto groupPow = [&](int mst, int a, double posReal) {
            double acc = 0;
            const double d1 = 1000.0 * 1e-6 * fs;
            for (long i = 0; i < M; i++) {
                const int cl = (int)(i & 1);
                const char* pat = phasePattern(mst == 1, (cl == 0 ? a : 1 - a) ? 'B' : 'A');
                const double t0 = (double)i * stepS + posReal;
                double sr = 0, si = 0;
                bool ok = true;
                for (int k = 0; k < kGroupPulses; k++) {
                    const double t = t0 + k * d1;
                    const long i1 = (long)std::floor(t);
                    if (i1 < 1 || i1 + 2 >= (long)nm) { ok = false; break; }
                    const float f = (float)(t - (double)i1);
                    // cubic (Catmull-Rom) interpolation of the matched output
                    const cf p0 = mc[(size_t)i1 - 1], p1 = mc[(size_t)i1], p2 = mc[(size_t)i1 + 1], p3 = mc[(size_t)i1 + 2];
                    const cf v = (p1 + 0.5f * f * ((p2 - p0) + f * ((2.f * p0 - 5.f * p1 + 4.f * p2 - p3) + f * (3.f * (p1 - p2) + p3 - p0)))) * rot[k];
                    const double sg = pat[k] == '+' ? 1.0 : -1.0;
                    sr += sg * v.real();
                    si += sg * v.imag();
                }
                if (ok) acc += sr * sr + si * si;
            }
            return acc;
        };
        const double coarse = (double)(pI + bestQ);
        double fineBest = -1, fineAt = 0;
        double ys[17];
        for (int j = 0; j < 17; j++) {
            ys[j] = groupPow(bestMst, bestA, coarse + (j - 8) * 0.25);
            if (ys[j] > fineBest) { fineBest = ys[j]; fineAt = j; }
        }
        double delta = (fineAt - 8) * 0.25;
        if (fineAt > 0 && fineAt < 16) {
            const double y0 = ys[(int)fineAt - 1], y1 = ys[(int)fineAt], y2 = ys[(int)fineAt + 1];
            const double den = y0 - 2 * y1 + y2;
            if (std::fabs(den) > 1e-18) delta += std::max(-0.25, std::min(0.25, 0.25 * 0.5 * (y0 - y2) / den));
        }
        const double posFine = coarse + delta;
        double sec = 0;
        for (int mst = 0; mst < 2; mst++)
            for (int a = 0; a < 2; a++)
                if (!(mst == bestMst && a == bestA)) sec = std::max(sec, groupPow(mst, a, posFine));
        bestAcc = groupPow(bestMst, bestA, posFine);
        (void)secondAcc;
        refs.push_back({posFine, r.ninth, r.margin, bestMst == 1, bestA, bestAcc, sec, r.wT});
    }
    if (refs.empty()) { res.griUs = 0; return res; }
    // drop duplicates (two peaks that are the same station)
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.acc > b.acc; });
    std::vector<Ref> kept;
    for (const Ref& r : refs) {
        bool dup = false;
        for (const Ref& k : kept) {
            double d = std::fabs(std::fmod(r.pos - k.pos, (double)Lf));
            d = std::min(d, (double)Lf - d);
            if (d < (double)off(400.0)) dup = true;
        }
        if (!dup) kept.push_back(r);
    }
    for (const Ref& r : kept) {
        if (!(r.acc > 0 && 1.0 - r.second / r.acc >= kMinCodeScore)) continue;
        Station st;
        double pos = std::fmod(r.pos, (double)Lf);
        if (pos < 0) pos += (double)Lf;
        st.absUs = pos / fs * 1e6;
        st.marginDb = r.margin;
        st.codeMaster = r.codeMaster;
        st.ninth = r.ninth;
        st.hypA = r.a;
        st.cfoHz = r.wT / (2.0 * kPi * 1e-3);
        st.master = r.ninth || r.codeMaster;   // narrowed below
        const long lastCl = (M - 1) & 1;
        st.code = (lastCl == 0 ? r.a : 1 - r.a) ? 'B' : 'A';
        st.codeScore = r.acc > 0 ? (float)std::max(0.0, 1.0 - r.second / r.acc) : 0.f;
        st.groupsUsed = (int)M;
        const double pw = std::max(0.0, r.acc / (double)M - kGroupPulses * npow);
        st.levelDb = (float)(20.0 * std::log10(std::max(1e-9, std::sqrt(pw) / (kGroupPulses * gainA))));
        res.stations.push_back(st);
        res.stations.back().master = r.codeMaster;
        if (!r.codeMaster && r.ninth) res.stations.back().master = false;
    }

    if (res.stations.empty()) { res.griUs = 0; return res; }

    // the reference: the master (phase code of a master), the strongest one if several; if none, the strongest station with a 9th pulse
    size_t ref = 0;
    bool haveMaster = false;
    for (size_t i = 0; i < res.stations.size(); i++)
        if (res.stations[i].master && (!haveMaster || res.stations[i].levelDb > res.stations[ref].levelDb)) { ref = i; haveMaster = true; }
    if (!haveMaster)
        for (size_t i = 0; i < res.stations.size(); i++)
            if (res.stations[i].ninth && (!haveMaster || res.stations[i].levelDb > res.stations[ref].levelDb)) { ref = i; haveMaster = true; }
    for (size_t i = 0; i < res.stations.size(); i++) res.stations[i].master = haveMaster && i == ref;
    res.masterFound = haveMaster;
    const double refPos = res.stations[ref].absUs;
    const double griD = (double)gri;
    for (Station& s : res.stations) {
        double d = std::fmod(s.absUs - refPos, griD);
        if (d < 0) d += griD;
        s.toaUs = d;
    }
    std::stable_sort(res.stations.begin(), res.stations.end(), [](const Station& a, const Station& b) {
        if (a.master != b.master) return a.master;
        return a.toaUs < b.toaUs;
    });

    // averaged pulse envelope of the first station: all its pulses in all groups, carrier phase removed with the group's own phase
    {
        const long pI = (long)std::lround(res.stations[0].absUs / 1e6 * fs);
        const double wTenv = res.stations[0].cfoHz * 2.0 * kPi * 1e-3;
        const long u0 = -off(50.0), u1 = off(400.0);
        res.envStartUs = -50.0;
        res.envStepUs = 1e6 / fs;
        std::vector<double> acc((size_t)(u1 - u0 + 1), 0.0);
        long used = 0;
        for (long i = 0; i < M; i++) {
            const long bi = (long)std::lround((double)i * stepS) + pI;
            if (bi + u0 < 0 || bi + offk[7] + u1 >= (long)n) continue;
            const char* pat = nullptr;
            // the pattern of this group: the refinement's winner, class by class
            const int cl = (int)(i & 1);
            const int a = res.stations[0].hypA;
            pat = phasePattern(res.stations[0].codeMaster, (cl == 0 ? a : 1 - a) ? 'B' : 'A');
            std::complex<double> ph(0, 0);
            for (int k = 0; k < kGroupPulses; k++) {
                const std::complex<double> v = std::complex<double>(mc[(size_t)(bi + offk[k])]) * std::polar(1.0, -(double)k * wTenv);
                ph += (pat[k] == '+' ? 1.0 : -1.0) * v;
            }
            if (std::abs(ph) < 1e-20) continue;
            ph /= std::abs(ph);
            for (int k = 0; k < kGroupPulses; k++)
                for (long u = u0; u <= u1; u++)
                    acc[(size_t)(u - u0)] += (pat[k] == '+' ? 1.0 : -1.0) * (std::complex<double>(x[(size_t)(bi + offk[k] + u)]) * std::polar(1.0, -(double)k * wTenv) * std::conj(ph)).real();
            used++;
        }
        for (double v : acc) res.pulseEnv.push_back((float)(used > 0 ? v / (double)(used * kGroupPulses) : 0.0));
    }
    res.found = true;
    (void)kPi;
    return res;
}

}  // namespace dect2::loran
