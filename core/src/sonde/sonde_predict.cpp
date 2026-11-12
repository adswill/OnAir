// Radiosonde landing prediction 4.0: see core/include/dect2/sonde/sonde_predict.h and docs/SONDE-PREDICT-4.0.md.
#include "dect2/sonde/sonde_predict.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace dect2 {
namespace sondepred {

namespace {
constexpr double kPi = 3.14159265358979323846, kD = kPi / 180.0, kR = 6371000.0;
constexpr double kGMR = 0.0341632;     // g0 * M / R* (K / m)
constexpr double kRd = 287.053;        // J / (kg K)
constexpr double kG = 9.80665;
double eastM(double lon, double lon0, double lat0) { return (lon - lon0) * kD * kR * std::cos(lat0 * kD); }
double northM(double lat, double lat0) { return (lat - lat0) * kD * kR; }
double distM(double la1, double lo1, double la2, double lo2) { return std::hypot(eastM(lo2, lo1, la1), northM(la2, la1)); }
void moveM(double& lat, double& lon, double de, double dn) {
    lat += dn / kR / kD;
    lon += de / (kR * std::cos(lat * kD)) / kD;
}
struct IsaLayer { double hb, tb, lapse, pb; };
const IsaLayer kIsa[] = {{0, 288.15, -0.0065, 101325.0}, {11000, 216.65, 0.0, 22632.06}, {20000, 216.65, 0.001, 5474.889},
                         {32000, 228.65, 0.0028, 868.0187}, {47000, 270.65, 0.0, 110.9063}, {51000, 270.65, -0.0028, 66.93887}};
const IsaLayer& isaLayer(double h) {
    int i = 0;
    while (i < 5 && h >= kIsa[i + 1].hb) i++;
    return kIsa[i];
}
}  // namespace

double isaTempK(double h) { const IsaLayer& L = isaLayer(h); return L.tb + L.lapse * (h - L.hb); }
double isaPressurePa(double h) {
    const IsaLayer& L = isaLayer(h);
    if (L.lapse == 0) return L.pb * std::exp(-kGMR * (h - L.hb) / L.tb);
    return L.pb * std::pow(L.tb / (L.tb + L.lapse * (h - L.hb)), kGMR / L.lapse);
}
double isaDensity(double h) { return isaPressurePa(h) / (kRd * isaTempK(h)); }

// ---- atmosphere -------------------------------------------------------------------------------------------------------------------
void Atmosphere::buildIsa() { build({}, {}, 250); }

void Atmosphere::build(const std::vector<double>& base, const std::vector<double>& tc, double layerM) {
    const int n = (int)((51000 - baseM) / stepM) + 1;
    rho.assign(n, 0);
    fromSonde = !base.empty() && base.size() == tc.size();
    std::vector<double> zc, off;   // measured centres and their offset from ISA
    if (fromSonde)
        for (size_t i = 0; i < base.size(); i++) { const double z = base[i] + layerM / 2; zc.push_back(z); off.push_back(tc[i] + 273.15 - isaTempK(z)); }
    auto tempAt = [&](double z) {
        double o = 0;
        if (!zc.empty()) {
            if (z <= zc.front()) o = off.front() * std::exp(-(zc.front() - z) / 3000.0);
            else if (z >= zc.back()) o = off.back() * std::exp(-(z - zc.back()) / 3000.0);
            else {
                const size_t j = std::upper_bound(zc.begin(), zc.end(), z) - zc.begin();
                const double u = (z - zc[j - 1]) / std::max(1e-6, zc[j] - zc[j - 1]);
                o = off[j - 1] + u * (off[j] - off[j - 1]);
            }
        }
        return std::max(150.0, isaTempK(z) + o);
    };
    double p = isaPressurePa(baseM), tPrev = tempAt(baseM);
    rho[0] = p / (kRd * tPrev);
    for (int k = 1; k < n; k++) {
        const double z = baseM + k * stepM, t = tempAt(z);
        p *= std::exp(-kG * stepM / (kRd * 0.5 * (t + tPrev)));
        rho[k] = p / (kRd * t);
        tPrev = t;
    }
}

double Atmosphere::density(double h) const {
    if (rho.empty()) return isaDensity(h);
    const double x = (h - baseM) / stepM;
    if (x <= 0) return rho.front() * std::exp(-x * stepM / 8000.0);
    const int n = (int)rho.size();
    if (x >= n - 1) return rho.back() * std::exp(-(x - (n - 1)) * stepM / 7000.0);
    const int i = (int)x; const double u = x - i;
    return rho[i] + u * (rho[i + 1] - rho[i]);
}

// ---- layer winds ------------------------------------------------------------------------------------------------------------------
int LayerWinds::okCount() const { int c = 0; for (const auto& l : layers) c += l.ok; return c; }
double LayerWinds::bottomM() const {
    for (size_t i = 0; i < layers.size(); i++) if (layers[i].ok) return (firstLayer + (double)i + 0.5) * layerM;
    return 0;
}
double LayerWinds::topM() const {
    for (size_t i = layers.size(); i-- > 0;) if (layers[i].ok) return (firstLayer + (double)i + 0.5) * layerM;
    return 0;
}
bool LayerWinds::at(double h, WindLayer& w, double maxGapM) const {
    if (layers.empty()) return false;
    const double x = h / layerM - 0.5 - firstLayer;   // in layer-centre units
    const int n = (int)layers.size();
    int lo = (int)std::floor(x), hi = lo + 1;
    const int maxGap = (int)std::ceil(maxGapM / layerM) + 1;
    int a = -1, b = -1;
    for (int i = std::min(lo, n - 1), k = 0; i >= 0 && k <= maxGap; i--, k++) if (layers[i].ok) { a = i; break; }
    for (int i = std::max(hi, 0), k = 0; i < n && k <= maxGap; i++, k++) if (layers[i].ok) { b = i; break; }
    if (a >= 0 && (x - a) * layerM > maxGapM) a = -1;
    if (b >= 0 && (b - x) * layerM > maxGapM) b = -1;
    if (a < 0 && b < 0) return false;
    if (a < 0 || b < 0 || a == b) { w = layers[a >= 0 ? a : b]; return true; }
    const WindLayer& A = layers[a]; const WindLayer& B = layers[b];
    const double u = std::min(1.0, std::max(0.0, (x - a) / (double)(b - a)));
    w = A;
    w.ve = A.ve + u * (B.ve - A.ve); w.vn = A.vn + u * (B.vn - A.vn);
    w.sigma = std::max(A.sigma, B.sigma) * (b - a > 1 ? 1.0 + 0.1 * (b - a - 1) : 1.0);
    w.tMid = A.tMid + u * (B.tMid - A.tMid);
    w.latMid = A.latMid + u * (B.latMid - A.latMid); w.lonMid = A.lonMid + u * (B.lonMid - A.lonMid);
    return true;
}

LayerWinds layerWinds(const std::vector<Fix>& f, size_t i0, size_t i1, double layerM, double minSpanS) {
    LayerWinds W; W.layerM = layerM;
    i1 = std::min(i1, f.size());
    if (i1 < i0 + 3 || layerM <= 0) return W;
    std::vector<size_t> idx;
    for (size_t i = i0; i < i1; i++) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return f[a].alt < f[b].alt; });
    const double aMin = f[idx.front()].alt, aMax = f[idx.back()].alt;
    W.firstLayer = (int)std::floor(aMin / layerM);
    const int last = (int)std::floor(aMax / layerM);
    W.layers.assign(last - W.firstLayer + 1, WindLayer());
    std::vector<double> alts(idx.size());
    for (size_t k = 0; k < idx.size(); k++) alts[k] = f[idx[k]].alt;
    std::vector<size_t> pts;
    for (int L = W.firstLayer; L <= last; L++) {
        const double base = L * layerM;
        WindLayer& out = W.layers[L - W.firstLayer];
        for (double pad = 0; pad <= layerM + 1e-6; pad += layerM / 4) {
            const size_t a = std::lower_bound(alts.begin(), alts.end(), base - pad) - alts.begin();
            const size_t b = std::lower_bound(alts.begin(), alts.end(), base + layerM + pad) - alts.begin();
            if (b < a + 3) continue;
            pts.assign(idx.begin() + a, idx.begin() + b);
            std::sort(pts.begin(), pts.end());
            const double span = f[pts.back()].t - f[pts.front()].t;
            if (span < minSpanS) continue;
            // least squares slope of east / north against time; one pass of outlier removal (GPS glitches)
            const double lat0 = f[pts.front()].lat, lon0 = f[pts.front()].lon, t0 = f[pts.front()].t;
            std::vector<char> use(pts.size(), 1);
            double ve = 0, vn = 0, se = 0, sn = 0, tm = 0, la = 0, lo = 0; int n = 0;
            for (int pass = 0; pass < 2; pass++) {
                double st = 0, sE = 0, sN = 0; n = 0;
                for (size_t k = 0; k < pts.size(); k++) if (use[k]) { const Fix& x = f[pts[k]]; st += x.t - t0; sE += eastM(x.lon, lon0, lat0); sN += northM(x.lat, lat0); n++; }
                if (n < 3) break;
                tm = st / n; const double eM = sE / n, nM = sN / n;
                double stt = 0, ste = 0, stn = 0;
                for (size_t k = 0; k < pts.size(); k++) if (use[k]) {
                    const Fix& x = f[pts[k]]; const double t = x.t - t0 - tm;
                    stt += t * t; ste += t * (eastM(x.lon, lon0, lat0) - eM); stn += t * (northM(x.lat, lat0) - nM);
                }
                if (stt <= 0) { n = 0; break; }
                ve = ste / stt; vn = stn / stt;
                double rr = 0; std::vector<double> res(pts.size(), 0);
                for (size_t k = 0; k < pts.size(); k++) {
                    const Fix& x = f[pts[k]]; const double t = x.t - t0 - tm;
                    const double re = eastM(x.lon, lon0, lat0) - eM - ve * t, rn = northM(x.lat, lat0) - nM - vn * t;
                    res[k] = std::hypot(re, rn);
                    if (use[k]) rr += res[k] * res[k];
                }
                const double rms = std::sqrt(rr / std::max(1, n - 2));
                se = sn = rms / std::sqrt(stt);
                if (pass == 0) {
                    bool drop = false;
                    for (size_t k = 0; k < pts.size(); k++) if (use[k] && res[k] > std::max(3.0 * rms, 25.0)) { use[k] = 0; drop = true; }
                    if (!drop) break;
                }
            }
            if (n < 3) continue;
            la = 0; lo = 0; int c = 0;
            for (size_t k = 0; k < pts.size(); k++) if (use[k]) { la += f[pts[k]].lat; lo += f[pts[k]].lon; c++; }
            out.ok = true; out.ve = ve; out.vn = vn; out.n = n;
            out.sigma = std::max(0.3, std::hypot(se, sn) / std::sqrt(2.0));
            out.tMid = t0 + tm; out.latMid = la / c; out.lonMid = lo / c;
            break;
        }
    }
    return W;
}

