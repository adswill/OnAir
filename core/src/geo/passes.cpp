// SGP4 (near-earth), look angles, pass search and Doppler: see passes.h
#include "dect2/geo/passes.h"
#include "dect2/files/files_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include "dect2/common/str.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
constexpr double kPi = dect2::math::kPi, k2Pi = 2 * kPi, kDeg = kPi / 180;
// WGS-72, as SGP4 needs it
constexpr double kMu = 398600.8, kRe = 6378.135, kJ2 = 0.001082616, kJ3 = -0.00000253881, kJ4 = -0.00000165597, kJ3oJ2 = kJ3 / kJ2;
const double kXke = 60.0 / std::sqrt(kRe * kRe * kRe / kMu);
constexpr double kC = 299792.458, kOmegaE = 7.292115e-5;

}

// the parsing is core/src/files/files_proto.cpp (C++) or the Rust crate onair_files
bool parseTle(const std::string& a, const std::string& b, Tle& t, const std::string& name) {
    const filesp::TleData d = filesp::tleParse(a, b, name);
    if (d.status == 1) return false;   // not a TLE: t is left as it was
    t.name = d.name; t.norad = d.norad; t.epochJd = d.epochJd;
    t.bstar = d.bstar; t.inclDeg = d.inclDeg; t.raanDeg = d.raanDeg; t.ecc = d.ecc; t.argpDeg = d.argpDeg; t.meanAnomDeg = d.meanAnomDeg; t.meanMotion = d.meanMotion;
    return d.status == 0;
}

std::vector<Tle> parseTleText(const std::string& text) {
    std::vector<Tle> out;
    for (const filesp::TleData& d : filesp::tleScan(text)) {
        Tle t;
        t.name = d.name; t.norad = d.norad; t.epochJd = d.epochJd;
        t.bstar = d.bstar; t.inclDeg = d.inclDeg; t.raanDeg = d.raanDeg; t.ecc = d.ecc; t.argpDeg = d.argpDeg; t.meanAnomDeg = d.meanAnomDeg; t.meanMotion = d.meanMotion;
        out.push_back(t);
    }
    return out;
}

