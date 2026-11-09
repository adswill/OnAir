// See xband_products.h. AVHRR calibration: NOAA KLM User's Guide method (radiance from the black body and space views, non-linearity correction, band coefficients), coefficients from SatDump's
// resources/calibration/AVHRR.json (GPL-3.0, Copyright Aang23 and contributors). Multi-channel recipes follow the EUMETSAT RGB recommendations for the channel pairs; code written for OnAir.
#include "dect2/xband/xband_products.h"
#include <algorithm>
#include <cmath>

namespace dect2::xband {

namespace {
constexpr double kC1 = 1.1910427e-5, kC2 = 1.4387752;

struct ChanSpec { double vc, A, B, Ns, b0, b1, b2; };
// channels 3b, 4, 5 of MetOp-B and MetOp-C
const ChanSpec kMetop[2][3] = {
    {{2664.32, 1.763611, 0.997018, 0, 0, 0, 0}, {933.63, 0.504183, 0.998638, -4.75, 4.85, -0.096771, 0.00048091}, {839.62, 0.381279, 0.998861, -4.39, 4.36, -0.076635, 0.00033524}},
    {{2671.94, 1.711878, 0.997197, 0, 0, 0, 0}, {930.31, 0.5899, 0.998403, -6.27, 6.58, -0.13203, 0.00065922}, {828.6, 0.378105, 0.998854, -2.55, 3.23, -0.05692, 0.00024963}},
};
const double kPrt[2][4][3] = {
    {{276.5853, 0.050933, 1.54333e-06}, {276.5335, 0.051033, 1.49751e-06}, {276.5721, 0.051097, 1.42928e-06}, {276.575, 0.05102, 1.50841e-06}},
    {{276.5862, 0.051051, 1.474208e-06}, {276.6136, 0.051029, 1.472138e-06}, {276.5975, 0.051065, 1.469268e-06}, {276.4595, 0.05099, 1.506223e-06}},
};

double planck(double t, double vc) { return kC1 * vc * vc * vc / (std::exp(kC2 * vc / t) - 1); }
double invPlanck(double n, double vc) { return kC2 * vc / std::log(1 + kC1 * vc * vc * vc / n); }

int findBand(const Store& s, const char* nameKey) {
    for (int i = 0; i < s.count(); i++) if (s.plane(i).name == nameKey) return i;
    return -1;
}

// A channel as 0..1, stretched on the 1 and 99 percentiles of the pixels that hold data
struct Band {
    const Plane* p = nullptr;
    double lo = 0, hi = 1;
    bool ok = false;
};
Band band(const Store& s, const char* name) {
    Band b;
    const int i = findBand(s, name);
    if (i < 0) return b;
    b.p = &s.plane(i);
    if (b.p->lines == 0) return b;
    std::vector<uint16_t> v;
    for (size_t k = 0; k < b.p->px.size(); k += 13) if (b.p->px[k]) v.push_back(b.p->px[k]);
    if (v.size() < 16) return b;
    std::sort(v.begin(), v.end());
    b.lo = v[v.size() / 100]; b.hi = v[v.size() * 99 / 100];
    if (b.hi <= b.lo) b.hi = b.lo + 1;
    b.ok = true;
    return b;
}
inline double sampleBand(const Band& b, int x, int y) {
    if (x < 0 || y < 0 || x >= b.p->width || y >= b.p->lines) return 0;
    const uint16_t v = b.p->px[(size_t)y * b.p->width + x];
    return std::clamp((v - b.lo) / (b.hi - b.lo), 0.0, 1.0);
}
inline uint32_t rgb(double r, double g, double b) {
    const auto c = [](double v) { return (uint32_t)std::clamp((int)std::lround(v * 255), 0, 255); };
    return 0xFF000000u | c(b) << 16 | c(g) << 8 | c(r);
}
// Natural colour by nearest neighbour when the channels have different widths
XbProduct composite(const std::string& name, const Band& r, const Band& g, const Band& b, int maxWidth, double gamma = 0.8) {
    XbProduct out; out.name = name;
    if (!r.ok || !g.ok || !b.ok) return out;
    const int full = std::max({r.p->width, g.p->width, b.p->width});
    const int step = maxWidth > 0 ? std::max(1, full / maxWidth) : 1;
    out.width = full / step;
    const int h = std::min({r.p->lines * (full / r.p->width), g.p->lines * (full / g.p->width), b.p->lines * (full / b.p->width)});
    out.height = h / step;
    out.rgba.resize((size_t)out.width * out.height);
    for (int y = 0; y < out.height; y++) {
        const int fy = y * step;
        for (int x = 0; x < out.width; x++) {
            const int fx = x * step;
            const double rv = std::pow(sampleBand(r, fx * r.p->width / full, fy * r.p->width / full), gamma);
            const double gv = std::pow(sampleBand(g, fx * g.p->width / full, fy * g.p->width / full), gamma);
            const double bv = std::pow(sampleBand(b, fx * b.p->width / full, fy * b.p->width / full), gamma);
            out.rgba[(size_t)y * out.width + x] = rgb(rv, gv, bv);
        }
    }
    return out;
}
// A difference-based RGB on counts: each difference stretched by its own percentiles, no physical scale
XbProduct diffRgb(const std::string& name, const Plane* a, const Plane* b, const Plane* c, const Plane* d, const Plane* e, int maxWidth) {
    XbProduct out; out.name = name; out.note = "uncalibrated (relative contrast only)";
    if (!a || !b || !c || !d || !e || a->lines == 0) return out;
    const int full = a->width;
    if (b->width != full || c->width != full || d->width != full || e->width != full) return out;
    const int lines = std::min({a->lines, b->lines, c->lines, d->lines, e->lines});
    const int step = maxWidth > 0 ? std::max(1, full / maxWidth) : 1;
    out.width = full / step; out.height = lines / step;
    out.rgba.resize((size_t)out.width * out.height);
    std::vector<float> dr, dg, dbv;
    for (int y = 0; y < out.height; y += 4) for (int x = 0; x < out.width; x += 4) {
        const size_t o = (size_t)(y * step) * full + x * step;
        if (!a->px[o] || !c->px[o]) continue;
        dr.push_back((float)a->px[o] - b->px[o]); dg.push_back((float)c->px[o] - d->px[o]); dbv.push_back(e->px[o]);
    }
    if (dr.size() < 16) { out.rgba.clear(); out.width = out.height = 0; return out; }
    const auto pct = [](std::vector<float>& v, double& lo, double& hi) { std::sort(v.begin(), v.end()); lo = v[v.size() / 100]; hi = v[v.size() * 99 / 100]; if (hi <= lo) hi = lo + 1; };
    double lr, hr, lg, hg, lb, hb;
    pct(dr, lr, hr); pct(dg, lg, hg); pct(dbv, lb, hb);
    for (int y = 0; y < out.height; y++) for (int x = 0; x < out.width; x++) {
        const size_t o = (size_t)(y * step) * full + x * step;
        const double r = ((double)a->px[o] - b->px[o] - lr) / (hr - lr), g = ((double)c->px[o] - d->px[o] - lg) / (hg - lg), bb = (e->px[o] - lb) / (hb - lb);
        out.rgba[(size_t)y * out.width + x] = rgb(std::clamp(r, 0.0, 1.0), std::clamp(g, 0.0, 1.0), std::clamp(bb, 0.0, 1.0));
    }
    return out;
}
const Plane* pl(const Store& s, const char* n) { int i = findBand(s, n); return i < 0 ? nullptr : &s.plane(i); }

uint32_t tempColour(double kelvin, double lo, double hi) {
    // a blue to red scale for temperature maps
    const double t = std::clamp((kelvin - lo) / (hi - lo), 0.0, 1.0);
    const double r = std::clamp(1.5 - std::fabs(4 * t - 3), 0.0, 1.0), g = std::clamp(1.5 - std::fabs(4 * t - 2), 0.0, 1.0), b = std::clamp(1.5 - std::fabs(4 * t - 1), 0.0, 1.0);
    return rgb(r, g, b);
}

} // namespace

double avhrrPlanckRadiance(double t, double vc) { return planck(t, vc); }

double avhrrTemperature(const AvhrrCal& cal, int y, int ch, int c) {
    if (y < 0 || y >= (int)cal.lines.size() || ch < 0 || ch > 2) return 0;
    const AvhrrCal::Line& l = cal.lines[(size_t)y];
    const int sat = cal.satIndex > 0.5 ? 1 : 0;
    const ChanSpec& sp = kMetop[sat][ch];
    if (l.nPrt <= 0 || l.cs[ch] < 800 || l.cb[ch] <= 0 || l.cs[ch] <= l.cb[ch]) return 0;
    double tbb = 0;
    for (int i = 0; i < l.nPrt; i++) {
        const double* co = kPrt[sat][i % 4];
        const double v = l.prt[i];
        tbb += co[0] + co[1] * v + co[2] * v * v;
    }
    tbb /= l.nPrt;
    const double tp = sp.A + sp.B * tbb;
    const double nb = planck(tp, sp.vc);
    const double nl = sp.Ns + (nb - sp.Ns) * (l.cs[ch] - c) / (l.cs[ch] - l.cb[ch]);
    const double nE = nl + sp.b0 + sp.b1 * nl + sp.b2 * nl * nl;
    if (nE <= 0) return 0;
    const double te = invPlanck(nE, sp.vc);
    return (te - sp.A) / sp.B;
}

std::vector<std::string> productNames(int profile) {
    switch (profile) {
    case kXbNpp: case kXbJpss2: return {"Natural colour", "True colour (M5, M4, M3)", "Dust (uncalibrated)", "Fog (uncalibrated)"};
    case kXbAquaAura: case kXbTerra: return {"Natural colour", "True colour (1, 4, 3)", "Airmass (uncalibrated)", "Dust (uncalibrated)", "Fog (uncalibrated)"};
    case kXbMetopDump: return {"Natural colour", "Cloud-top temperature", "Sea surface temperature", "Fire"};
    default: return {};
    }
}

std::vector<XbProduct> makeProducts(const Store& s, int profile, const AvhrrCal* cal, int maxWidth) {
    std::vector<XbProduct> out;
    const auto addIf = [&](XbProduct p) { if (p.width > 0 && !p.rgba.empty()) out.push_back(std::move(p)); };
    if (profile == kXbNpp || profile == kXbJpss2) {
        addIf(composite("Natural colour", band(s, "VIIRS M10"), band(s, "VIIRS M7"), band(s, "VIIRS M5"), maxWidth));
        addIf(composite("True colour (M5, M4, M3)", band(s, "VIIRS M5"), band(s, "VIIRS M4"), band(s, "VIIRS M3"), maxWidth));
        addIf(diffRgb("Dust (uncalibrated)", pl(s, "VIIRS M16"), pl(s, "VIIRS M15"), pl(s, "VIIRS M15"), pl(s, "VIIRS M14"), pl(s, "VIIRS M15"), maxWidth));
        addIf(diffRgb("Fog (uncalibrated)", pl(s, "VIIRS M16"), pl(s, "VIIRS M15"), pl(s, "VIIRS M15"), pl(s, "VIIRS M12"), pl(s, "VIIRS M15"), maxWidth));
    } else if (profile == kXbAquaAura || profile == kXbTerra) {
        addIf(composite("Natural colour", band(s, "MODIS 7"), band(s, "MODIS 2"), band(s, "MODIS 1"), maxWidth));
        addIf(composite("True colour (1, 4, 3)", band(s, "MODIS 1"), band(s, "MODIS 4"), band(s, "MODIS 3"), maxWidth));
        addIf(diffRgb("Airmass (uncalibrated)", pl(s, "MODIS 27"), pl(s, "MODIS 28"), pl(s, "MODIS 30"), pl(s, "MODIS 31"), pl(s, "MODIS 27"), maxWidth));
        addIf(diffRgb("Dust (uncalibrated)", pl(s, "MODIS 32"), pl(s, "MODIS 31"), pl(s, "MODIS 31"), pl(s, "MODIS 29"), pl(s, "MODIS 31"), maxWidth));
        addIf(diffRgb("Fog (uncalibrated)", pl(s, "MODIS 32"), pl(s, "MODIS 31"), pl(s, "MODIS 31"), pl(s, "MODIS 22"), pl(s, "MODIS 31"), maxWidth));
    } else if (profile == kXbMetopDump && cal) {
        const Band b1 = band(s, "AVHRR 1"), b2 = band(s, "AVHRR 2"), b4 = band(s, "AVHRR 4");
        addIf(composite("Natural colour", b2, b1, b4, maxWidth));   // AVHRR has no blue; channel 4 inverted is common, here the plain false colour
        const int i3 = findBand(s, "AVHRR 3"), i4 = findBand(s, "AVHRR 4"), i5 = findBand(s, "AVHRR 5");
        if (i4 >= 0 && i5 >= 0 && !cal->lines.empty() && s.plane(i4).lines > 0) {
            const Plane& p4 = s.plane(i4), &p5 = s.plane(i5);
            const int step = maxWidth > 0 ? std::max(1, p4.width / maxWidth) : 1;
            XbProduct ct, sst, fire;
            ct.name = "Cloud-top temperature"; sst.name = "Sea surface temperature"; fire.name = "Fire";
            ct.note = "brightness temperature of channel 4, 190 K blue to 320 K red";
            sst.note = "split window (McClain 1985), not tuned for this satellite, clear sky only, no land mask, 270 K to 305 K";
            fire.note = "channel 3b hot spots over channel 4 (T3b > 310 K and T3b - T4 > 10 K), night passes only";
            const int w = p4.width / step, h = std::min({p4.lines, p5.lines, (int)cal->lines.size()}) / step;
            ct.width = sst.width = fire.width = w; ct.height = sst.height = fire.height = h;
            ct.rgba.resize((size_t)w * h); sst.rgba.resize((size_t)w * h); fire.rgba.resize((size_t)w * h);
            bool any3b = false;
            for (int y = 0; y < h; y++) {
                const int fy = y * step;
                for (int x = 0; x < w; x++) {
                    const int fx = x * step;
                    const int c4 = p4.px[(size_t)fy * p4.width + fx] >> 6, c5 = p5.px[(size_t)fy * p5.width + fx] >> 6;
                    const double t4 = avhrrTemperature(*cal, fy, 1, c4), t5 = avhrrTemperature(*cal, fy, 2, c5);
                    ct.rgba[(size_t)y * w + x] = t4 > 0 ? tempColour(t4, 190, 320) : 0xFF000000u;
                    const double t = t4 > 0 && t5 > 0 ? 1.0346 * t4 + 2.5779 * (t4 - t5) - 283.21 + 273.15 : 0;
                    sst.rgba[(size_t)y * w + x] = t > 240 ? tempColour(t, 270, 305) : 0xFF000000u;
                    uint32_t f = rgb(0, std::clamp((t4 - 190) / 130, 0.0, 1.0) * 0.3, 0);
                    if (i3 >= 0 && cal->lines[(size_t)fy].is3b) {
                        any3b = true;
                        const int c3 = s.plane(i3).px[(size_t)fy * s.plane(i3).width + fx] >> 6;
                        const double t3 = avhrrTemperature(*cal, fy, 0, c3);
                        if (t3 > 310 && t3 - t4 > 10) f = rgb(1, 0.15, 0);
                    }
                    fire.rgba[(size_t)y * w + x] = f;
                }
            }
            addIf(std::move(ct));
            addIf(std::move(sst));
            if (any3b) addIf(std::move(fire));
        }
    }
    return out;
}

} // namespace dect2::xband