// ---- forecast winds ---------------------------------------------------------------------------------------------------------------
namespace {
bool levelAt(const std::vector<ForecastWinds::Level>& L, double h, double& ve, double& vn) {
    if (L.empty()) return false;
    if (h <= L.front().zM) { ve = L.front().ve; vn = L.front().vn; return true; }
    if (h >= L.back().zM) { ve = L.back().ve; vn = L.back().vn; return true; }
    const size_t j = std::upper_bound(L.begin(), L.end(), h, [](double v, const ForecastWinds::Level& l) { return v < l.zM; }) - L.begin();
    const auto& a = L[j - 1]; const auto& b = L[j];
    const double u = (h - a.zM) / std::max(1e-6, b.zM - a.zM);
    ve = a.ve + u * (b.ve - a.ve); vn = a.vn + u * (b.vn - a.vn);
    return true;
}
bool columnAt(const ForecastWinds::Column& c, double h, double t, double& ve, double& vn) {
    if (c.times.empty() || c.levels.size() != c.times.size()) return false;
    size_t j = std::upper_bound(c.times.begin(), c.times.end(), t) - c.times.begin();
    double e1, n1, e2, n2;
    if (j == 0 || j == c.times.size()) {
        const size_t k = j == 0 ? 0 : c.times.size() - 1;
        return levelAt(c.levels[k], h, ve, vn);
    }
    const bool a = levelAt(c.levels[j - 1], h, e1, n1), b = levelAt(c.levels[j], h, e2, n2);
    if (!a && !b) return false;
    if (!a) { ve = e2; vn = n2; return true; }
    if (!b) { ve = e1; vn = n1; return true; }
    const double u = (t - c.times[j - 1]) / std::max(1e-6, c.times[j] - c.times[j - 1]);
    ve = e1 + u * (e2 - e1); vn = n1 + u * (n2 - n1);
    return true;
}
}  // namespace