bool Sgp4::init(const Tle& t) {
    tle_ = t; ok_ = false;
    const double x2o3 = 2.0 / 3.0;
    ecco_ = t.ecc; inclo_ = t.inclDeg * kDeg; nodeo_ = t.raanDeg * kDeg; argpo_ = t.argpDeg * kDeg; mo_ = t.meanAnomDeg * kDeg;
    bstar_ = t.bstar;
    double no = t.meanMotion * k2Pi / 1440.0;   // rad / min (Kozai)
    if (no <= 0 || ecco_ < 0 || ecco_ >= 1) return false;
    // initl: the Brouwer mean motion
    const double eccsq = ecco_ * ecco_, omeosq = 1 - eccsq, rteosq = std::sqrt(omeosq), cosio = std::cos(inclo_), cosio2 = cosio * cosio;
    const double ak = std::pow(kXke / no, x2o3), d1 = 0.75 * kJ2 * (3 * cosio2 - 1) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    const double adel = ak * (1 - del * del - del * (1.0 / 3.0 + 134 * del * del / 81.0));
    del = d1 / (adel * adel);
    no = no / (1 + del);
    no_ = no;
    if (k2Pi / no >= 225.0) return false;   // deep space: not handled
    const double ao = std::pow(kXke / no, x2o3), sinio = std::sin(inclo_), po = ao * omeosq, con42 = 1 - 5 * cosio2;
    con41_ = -con42 - cosio2 - cosio2;
    const double posq = po * po, rp = ao * (1 - ecco_);
    isimp_ = rp < 220.0 / kRe + 1;
    double sfour = 78.0 / kRe + 1, qzms24 = std::pow((120.0 - 78.0) / kRe, 4);
    const double perige = (rp - 1) * kRe;
    if (perige < 156) {
        sfour = perige < 98 ? 20 : perige - 78;
        qzms24 = std::pow((120 - sfour) / kRe, 4);
        sfour = sfour / kRe + 1;
    }
    const double pinvsq = 1 / posq, tsi = 1 / (ao - sfour);
    eta_ = ao * ecco_ * tsi;
    const double etasq = eta_ * eta_, eeta = ecco_ * eta_, psisq = std::fabs(1 - etasq), coef = qzms24 * std::pow(tsi, 4), coef1 = coef / std::pow(psisq, 3.5);
    const double cc2 = coef1 * no * (ao * (1 + 1.5 * etasq + eeta * (4 + etasq)) + 0.375 * kJ2 * tsi / psisq * con41_ * (8 + 3 * etasq * (8 + etasq)));
    cc1_ = bstar_ * cc2;
    const double cc3 = ecco_ > 1e-4 ? -2 * coef * tsi * kJ3oJ2 * no * sinio / ecco_ : 0;
    x1mth2_ = 1 - cosio2;
    cc4_ = 2 * no * coef1 * ao * omeosq *
           (eta_ * (2 + 0.5 * etasq) + ecco_ * (0.5 + 2 * etasq) -
            kJ2 * tsi / (ao * psisq) * (-3 * con41_ * (1 - 2 * eeta + etasq * (1.5 - 0.5 * eeta)) + 0.75 * x1mth2_ * (2 * etasq - eeta * (1 + etasq)) * std::cos(2 * argpo_)));
    cc5_ = 2 * coef1 * ao * omeosq * (1 + 2.75 * (etasq + eeta) + eeta * etasq);
    const double cosio4 = cosio2 * cosio2, temp1 = 1.5 * kJ2 * pinvsq * no, temp2 = 0.5 * temp1 * kJ2 * pinvsq, temp3 = -0.46875 * kJ4 * pinvsq * pinvsq * no;
    mdot_ = no + 0.5 * temp1 * rteosq * con41_ + 0.0625 * temp2 * rteosq * (13 - 78 * cosio2 + 137 * cosio4);
    argpdot_ = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7 - 114 * cosio2 + 395 * cosio4) + temp3 * (3 - 36 * cosio2 + 49 * cosio4);
    const double xhdot1 = -temp1 * cosio;
    nodedot_ = xhdot1 + (0.5 * temp2 * (4 - 19 * cosio2) + 2 * temp3 * (3 - 7 * cosio2)) * cosio;
    omgcof_ = bstar_ * cc3 * std::cos(argpo_);
    xmcof_ = ecco_ > 1e-4 ? -x2o3 * coef * bstar_ / eeta : 0;
    nodecf_ = 3.5 * omeosq * xhdot1 * cc1_;
    t2cof_ = 1.5 * cc1_;
    xlcof_ = std::fabs(cosio + 1) > 1.5e-12 ? -0.25 * kJ3oJ2 * sinio * (3 + 5 * cosio) / (1 + cosio) : -0.25 * kJ3oJ2 * sinio * (3 + 5 * cosio) / 1.5e-12;
    aycof_ = -0.5 * kJ3oJ2 * sinio;
    delmo_ = std::pow(1 + eta_ * std::cos(mo_), 3);
    sinmao_ = std::sin(mo_);
    x7thm1_ = 7 * cosio2 - 1;
    if (!isimp_) {
        const double cc1sq = cc1_ * cc1_;
        d2_ = 4 * ao * tsi * cc1sq;
        const double temp = d2_ * tsi * cc1_ / 3;
        d3_ = (17 * ao + sfour) * temp;
        d4_ = 0.5 * temp * ao * tsi * (221 * ao + 31 * sfour) * cc1_;
        t3cof_ = d2_ + 2 * cc1sq;
        t4cof_ = 0.25 * (3 * d3_ + cc1_ * (12 * d2_ + 10 * cc1sq));
        t5cof_ = 0.2 * (3 * d4_ + 12 * cc1_ * d3_ + 6 * d2_ * d2_ + 15 * cc1sq * (2 * d2_ + cc1sq));
    }
    double r[3], v[3];
    ok_ = true;
    ok_ = propagate(0, r, v);
    return ok_;
}

bool Sgp4::propagate(double t, double r[3], double v[3]) const {
    if (!ok_) return false;
    const double x2o3 = 2.0 / 3.0, vkmpersec = kRe * kXke / 60.0;
    const double xmdf = mo_ + mdot_ * t, argpdf = argpo_ + argpdot_ * t, nodedf = nodeo_ + nodedot_ * t;
    double argpm = argpdf, mm = xmdf;
    const double t2 = t * t;
    double nodem = nodedf + nodecf_ * t2, tempa = 1 - cc1_ * t, tempe = bstar_ * cc4_ * t, templ = t2cof_ * t2;
    if (!isimp_) {
        const double delomg = omgcof_ * t, delmtemp = 1 + eta_ * std::cos(xmdf), delm = xmcof_ * (delmtemp * delmtemp * delmtemp - delmo_), temp = delomg + delm;
        mm = xmdf + temp; argpm = argpdf - temp;
        const double t3 = t2 * t, t4 = t3 * t;
        tempa = tempa - d2_ * t2 - d3_ * t3 - d4_ * t4;
        tempe = tempe + bstar_ * cc5_ * (std::sin(mm) - sinmao_);
        templ = templ + t3cof_ * t3 + t4 * (t4cof_ + t * t5cof_);
    }
    double nm = no_, em = ecco_;
    const double inclm = inclo_;
    const double am = std::pow(kXke / nm, x2o3) * tempa * tempa;
    nm = kXke / std::pow(am, 1.5);
    em = em - tempe;
    if (em >= 1 || em < -0.001 || am < 0.95) return false;
    if (em < 1e-6) em = 1e-6;
    mm = mm + no_ * templ;
    double xlm = mm + argpm + nodem;
    nodem = std::fmod(nodem, k2Pi); argpm = std::fmod(argpm, k2Pi); xlm = std::fmod(xlm, k2Pi);
    mm = std::fmod(xlm - argpm - nodem, k2Pi);
    const double sinip = std::sin(inclm), cosip = std::cos(inclm), ep = em, argpp = argpm, nodep = nodem, mp = mm;
    // long period periodics
    const double axnl = ep * std::cos(argpp);
    double temp = 1 / (am * (1 - ep * ep));
    const double aynl = ep * std::sin(argpp) + temp * aycof_, xl = mp + argpp + nodep + temp * xlcof_ * axnl;
    // Kepler's equation
    const double u = std::fmod(xl - nodep, k2Pi);
    double eo1 = u, tem5 = 9999.9, sineo1 = 0, coseo1 = 0;
    for (int ktr = 1; std::fabs(tem5) >= 1e-12 && ktr <= 10; ktr++) {
        sineo1 = std::sin(eo1); coseo1 = std::cos(eo1);
        tem5 = 1 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (std::fabs(tem5) >= 0.95) tem5 = tem5 > 0 ? 0.95 : -0.95;
        eo1 += tem5;
    }
    // short period periodics
    const double ecose = axnl * coseo1 + aynl * sineo1, esine = axnl * sineo1 - aynl * coseo1, el2 = axnl * axnl + aynl * aynl, pl = am * (1 - el2);
    if (pl < 0) return false;
    const double rl = am * (1 - ecose), rdotl = std::sqrt(am) * esine / rl, rvdotl = std::sqrt(pl) / rl, betal = std::sqrt(1 - el2);
    temp = esine / (1 + betal);
    const double sinu = am / rl * (sineo1 - aynl - axnl * temp), cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = std::atan2(sinu, cosu);
    const double sin2u = (cosu + cosu) * sinu, cos2u = 1 - 2 * sinu * sinu;
    temp = 1 / pl;
    const double temp1 = 0.5 * kJ2 * temp, temp2 = temp1 * temp;
    const double mrt = rl * (1 - 1.5 * temp2 * betal * con41_) + 0.5 * temp1 * x1mth2_ * cos2u;
    su = su - 0.25 * temp2 * x7thm1_ * sin2u;
    const double xnode = nodep + 1.5 * temp2 * cosip * sin2u, xinc = inclm + 1.5 * temp2 * cosip * sinip * cos2u;
    const double mvt = rdotl - nm * temp1 * x1mth2_ * sin2u / kXke, rvdot = rvdotl + nm * temp1 * (x1mth2_ * cos2u + 1.5 * con41_) / kXke;
    const double sinsu = std::sin(su), cossu = std::cos(su), snod = std::sin(xnode), cnod = std::cos(xnode), sini = std::sin(xinc), cosi = std::cos(xinc);
    const double xmx = -snod * cosi, xmy = cnod * cosi;
    const double ux = xmx * sinsu + cnod * cossu, uy = xmy * sinsu + snod * cossu, uz = sini * sinsu;
    const double vx = xmx * cossu - cnod * sinsu, vy = xmy * cossu - snod * sinsu, vz = sini * cossu;
    r[0] = mrt * ux * kRe; r[1] = mrt * uy * kRe; r[2] = mrt * uz * kRe;
    v[0] = (mvt * ux + rvdot * vx) * vkmpersec; v[1] = (mvt * uy + rvdot * vy) * vkmpersec; v[2] = (mvt * uz + rvdot * vz) * vkmpersec;
    return mrt >= 1;   // below the surface: decayed
}