bool ForecastWinds::at(double lat, double lon, double h, double t, double& ve, double& vn) const {
    double sw = 0, se = 0, sn = 0;
    for (const Column& c : cols) {
        double e, n;
        if (!columnAt(c, h, t, e, n)) continue;
        const double d = distM(lat, lon, c.lat, c.lon), w = 1.0 / (d * d + 5000.0 * 5000.0);
        sw += w; se += w * e; sn += w * n;
    }
    if (sw <= 0) return false;
    ve = se / sw; vn = sn / sw;
    return true;
}
double ForecastWinds::topM() const {
    double top = 0;
    for (const Column& c : cols) for (const auto& L : c.levels) if (!L.empty()) top = std::max(top, L.back().zM);
    return top;
}

ForecastWinds windsFromPath(const std::vector<PathPoint>& p, const std::string& source) {
    ForecastWinds W; W.source = source;
    if (p.size() < 3) return W;
    size_t top = 0;
    for (size_t i = 0; i < p.size(); i++) if (p[i].altM > p[top].altM) top = i;
    size_t a = top, b = p.size();
    if (b - a < 3) { a = 0; b = top + 1; }   // no descent in it: the ascent
    if (b - a < 3) return W;
    ForecastWinds::Column c;
    std::vector<ForecastWinds::Level> L;
    double la = 0, lo = 0, tt = 0; int n = 0;
    for (size_t i = a; i + 1 < b; i++) {
        const double dt = p[i + 1].unixT - p[i].unixT;
        if (dt <= 0) continue;
        ForecastWinds::Level l;
        l.zM = 0.5 * (p[i].altM + p[i + 1].altM);
        l.ve = eastM(p[i + 1].lon, p[i].lon, p[i].lat) / dt; l.vn = northM(p[i + 1].lat, p[i].lat) / dt;
        if (std::hypot(l.ve, l.vn) > 200) continue;
        L.push_back(l); la += p[i].lat; lo += p[i].lon; tt += p[i].unixT; n++;
    }
    if (n < 2) return W;
    std::sort(L.begin(), L.end(), [](const ForecastWinds::Level& x, const ForecastWinds::Level& y) { return x.zM < y.zM; });
    c.lat = la / n; c.lon = lo / n; c.times.push_back(tt / n); c.levels.push_back(L);
    W.cols.push_back(c);
    return W;
}

// ---- descent fit ------------------------------------------------------------------------------------------------------------------
double DescentFit::kAt(double h) const {
    const double c = std::min(0.25, std::max(-0.25, slopePerKm * (h - atH) / 1000.0));
    return K * (1 + c);
}

DescentFit fitDescent(const std::vector<Fix>& f, size_t burst, const Atmosphere& atm, double tauS) {
    DescentFit r;
    if (burst >= f.size() || f.size() < burst + 3) return r;
    const double tB = f[burst].t, tNow = f.back().t, H = f.back().alt, r0 = atm.rho0();
    struct S { double x, k, w; };
    std::vector<S> s;
    size_t i = burst;
    while (i < f.size() && f[i].t < tB + 30) i++;
    while (i + 1 < f.size()) {
        size_t k = i + 1;
        while (k < f.size() && f[k].t - f[i].t < 4) k++;
        if (k >= f.size()) break;
        const double dt = f[k].t - f[i].t, v = -(f[k].alt - f[i].alt) / dt, h = 0.5 * (f[i].alt + f[k].alt), tm = 0.5 * (f[i].t + f[k].t);
        if (v > 0.5 && dt < 120) s.push_back({(h - H) / 1000.0, v * std::sqrt(atm.density(h) / r0), dt * std::exp(-(tNow - tm) / std::max(10.0, tauS))});
        i = k;
    }
    if (s.size() < 3) return r;
    std::vector<char> use(s.size(), 1);
    double a = 0, b = 0;
    for (int pass = 0; pass < 2; pass++) {
        double sw = 0, sx = 0, sxx = 0, sk = 0, sxk = 0;
        for (size_t j = 0; j < s.size(); j++) if (use[j]) { sw += s[j].w; sx += s[j].w * s[j].x; sxx += s[j].w * s[j].x * s[j].x; sk += s[j].w * s[j].k; sxk += s[j].w * s[j].x * s[j].k; }
        if (sw <= 0) return r;
        const double lam = 0.5 * sw;   // ridge on the slope: it needs a good spread of heights to move
        const double det = sw * (sxx + lam) - sx * sx;
        if (det <= 0) { a = sk / sw; b = 0; } else { a = (sk * (sxx + lam) - sx * sxk) / det; b = (sw * sxk - sx * sk) / det; }
        if (pass == 0) {
            std::vector<double> res;
            for (size_t j = 0; j < s.size(); j++) res.push_back(std::fabs(s[j].k - a - b * s[j].x));
            std::vector<double> sr = res; std::nth_element(sr.begin(), sr.begin() + sr.size() / 2, sr.end());
            const double lim = std::max(0.3, 4 * 1.4826 * sr[sr.size() / 2]);
            for (size_t j = 0; j < s.size(); j++) if (res[j] > lim) use[j] = 0;
        }
    }
    double sw = 0, sw2 = 0, sr2 = 0; int n = 0;
    for (size_t j = 0; j < s.size(); j++) if (use[j]) { const double e = s[j].k - a - b * s[j].x; sw += s[j].w; sw2 += s[j].w * s[j].w; sr2 += s[j].w * e * e; n++; }
    if (n < 3 || sw <= 0 || a <= 0.3) return r;
    const double neff = sw * sw / std::max(1e-12, sw2);
    r.ok = true; r.K = a; r.slopePerKm = std::min(0.05, std::max(-0.05, b / a)); r.atH = H; r.n = n;
    r.sigmaRel = std::sqrt(sr2 / sw / std::max(1.0, neff)) / a;
    r.noParachute = a > 11.0;
    return r;
}

// ---- Kalman filter on the drift ---------------------------------------------------------------------------------------------------
namespace {
struct Kf2 {   // position and velocity of one axis
    double x = 0, v = 0, P00 = 0, P01 = 0, P11 = 0;
    void init(double z, double vz, double R, double vVar) { x = z; v = vz; P00 = R; P01 = 0; P11 = vVar; }
    void predict(double dt, double q) {
        x += v * dt;
        const double p00 = P00 + 2 * dt * P01 + dt * dt * P11 + q * dt * dt * dt / 3, p01 = P01 + dt * P11 + q * dt * dt / 2, p11 = P11 + q * dt;
        P00 = p00; P01 = p01; P11 = p11;
    }
    double innov(double z) const { return z - x; }
    double S(double R) const { return P00 + R; }
    void update(double z, double R) {
        const double s = P00 + R, k0 = P00 / s, k1 = P01 / s, y = z - x;
        x += k0 * y; v += k1 * y;
        const double p00 = (1 - k0) * P00, p01 = (1 - k0) * P01, p11 = P11 - k1 * P01;
        P00 = p00; P01 = p01; P11 = p11;
    }
};
}  // namespace

Drift kalmanDrift(const std::vector<Fix>& f, size_t i0, double windowS, double gps) {
    Drift d;
    if (f.size() < 3) return d;
    const Fix& L = f.back();
    windowS = std::max(10.0, windowS);
    size_t a = f.size() - 1;
    while (a > i0 && a > 0 && L.t - f[a - 1].t <= 3 * windowS) a--;
    if (f.size() - a < 3) return d;
    std::vector<double> gaps;
    for (size_t i = a + 1; i < f.size(); i++) gaps.push_back(f[i].t - f[i - 1].t);
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    const double dtm = std::max(0.5, gaps[gaps.size() / 2]);
    const double T = windowS / 3.0, R = gps * gps, Rz = 2.25 * R;
    const double q = R * dtm / (T * T * T * T), qz = Rz * dtm / (T * T * T * T);
    Kf2 E, N, Z;
    const Fix& f0 = f[a]; const Fix& f1 = f[a + 1];
    const double dt0 = std::max(0.5, f1.t - f0.t);
    E.init(eastM(f0.lon, L.lon, L.lat), (eastM(f1.lon, L.lon, L.lat) - eastM(f0.lon, L.lon, L.lat)) / dt0, R, 2 * R / (dt0 * dt0) + 25);
    N.init(northM(f0.lat, L.lat), (northM(f1.lat, L.lat) - northM(f0.lat, L.lat)) / dt0, R, 2 * R / (dt0 * dt0) + 25);
    Z.init(f0.alt, (f1.alt - f0.alt) / dt0, Rz, 2 * Rz / (dt0 * dt0) + 25);
    double tPrev = f0.t; int bad = 0;
    d.used = 1;
    for (size_t i = a + 1; i < f.size(); i++) {
        const double dt = f[i].t - tPrev;
        if (dt <= 0) continue;
        E.predict(dt, q); N.predict(dt, q); Z.predict(dt, qz);
        const double ze = eastM(f[i].lon, L.lon, L.lat), zn = northM(f[i].lat, L.lat);
        const double ye = E.innov(ze), yn = N.innov(zn);
        const double m2 = ye * ye / E.S(R) + yn * yn / N.S(R);
        if (m2 > 50 && std::hypot(ye, yn) > 30 && bad < 3) { bad++; d.rejected++; tPrev = f[i].t; continue; }   // a glitch
        bad = 0;
        E.update(ze, R); N.update(zn, R); Z.update(f[i].alt, Rz);
        tPrev = f[i].t; d.used++;
    }
    // to the time of the newest fix
    if (L.t > tPrev) { E.predict(L.t - tPrev, q); N.predict(L.t - tPrev, q); Z.predict(L.t - tPrev, qz); }
    d.ok = true; d.t = L.t;
    d.lat = L.lat; d.lon = L.lon; moveM(d.lat, d.lon, E.x, N.x);
    d.alt = Z.x; d.ve = E.v; d.vn = N.v; d.vz = Z.v;
    d.sigmaV = std::sqrt(std::max(0.0, 0.5 * (E.P11 + N.P11)));
    d.sigmaPos = std::sqrt(std::max(0.0, 0.5 * (E.P00 + N.P00)));
    return d;
}

}  // namespace sondepred

// ---- the tracker ------------------------------------------------------------------------------------------------------------------
using namespace sondepred;