double unixToJd(double s) { return s / 86400.0 + 2440587.5; }
double jdToUnix(double jd) { return (jd - 2440587.5) * 86400.0; }

double gmst(double jd) {
    const double tut1 = (jd - 2451545.0) / 36525.0;
    double temp = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 + (876600.0 * 3600 + 8640184.812866) * tut1 + 67310.54841;   // seconds
    temp = std::fmod(temp * kDeg / 240.0, k2Pi);
    return temp < 0 ? temp + k2Pi : temp;
}

bool lookAt(const Sgp4& s, const Observer& o, double unixSec, Look& out) {
    const double jd = unixToJd(unixSec);
    double r[3], v[3];
    if (!s.propagate((jd - s.tle().epochJd) * 1440.0, r, v)) return false;
    const double th = gmst(jd), c = std::cos(th), sn = std::sin(th);
    // TEME -> earth fixed (no polar motion), and the velocity seen from the rotating earth
    const double re[3] = {c * r[0] + sn * r[1], -sn * r[0] + c * r[1], r[2]};
    double ve[3] = {c * v[0] + sn * v[1], -sn * v[0] + c * v[1], v[2]};
    ve[0] += kOmegaE * re[1]; ve[1] -= kOmegaE * re[0];
    // the observer on the WGS-84 ellipsoid
    const double a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f);
    const double la = o.latDeg * kDeg, lo = o.lonDeg * kDeg, sla = std::sin(la), cla = std::cos(la), slo = std::sin(lo), clo = std::cos(lo);
    const double N = a / std::sqrt(1 - e2 * sla * sla);
    const double ob[3] = {(N + o.altKm) * cla * clo, (N + o.altKm) * cla * slo, (N * (1 - e2) + o.altKm) * sla};
    const double d[3] = {re[0] - ob[0], re[1] - ob[1], re[2] - ob[2]};
    const double east = -slo * d[0] + clo * d[1];
    const double north = -sla * clo * d[0] - sla * slo * d[1] + cla * d[2];
    const double up = cla * clo * d[0] + cla * slo * d[1] + sla * d[2];
    const double rng = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    out.rangeKm = rng;
    out.elDeg = std::asin(std::max(-1.0, std::min(1.0, up / rng))) / kDeg;
    double az = std::atan2(east, north) / kDeg;
    out.azDeg = az < 0 ? az + 360 : az;
    out.rangeRateKmS = (d[0] * ve[0] + d[1] * ve[1] + d[2] * ve[2]) / rng;
    return true;
}

bool geoLookAngles(const Observer& o, double satLonEastDeg, Look& out) {
    constexpr double R = 6378.137 + 35786.0;   // km from Earth centre (WGS-84 equatorial + nominal GEO height)
    const double lo = satLonEastDeg * kDeg;
    const double re[3] = {R * std::cos(lo), R * std::sin(lo), 0.0};
    const double a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f);
    const double la = o.latDeg * kDeg, obsLo = o.lonDeg * kDeg, sla = std::sin(la), cla = std::cos(la), slo = std::sin(obsLo), clo = std::cos(obsLo);
    const double N = a / std::sqrt(1 - e2 * sla * sla);
    const double ob[3] = {(N + o.altKm) * cla * clo, (N + o.altKm) * cla * slo, (N * (1 - e2) + o.altKm) * sla};
    const double d[3] = {re[0] - ob[0], re[1] - ob[1], re[2] - ob[2]};
    const double east = -slo * d[0] + clo * d[1];
    const double north = -sla * clo * d[0] - sla * slo * d[1] + cla * d[2];
    const double up = cla * clo * d[0] + cla * slo * d[1] + sla * d[2];
    const double rng = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (rng < 1e-3) return false;
    out.rangeKm = rng;
    out.elDeg = std::asin(std::max(-1.0, std::min(1.0, up / rng))) / kDeg;
    double az = std::atan2(east, north) / kDeg;
    out.azDeg = az < 0 ? az + 360 : az;
    out.rangeRateKmS = 0;
    return true;
}