void SondeLandTracker::addFix(double t, double lat, double lon, double alt, double tempC) {
    if (!(std::fabs(lat) <= 90) || !(std::fabs(lon) <= 180) || (lat == 0 && lon == 0) || !std::isfinite(alt) || alt < -1000 || alt > 60000) { rejected_++; return; }
    Fix x; x.t = t; x.lat = lat; x.lon = lon; x.alt = alt; x.tempC = tempC;
    if (!f_.empty()) {
        const Fix& p = f_.back();
        const double dt = t - p.t;
        if (dt <= 0) return;   // the same frame again, or older
        const double hs = distM(p.lat, p.lon, lat, lon) / dt, vs = std::fabs(alt - p.alt) / dt;
        if ((hs > 150 + 50 / dt || vs > 100 + 50 / dt) && pendingBad_ < 4) { pendingBad_++; rejected_++; return; }   // a GPS glitch
    }
    pendingBad_ = 0;
    f_.push_back(x);
    if (f_.size() > 40000) {   // keep the newest half at full rate, thin the older half
        std::vector<Fix> g;
        const size_t half = f_.size() / 2;
        for (size_t i = 0; i < half; i += 2) g.push_back(f_[i]);
        g.insert(g.end(), f_.begin() + half, f_.end());
        f_.swap(g);
    }
}

void SondeLandTracker::seed(const std::vector<SondeTrackPoint>& tr) {
    if (!f_.empty()) return;
    for (size_t i = 0; i < tr.size(); i++) addFix(tr[i].unixT ? (double)tr[i].unixT : (double)i, tr[i].lat, tr[i].lon, tr[i].altM, tr[i].tempC);
}

size_t SondeLandTracker::burstIndex() const {
    size_t top = 0;
    for (size_t i = 0; i < f_.size(); i++) if (f_[i].alt > f_[top].alt) top = i;
    return top;
}

bool SondeLandTracker::descending() const {
    if (f_.size() < 3) return false;
    const size_t top = burstIndex();
    if (f_[top].alt - f_.back().alt < 150) return false;
    size_t a = f_.size() - 1;
    while (a > 0 && f_.back().t - f_[a - 1].t <= 20) a--;
    return f_[a].alt - f_.back().alt > 10 || (a == f_.size() - 1 && f_[f_.size() - 2].alt > f_.back().alt);
}

LayerWinds SondeLandTracker::ascentWinds(double layerM) const {
    const size_t top = burstIndex();
    LayerWinds w = layerWinds(f_, 0, descending() ? top + 1 : f_.size(), layerM, 30);
    // light 1-2-1 smoothing where both neighbours have a fit (the balloon's swing leaves noise in thin layers)
    std::vector<WindLayer> s = w.layers;
    for (size_t i = 1; i + 1 < s.size(); i++)
        if (w.layers[i].ok && w.layers[i - 1].ok && w.layers[i + 1].ok) {
            s[i].ve = 0.25 * w.layers[i - 1].ve + 0.5 * w.layers[i].ve + 0.25 * w.layers[i + 1].ve;
            s[i].vn = 0.25 * w.layers[i - 1].vn + 0.5 * w.layers[i].vn + 0.25 * w.layers[i + 1].vn;
        }
    w.layers = s;
    return w;
}

LayerWinds SondeLandTracker::descentWinds(double layerM) const {
    if (!descending()) return LayerWinds();
    return layerWinds(f_, burstIndex(), f_.size(), layerM, 20);
}

Atmosphere SondeLandTracker::atmosphere(double layerM) const {
    Atmosphere a;
    const size_t end = descending() ? burstIndex() + 1 : f_.size();
    std::vector<double> sum, base; std::vector<int> cnt;
    int first = 0, last = -1;
    for (int pass = 0; pass < 2 && last < first; pass++) {   // the ascent's temperatures, else the descent's
        const size_t a = pass == 0 ? 0 : end, b = pass == 0 ? end : f_.size();
        double lo = 1e9, hi = -1e9;
        for (size_t i = a; i < b; i++) if (f_[i].tempC > -900) { lo = std::min(lo, f_[i].alt); hi = std::max(hi, f_[i].alt); }
        if (hi < lo) continue;
        first = (int)std::floor(lo / layerM); last = (int)std::floor(hi / layerM);
        sum.assign(last - first + 1, 0); cnt.assign(last - first + 1, 0);
        for (size_t i = a; i < b; i++) if (f_[i].tempC > -900) { const int L = (int)std::floor(f_[i].alt / layerM) - first; sum[L] += f_[i].tempC; cnt[L]++; }
    }
    std::vector<double> tb, tc;
    for (int L = first; L <= last; L++) if (cnt[L - first] > 0) { tb.push_back(L * layerM); tc.push_back(sum[L - first] / cnt[L - first]); }
    if (tb.empty()) a.buildIsa(); else a.build(tb, tc, layerM);
    return a;
}

namespace {
// What the integration needs at a height: the wind (prior plus the corrections) and its error.
struct WindModel {
    const LayerWinds* asc = nullptr;
    const ForecastWinds* fc = nullptr;
    const SondePredictOpts* o = nullptr;
    bool persist = false;            // no prior anywhere: the drift now, held
    double H = 0;                    // the sonde's height now
    double Lc = 600;
    double dAvgE = 0, dAvgN = 0, dNowE = 0, dNowN = 0;   // the change since the ascent: from the layers passed, and now
    double holdE = 0, holdN = 0, sigV = 1;
    double ascTop = 0, ascBot = 0;
    bool prior(double h, double t, double lat, double lon, double& e, double& n, double& sig) const {
        double sw = 0, se = 0, sn = 0;
        WindLayer w;
        if (asc && asc->at(h, w, 1000)) {
            const double age = std::fabs(t - w.tMid) / 3600.0, dkm = distM(lat, lon, w.latMid, w.lonMid) / 1000.0;
            const double s2 = w.sigma * w.sigma + std::pow(o->windChangeMsPerH * age, 2) + std::pow(o->windChangeMsPer100Km * dkm / 100.0, 2);
            sw += 1 / s2; se += w.ve / s2; sn += w.vn / s2;
        }
        double fe, fn;
        if (fc && fc->at(lat, lon, h, t, fe, fn)) { const double s2 = o->forecastSigmaMs * o->forecastSigmaMs; sw += 1 / s2; se += fe / s2; sn += fn / s2; }
        if (sw <= 0 && asc && asc->okCount() > 0 && asc->at(h, w, 1e9)) {   // beyond what the ascent measured and no forecast: its end, held
            const double gap = h > ascTop ? h - ascTop : (h < ascBot ? ascBot - h : 0);
            const double s = 2.0 + 2.0 * gap / 1000.0;
            sw = 1 / (s * s); se = w.ve * sw; sn = w.vn * sw;
        }
        if (sw <= 0) return false;
        e = se / sw; n = sn / sw; sig = std::sqrt(1 / sw);
        return true;
    }
    // the wind used at height h (below H); sig: its 1-sigma error
    void at(double h, double t, double lat, double lon, double& e, double& n, double& sig, double& c) const {
        const double d = std::max(0.0, H - h);
        c = std::exp(-d / Lc);
        if (persist) { e = holdE; n = holdN; sig = std::hypot(sigV, 4.0 * (1 - std::exp(-d / 3000.0))); return; }
        double pe = 0, pn = 0, ps = 3;
        if (!prior(h, t, lat, lon, pe, pn, ps)) { pe = holdE; pn = holdN; ps = 4; }
        const double g = std::exp(-d / 3000.0);
        e = pe + dAvgE * g + (dNowE - dAvgE) * c;
        n = pn + dAvgN * g + (dNowN - dAvgN) * c;
        sig = std::sqrt(sigV * sigV * c * c + ps * ps * (1 - c * c));
    }
};

struct Landing { double lat = 0, lon = 0, t = 0, ground = 0; bool terrain = false; };

// height grid of the members' wind (the nominal path's wind, from the bottom up)
struct Grid { double h0 = 0, dh = 25; std::vector<double> e, n, s, c; };
}  // namespace