double dopplerHz(double freqHz, double rangeRateKmS) { return -freqHz * rangeRateKmS / kC; }

bool subPoint(const Sgp4& s, double unixSec, double& latDeg, double& lonDeg, double& altKm) {
    const double jd = unixToJd(unixSec);
    double r[3], v[3];
    if (!s.propagate((jd - s.tle().epochJd) * 1440.0, r, v)) return false;
    const double th = gmst(jd), x = std::cos(th) * r[0] + std::sin(th) * r[1], y = -std::sin(th) * r[0] + std::cos(th) * r[1], z = r[2];
    const double a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f), p = std::sqrt(x * x + y * y);
    double la = std::atan2(z, p * (1 - e2)), N = a;
    for (int k = 0; k < 5; k++) { N = a / std::sqrt(1 - e2 * std::sin(la) * std::sin(la)); la = std::atan2(z + e2 * N * std::sin(la), p); }
    latDeg = la / kDeg; lonDeg = std::atan2(y, x) / kDeg;
    altKm = p / std::cos(la) - N;
    return true;
}

std::vector<Pass> findPasses(const Sgp4& s, const Observer& o, double t0, double t1, double minEl) {
    std::vector<Pass> out;
    if (!s.ok() || t1 <= t0) return out;
    auto el = [&](double t, bool& ok) { Look l; ok = lookAt(s, o, t, l); return ok ? l.elDeg - minEl : -999.0; };
    auto edge = [&](double lo, double hi) {   // lo below, hi above (or the other way): the crossing to ~0.5 s
        bool ok;
        const bool riseLo = el(lo, ok) < 0;
        while (hi - lo > 0.5) { const double m = 0.5 * (lo + hi); if ((el(m, ok) < 0) == riseLo) lo = m; else hi = m; }
        return 0.5 * (lo + hi);
    };
    const double fine = 20.0;   // a pass above the horizon lasts minutes: none is missed with this step except a graze of seconds
    bool ok;
    double prevT = t0, prevE = el(t0, ok);
    if (!ok) return out;
    Pass cur;
    bool in = prevE >= 0;
    if (in) cur.aos = t0;
    double bestT = t0, bestE = prevE;
    double step = fine;
    for (double t = t0 + step;; t += step) {
        if (t > t1) t = t1;
        const double e = el(t, ok);
        if (!ok) break;
        step = !in && e + minEl < -25.0 ? 120.0 : fine;   // far below the horizon (a low orbit needs minutes to get up): save the work
        if (!in && e >= 0) { in = true; cur = Pass(); cur.aos = edge(prevT, t); bestT = t; bestE = e; }
        if (in && e > bestE) { bestE = e; bestT = t; }
        if (in && (e < 0 || t >= t1)) {
            cur.los = e < 0 ? edge(prevT, t) : t;
            // the highest point: golden-section search around the best sample
            double a = std::max(cur.aos, bestT - fine), b = std::min(cur.los, bestT + fine);
            const double g = 0.381966011250105;
            for (int k = 0; k < 40 && b - a > 0.2; k++) {
                const double m1 = a + g * (b - a), m2 = b - g * (b - a);
                if (el(m1, ok) < el(m2, ok)) a = m1; else b = m2;
            }
            cur.tca = 0.5 * (a + b);
            Look l;
            lookAt(s, o, cur.tca, l); cur.maxElDeg = l.elDeg;
            lookAt(s, o, cur.aos, l); cur.aosAzDeg = l.azDeg;
            lookAt(s, o, cur.los, l); cur.losAzDeg = l.azDeg;
            out.push_back(cur);
            in = false;
        }
        prevT = t; prevE = e;
        if (t >= t1) break;
    }
    return out;
}


bool tleUpdateDue(double nowSec, double lastOkSec, double lastTrySec, double minIntervalSec, double retrySec) {
    if (lastOkSec > 0) {
        const double age = nowSec - lastOkSec;
        if (age >= 0 && age < minIntervalSec) return false;
    }
    if (lastTrySec > 0) {
        const double since = nowSec - lastTrySec;
        if (since >= 0 && since < retrySec) return false;
    }
    return true;
}

}