SondePrediction SondeLandTracker::predict(const SondePredictOpts& o, const SondePredictEnv& env) const {
    SondePrediction P;
    if (f_.size() < 3) return P;
    const Fix& last = f_.back();
    const size_t top = burstIndex();
    const bool desc = descending();
    P.climbing = !desc;
    if (P.climbing && last.alt >= o.burstM - 50 && f_.back().alt <= f_[f_.size() - 2].alt) return P;   // at or past the assumed burst but not yet falling
    const Atmosphere atm = atmosphere();
    const double r0 = atm.rho0();
    const LayerWinds asc = o.useAscent ? ascentWinds(o.layerM) : LayerWinds();
    const LayerWinds dsc = desc ? descentWinds(o.layerM) : LayerWinds();
    const ForecastWinds* fc = (o.useForecast && env.forecast && !env.forecast->empty()) ? env.forecast : nullptr;
    auto groundAt = [&](double la, double lo, bool* fromT) {
        double h;
        if (o.useTerrain && env.ground && env.ground(la, lo, h) && std::isfinite(h)) { if (fromT) *fromT = true; return h; }
        if (fromT) *fromT = false;
        return o.groundM;
    };
    const double gNow = groundAt(last.lat, last.lon, nullptr);
    const double agl = last.alt - gNow;
    if (desc && agl < 5) return P;
    const bool approach = agl < o.approachBelowM;
    const double win = approach ? std::min(120.0, std::max(30.0, o.approachWindowS)) : 180.0;
    Drift dr = kalmanDrift(f_, desc ? top : 0, win, o.gpsSigmaM);
    if (desc && dr.ok && dr.t - f_[top].t < 20) dr.ok = false;   // the burst is still in the window
    // the start
    double lat0 = last.lat, lon0 = last.lon, h0 = last.alt;
    if (dr.ok) { lat0 = dr.lat; lon0 = dr.lon; h0 = dr.alt; }
    const double t0 = last.t;
    // descent rate
    DescentFit fit;
    if (desc && o.useFit) fit = fitDescent(f_, top, atm, approach ? std::max(60.0, std::min(o.descentTauS, 4 * o.approachWindowS)) : o.descentTauS);
    double K = o.defaultSeaLevelDescentMs, kSig = 0.15;
    if (fit.ok) { K = fit.K; kSig = std::max(0.02, fit.sigmaRel) + 0.02; P.fitted = true; P.noParachute = fit.noParachute; }
    else if (desc && dr.ok && -dr.vz > 0.5) { K = -dr.vz * std::sqrt(atm.density(h0) / r0); kSig = 0.08; }
    else if (desc) {
        size_t a = f_.size() - 1;
        while (a > top && last.t - f_[a - 1].t <= 30) a--;
        const double dt = last.t - f_[a].t;
        if (dt > 2 && f_[a].alt > last.alt) { K = (f_[a].alt - last.alt) / dt * std::sqrt(atm.density(last.alt) / r0); kSig = 0.1; }
    }
    auto kAt = [&](double h) { return fit.ok ? fit.kAt(h) : K; };
    P.seaLevelDescentMs = K;
    // the wind model
    WindModel M; M.asc = &asc; M.fc = fc; M.o = &o; M.H = h0;
    M.ascTop = asc.topM(); M.ascBot = asc.bottomM();
    M.Lc = std::max(100.0, o.driftCorrLenM);
    M.holdE = dr.ok ? dr.ve : 0; M.holdN = dr.ok ? dr.vn : 0; M.sigV = dr.ok ? std::max(0.2, dr.sigmaV) : 3.0;
    double pe, pn, ps;
    const bool havePrior = M.prior(h0, t0, lat0, lon0, pe, pn, ps);
    if (!havePrior && !dr.ok) {   // nothing at all: the last two fixes
        const Fix& p = f_[f_.size() - 2]; const double dt = std::max(0.5, last.t - p.t);
        M.holdE = eastM(last.lon, p.lon, p.lat) / dt; M.holdN = northM(last.lat, p.lat) / dt;
    }
    M.persist = !havePrior;
    if (desc && havePrior) {
        // the change since the ascent: descent layers already passed (within 3 km above) against the prior there
        double sw = 0, se = 0, sn = 0;
        for (size_t i = 0; i < dsc.layers.size(); i++) {
            const WindLayer& w = dsc.layers[i];
            if (!w.ok) continue;
            const double hc = (dsc.firstLayer + (double)i + 0.5) * dsc.layerM;
            if (hc < h0 || hc > h0 + 3000) continue;
            double qe, qn, qs;
            if (!M.prior(hc, w.tMid, w.latMid, w.lonMid, qe, qn, qs)) continue;
            const double wt = std::exp(-(hc - h0) / 1500.0) / (w.sigma * w.sigma + 1.0);
            sw += wt; se += wt * (w.ve - qe); sn += wt * (w.vn - qn);
        }
        if (sw > 0) { M.dAvgE = se / sw; M.dAvgN = sn / sw; }
        const double m = std::hypot(M.dAvgE, M.dAvgN);
        if (m > 15) { M.dAvgE *= 15 / m; M.dAvgN *= 15 / m; }
        M.dNowE = M.dAvgE; M.dNowN = M.dAvgN;
        if (dr.ok && o.useKalman) { M.dNowE = dr.ve - pe; M.dNowN = dr.vn - pn; P.usedDrift = true; }
    } else if (!desc && dr.ok && havePrior && o.useKalman) {   // climbing: the drift now against the prior at this height
        M.dNowE = dr.ve - pe; M.dNowN = dr.vn - pn; P.usedDrift = true;
    }
    if (M.persist && dr.ok) P.usedDrift = true;
    P.driftVe = dr.ok ? dr.ve : M.holdE; P.driftVn = dr.ok ? dr.vn : M.holdN;
    P.ascentLayers = asc.okCount(); P.usedForecast = fc != nullptr;
    // climbing: the rest of the way up first
    const double climbMs = (!desc && dr.ok && dr.vz > 0.5) ? dr.vz : 5.0;
    const double burstNom = P.climbing ? std::max(o.burstM, h0 + 100) : h0;

    // ---- the integration
    Grid G;
    struct Run { double kMul = 1, gOff = 0, burst = 0, de = 0, dn = 0; const std::vector<double>* we = nullptr; const std::vector<double>* wn = nullptr; };
    auto fly = [&](const Run& r, bool nominal, std::vector<std::pair<double, double>>* path) {
        Landing L;
        double la = lat0, lo = lon0, h = h0, t = t0;
        moveM(la, lo, r.de, r.dn);
        auto wind = [&](double hh, double tt, double& e, double& n) {
            if (nominal) { double s, c; M.at(hh, tt, la, lo, e, n, s, c); return; }
            const double x = std::max(0.0, std::min((double)G.e.size() - 1.001, (hh - G.h0) / G.dh));
            const int i = (int)x; const double u = x - i;
            e = G.e[i] + u * (G.e[i + 1] - G.e[i]); n = G.n[i] + u * (G.n[i + 1] - G.n[i]);
            if (r.we) {
                const double y = std::max(0.0, std::min((double)r.we->size() - 1.001, (hh - G.h0) / 250.0));
                const int j = (int)y; const double w = y - j;
                const double sc = G.s[i] + u * (G.s[i + 1] - G.s[i]);
                e += sc * ((*r.we)[j] + w * ((*r.we)[j + 1] - (*r.we)[j]));
                n += sc * ((*r.wn)[j] + w * ((*r.wn)[j + 1] - (*r.wn)[j]));
            }
        };
        double lastPath = -1e9;
        if (P.climbing) {
            const double top2 = r.burst;
            while (h < top2) {
                const double dt = 10, hm = h + climbMs * dt / 2;
                double e, n; wind(std::min(hm, top2), t + dt / 2, e, n);
                moveM(la, lo, e * dt, n * dt); h += climbMs * dt; t += dt;
                if (path && t - lastPath >= 20) { path->push_back({la, lo}); lastPath = t; }
            }
            h = top2;
        }
        bool fromT = false;
        double g = groundAt(la, lo, &fromT) + r.gOff;
        for (int step = 0; step < 40000; step++) {
            const double a = h - g;
            const double dt = a > 1500 ? 5 : (a > 200 ? 2 : 1);
            auto vAt = [&](double hh) { return std::max(0.5, kAt(hh) * r.kMul * std::sqrt(r0 / atm.density(hh))); };
            const double hm = h - vAt(h) * dt / 2;
            double e, n; wind(hm, t + dt / 2, e, n);
            const double v = vAt(hm);
            double la2 = la, lo2 = lo; moveM(la2, lo2, e * dt, n * dt);
            const double h2 = h - v * dt;
            bool ft = false;
            const double g2 = groundAt(la2, lo2, &ft) + r.gOff;
            if (h2 <= g2) {
                const double u = std::min(1.0, std::max(0.0, (h - g) / std::max(1e-6, (h - g) - (h2 - g2))));
                L.lat = la + u * (la2 - la); L.lon = lo + u * (lo2 - lo); L.t = t + u * dt; L.ground = g + u * (g2 - g); L.terrain = ft;
                if (path) path->push_back({L.lat, L.lon});
                return L;
            }
            la = la2; lo = lo2; h = h2; t += dt; g = g2; fromT = ft;
            if (nominal) {   // the members' wind grid
                double we, wn, s, c; M.at(hm, t - dt / 2, la, lo, we, wn, s, c);
                G.e.push_back(we); G.n.push_back(wn); G.s.push_back(s); G.c.push_back(hm);
            }
            if (path && t - lastPath >= 20) { path->push_back({la, lo}); lastPath = t; }
        }
        L.lat = la; L.lon = lo; L.t = t; L.ground = g; L.terrain = fromT;
        return L;
    };
    Run nom; nom.burst = burstNom;
    const Landing Ln = fly(nom, true, &P.path);
    P.valid = true; P.lat = Ln.lat; P.lon = Ln.lon; P.secondsLeft = Ln.t - t0; P.landUnix = Ln.t; P.groundM = Ln.ground; P.groundFromTerrain = Ln.terrain;

    // ---- the ensemble
    if (o.members > 0 && G.c.size() >= 2) {
        // resample the nominal run's (height, wind) pairs onto a regular grid from the ground up
        std::vector<double> hs = G.c, es = G.e, ns = G.n, ss = G.s;
        std::vector<size_t> ord(hs.size());
        for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return hs[a] < hs[b]; });
        const double hLo = hs[ord.front()] - 200, hHi = std::max(hs[ord.back()], burstNom) + 200;
        Grid R; R.dh = 25; R.h0 = hLo;
        const int n = std::max(2, (int)((hHi - hLo) / R.dh) + 2);
        size_t k = 0;
        for (int i = 0; i < n; i++) {
            const double h = hLo + i * R.dh;
            while (k + 1 < ord.size() && hs[ord[k + 1]] < h) k++;
            size_t a = ord[k], b = ord[std::min(k + 1, ord.size() - 1)];
            double u = (hs[b] > hs[a]) ? (h - hs[a]) / (hs[b] - hs[a]) : 0;
            u = std::min(1.0, std::max(0.0, u));
            R.e.push_back(es[a] + u * (es[b] - es[a])); R.n.push_back(ns[a] + u * (ns[b] - ns[a])); R.s.push_back(ss[a] + u * (ss[b] - ss[a]));
        }
        G = R;
        std::mt19937 rng(o.seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        const int nw = (int)((hHi - hLo) / 250.0) + 2;
        const double rho = std::exp(-250.0 / 1500.0), rq = std::sqrt(1 - rho * rho);
        std::vector<double> we(nw), wn(nw);
        std::vector<double> dE, dN, dT;
        const double gSig = (o.useTerrain && env.ground) ? 8.0 : 20.0;
        for (int m = 0; m < o.members; m++) {
            Run r; r.kMul = std::exp(kSig * nd(rng)); r.gOff = gSig * nd(rng);
            r.burst = P.climbing ? std::max(h0 + 100, burstNom * (1 + 0.08 * nd(rng))) : h0;
            r.de = dr.ok ? dr.sigmaPos * nd(rng) : 0; r.dn = dr.ok ? dr.sigmaPos * nd(rng) : 0;
            // unit wind error, correlated over 1.5 km, from the top down; at the sonde's height it is the drift's own error
            double xe = nd(rng), xn = nd(rng);
            for (int i = nw - 1; i >= 0; i--) { we[i] = xe; wn[i] = xn; xe = rho * xe + rq * nd(rng); xn = rho * xn + rq * nd(rng); }
            r.we = &we; r.wn = &wn;
            const Landing L = fly(r, false, nullptr);
            dE.push_back(eastM(L.lon, P.lon, P.lat)); dN.push_back(northM(L.lat, P.lat)); dT.push_back(L.t - Ln.t);
            P.members.push_back({L.lat, L.lon});
        }
        const double N = (double)dE.size();
        double me = 0, mn = 0, mt = 0;
        for (size_t i = 0; i < dE.size(); i++) { me += dE[i]; mn += dN[i]; mt += dT[i]; }
        me /= N; mn /= N; mt /= N;
        double cee = 0, cnn = 0, cen = 0, ctt = 0;
        std::vector<double> r;
        for (size_t i = 0; i < dE.size(); i++) {
            cee += (dE[i] - me) * (dE[i] - me); cnn += (dN[i] - mn) * (dN[i] - mn); cen += (dE[i] - me) * (dN[i] - mn); ctt += (dT[i] - mt) * (dT[i] - mt);
            r.push_back(std::hypot(dE[i], dN[i]));
        }
        cee /= N; cnn /= N; cen /= N;
        const double tr = cee + cnn, det = cee * cnn - cen * cen, disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
        P.ellMajorM = std::sqrt(std::max(0.0, tr / 2 + disc)); P.ellMinorM = std::sqrt(std::max(0.0, tr / 2 - disc));
        double ang = 0.5 * std::atan2(2 * cen, cnn - cee) / kD;   // from north towards east
        if (ang < 0) ang += 180;
        P.ellAngleDeg = ang;
        std::sort(r.begin(), r.end());
        P.r50M = r[(size_t)(0.50 * (r.size() - 1))]; P.r95M = r[(size_t)(0.95 * (r.size() - 1))];
        P.secondsSigma = std::sqrt(ctt / N);
        P.hasSpread = true;
    }
    char b[256];
    const int s = (int)std::max(0.0, P.secondsLeft);
    std::snprintf(b, sizeof b, "%s%d:%02d%s; descent %.1f m/s at sea level (%s)%s; wind: %s%s%s", P.climbing ? "climbing; lands in about " : "lands in ",
                  s / 60, s % 60, P.hasSpread ? "" : "", K, P.fitted ? "fitted" : "assumed", P.noParachute ? ", no parachute" : "",
                  asc.okCount() ? "ascent" : (fc ? "" : "drift now"), fc ? (asc.okCount() ? " + forecast" : "forecast") : "",
                  P.usedDrift ? " + drift now" : "");
    P.text = b;
    if (P.hasSpread) {
        std::snprintf(b, sizeof b, "; 50 %% within %.0f m, 95 %% within %.0f m", P.r50M, P.r95M);
        P.text += b;
    }
    return P;
}

SondePrediction predictSondeLanding40(const std::vector<SondeTrackPoint>& track, const SondePredictOpts& o, const SondePredictEnv& env) {
    SondeLandTracker t;
    t.seed(track);
    return t.predict(o, env);
}

}  // namespace dect2
