// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: parametric templates. Each takes the design frequency and a few parameters whose defaults come from the usual
// textbook / handbook formulas (docs/ANTENNA.md lists them); the solver then shows how close the formula is.
#include "dect2/antd/antd_tools.h"
#include <algorithm>
#include <cmath>

namespace dect2::antd {

namespace {

double lam(double fHz) { return kC0 / std::max(fHz, 1.0); }
double defRadius(double fHz) { return std::min(0.002, std::max(0.0005, lam(fHz) / 1000)); }
bool hf(double fHz) { return fHz < 30e6; }
double defHeight(double fHz) { return hf(fHz) ? std::min(lam(fHz) / 2, 15.0) : 0.0; }
GroundType defGround(double fHz) { return hf(fHz) ? GroundType::Real : GroundType::Free; }

TParam P(const char* key, const char* label, double v, double lo, double hi, PKind k, const char* tip = "") {
    TParam p; p.key = key; p.label = label; p.value = v; p.lo = lo; p.hi = hi; p.kind = k; p.tip = tip; return p;
}

void W(Model& m, Vec3 a, Vec3 b, double r, double sigma = 0, int seg = 0) {
    Wire w; w.a = a; w.b = b; w.radius = r; w.segments = seg; w.conductivity = sigma; m.wires.push_back(w);
}
void SRC(Model& m, int wire, double pos, cd v = 1.0) { Source s; s.wire = wire; s.pos = pos; s.volts = v; m.sources.push_back(s); }

// a polygon loop in the y-z plane (x = x0) around (y0, z0), n sides, a side centred at the bottom; returns the bottom wire's index
int loopYZ(Model& m, double x0, double y0, double z0, double radius, int n, double r) {
    const int first = (int)m.wires.size();
    for (int i = 0; i < n; ++i) {
        const double a0 = -kPi / 2 - kPi / n + 2 * kPi * i / n, a1 = a0 + 2 * kPi / n;
        W(m, {x0, y0 + radius * std::cos(a0), z0 + radius * std::sin(a0)}, {x0, y0 + radius * std::cos(a1), z0 + radius * std::sin(a1)}, r);
    }
    return first;   // wire 0 of the loop: the bottom side (from -90 - 180/n to -90 + 180/n degrees)
}

} // namespace

const std::vector<TemplateInfo>& templates() {
    static const std::vector<TemplateInfo> t = {
        {"dipole", "Half-wave dipole", "A straight wire, fed in the middle. 142.65 / f(MHz) metres in total.", 50, "len"},
        {"invv", "Inverted-V", "A dipole whose legs slope down from one high point. Slightly longer than a flat dipole.", 50, "leg"},
        {"folded", "Folded dipole", "Two parallel half-wave wires joined at the ends; about 4 x the dipole impedance (300 ohm).", 300, "len"},
        {"ocf", "Off-centre-fed dipole", "A half-wave wire fed about a third from one end: several bands through a 4:1 balun.", 200, "len"},
        {"efhw", "End-fed half-wave", "A half-wave wire fed at one end against a short counterpoise; needs a 49:1 transformer.", 2450, "len"},
        {"g5rv", "G5RV-like", "1.5 wavelength flat top with a half-wave of 450-ohm ladder line, then coax (multiband with a tuner).", 50, "top"},
        {"delta", "Delta loop", "A full-wave triangle, apex up, fed in the middle of the bottom side (horizontal polarisation).", 100, "per"},
        {"quadloop", "Quad loop", "A full-wave square loop, fed in the middle of the bottom side.", 100, "per"},
        {"magloop", "Magnetic (small transmitting) loop", "A small loop of thick tube tuned by a capacitor, fed by a coupling loop a fifth of its size.", 50, "C"},
        {"yagi", "Yagi-Uda", "Reflector, driven element and directors on a boom, from a spacing / length table.", 50, "de"},
        {"quad", "Cubical quad (2 element)", "A driven square loop and a slightly larger reflector loop.", 100, "per"},
        {"vertical", "Vertical with radials", "A quarter-wave vertical with horizontal radials just above the ground.", 50, "len"},
        {"groundplane", "Ground plane", "A quarter-wave vertical with four radials sloping down 45 degrees (about 50 ohm).", 50, "len"},
        {"jpole", "J-pole", "A half-wave radiator on a quarter-wave matching stub, fed at a tap near the bottom.", 50, "stub"},
        {"slimjim", "Slim jim", "A folded half-wave over a quarter-wave stub, fed at a tap near the bottom.", 50, "stub"},
        {"helix", "Axial-mode helix", "One-wavelength turns over a ground plane: circular polarisation for satellites.", 140, ""},
        {"turnstile", "Turnstile", "Two crossed dipoles fed 90 degrees apart (circular overhead), with reflectors below.", 50, "len"},
        {"lpda", "Log-periodic dipole array", "Dipoles scaled by tau, fed by a crossed line: wideband and directional.", 50, ""},
        {"discone", "Discone (sketch)", "A disc over a cone of radial wires: very wideband, vertically polarised.", 50, ""},
    };
    return t;
}

const TemplateInfo* findTemplate(const std::string& id) {
    for (const auto& t : templates()) if (t.id == id) return &t;
    return nullptr;
}

double paramValue(const std::vector<TParam>& p, const std::string& key, double dflt) {
    for (const auto& x : p) if (x.key == key) return x.value;
    return dflt;
}

std::vector<TParam> templateParams(const std::string& id, double f) {
    const double L = lam(f), r = defRadius(f), h = defHeight(f);
    const double fm = f / 1e6;
    std::vector<TParam> p;
    auto H = [&](double v) { p.push_back(P("h", "Height", v, 0, std::max(100.0, 3 * L), PKind::Length, "Height of the feed point above the ground (0 in free space = centred on the origin)")); };
    auto R = [&](double v) { p.push_back(P("r", "Wire radius", v, 1e-5, 0.05, PKind::Length)); };
    if (id == "dipole") { p.push_back(P("len", "Total length", 142.65 / fm, 0.3 * L, 0.7 * L, PKind::Length, "142.65 / f(MHz) m: half a wavelength less about 5 % end effect")); H(h); R(r); }
    else if (id == "invv") {
        p.push_back(P("leg", "Leg length", 146.0 / fm / 2, 0.15 * L, 0.35 * L, PKind::Length, "Each leg: 73 / f(MHz) m (an inverted-V wants about 2 % more than a flat dipole)"));
        p.push_back(P("droop", "Leg droop", 45, 0, 70, PKind::Angle, "Degrees below horizontal (45 = 90 degrees between the legs)"));
        H(hf(f) ? std::max(h, 0.3 * L) : 0.3 * L); R(r);
    } else if (id == "folded") {
        p.push_back(P("len", "Length", 0.47 * L, 0.3 * L, 0.6 * L, PKind::Length));
        p.push_back(P("s", "Spacing", std::max(0.01, L / 100), 0.002, 0.1 * L, PKind::Length, "Between the two wires (about a hundredth of a wavelength)"));
        H(h); R(r);
    } else if (id == "ocf") {
        p.push_back(P("len", "Total length", 0.49 * L, 0.3 * L, 0.7 * L, PKind::Length));
        p.push_back(P("off", "Feed position", 0.33, 0.05, 0.5, PKind::Ratio, "Fraction of the length from one end (a third is usual)"));
        H(h); R(r);
    } else if (id == "efhw") {
        p.push_back(P("len", "Wire length", 0.49 * L, 0.3 * L, 0.7 * L, PKind::Length));
        p.push_back(P("cp", "Counterpoise", 0.05 * L, 0.005 * L, 0.25 * L, PKind::Length, "A short wire hanging down from the feed point (often just the coax screen)"));
        H(hf(f) ? h : 0.25 * L); R(r);
    } else if (id == "g5rv") {
        p.push_back(P("top", "Flat top", 1.47 * L, 0.5 * L, 2.5 * L, PKind::Length, "1.5 wavelengths at the design frequency (31.1 m at 14.15 MHz)"));
        p.push_back(P("ladder", "Ladder line length", 0.49 * L, 0.05 * L, 1.5 * L, PKind::Length, "Half a wavelength of line times its velocity factor (10.36 m of open wire)"));
        p.push_back(P("lz0", "Ladder line impedance", 450, 100, 700, PKind::Ohm));
        p.push_back(P("lvf", "Ladder line velocity factor", 0.97, 0.5, 1, PKind::Ratio));
        H(h); R(r);
    } else if (id == "delta" || id == "quadloop") {
        p.push_back(P("per", "Perimeter", 306.3 / fm, 0.7 * L, 1.4 * L, PKind::Length, "306.3 / f(MHz) m: a full-wave loop resonates a little above one wavelength of wire"));
        H(hf(f) ? std::max(3.0, h - 0.15 * L) : 0.0); R(r);
    } else if (id == "magloop") {
        const double D = std::min(0.15 * L / kPi, 3.0);
        const double a = 0.011, Lh = kMu0 * D / 2 * (std::log(8 * D / 2 / a) - 2);
        const double c = 1.0 / (std::pow(2 * kPi * f, 2) * Lh);
        p.push_back(P("D", "Loop diameter", D, 0.01 * L, 0.1 * L, PKind::Length, "A tenth to a quarter wavelength around"));
        p.push_back(P("tube", "Tube radius", a, 0.001, 0.05, PKind::Length, "Thick copper tube: the loss resistance falls with it"));
        p.push_back(P("C", "Tuning capacitor", c * 1e12, c * 0.3e12, c * 3e12, PKind::Pf, "Starts from the loop's inductance mu0 R (ln(8R/a) - 2), then tuned by the solver"));
        p.push_back(P("cl", "Coupling loop / main", 0.2, 0.08, 0.35, PKind::Ratio, "The coupling loop's diameter against the main loop's (a fifth is the usual start)"));
        H(hf(f) ? 1.5 : 0.0);
        // tune: the capacitor to resonance for a few coupling loop sizes, keep the best match to 50 ohm
        std::vector<TParam> best = p; double bestSwr = 1e30;
        for (double cl : {0.12, 0.16, 0.2, 0.24, 0.28}) {
            std::vector<TParam> q = p;
            for (auto& x : q) if (x.key == "cl") x.value = cl;
            cd z;
            if (!trimParam(id, f, q, "C", nullptr, 50, &z)) continue;
            const double s = vswr(z, 50);
            if (s < bestSwr) { bestSwr = s; best = q; }
        }
        p = best;
    } else if (id == "yagi") {
        p.push_back(P("n", "Elements", 3, 2, 12, PKind::Count));
        p.push_back(P("refl", "Reflector length", 0.495 * L, 0.4 * L, 0.6 * L, PKind::Length));
        p.push_back(P("de", "Driven element length", 0.473 * L, 0.4 * L, 0.55 * L, PKind::Length));
        p.push_back(P("dir", "First director length", 0.440 * L, 0.35 * L, 0.5 * L, PKind::Length, "Each further director 0.4 % of a wavelength shorter"));
        p.push_back(P("sp", "Spacing scale", 1.0, 0.5, 2.0, PKind::Ratio, "Times the table: reflector 0.2, first director 0.1, then 0.2, 0.25, 0.3 wavelength"));
        H(h); R(r);
    } else if (id == "quad") {
        p.push_back(P("per", "Driven loop perimeter", 1.02 * L, 0.8 * L, 1.2 * L, PKind::Length));
        p.push_back(P("rper", "Reflector perimeter", 1.05 * L, 0.8 * L, 1.3 * L, PKind::Length));
        p.push_back(P("sp", "Spacing", 0.15 * L, 0.05 * L, 0.3 * L, PKind::Length));
        H(h); R(r);
    } else if (id == "vertical" || id == "groundplane") {
        p.push_back(P("len", "Vertical length", 71.3 / fm, 0.15 * L, 0.7 * L, PKind::Length, "71.3 / f(MHz) m: a quarter wave less the end effect"));
        p.push_back(P("rad", "Radial length", id == "groundplane" ? 0.25 * L : 0.25 * L, 0.05 * L, 0.6 * L, PKind::Length));
        p.push_back(P("nr", "Radials", 4, 0, 16, PKind::Count));
        p.push_back(P("droop", "Radial droop", id == "groundplane" ? 45 : 0, 0, 80, PKind::Angle, "Degrees below horizontal: 45 brings a ground plane to about 50 ohm"));
        p.push_back(P("h", "Base height", id == "groundplane" ? (hf(f) ? 0.25 * L : 0.0) : std::min(0.5, 0.02 * L), 0, std::max(100.0, 3 * L), PKind::Length,
                      "Height of the feed point (0 over a ground: a monopole into the ground, no radials)"));
        R(r);
    } else if (id == "jpole" || id == "slimjim") {
        p.push_back(P("rad", "Radiator", 0.48 * L, 0.3 * L, 0.6 * L, PKind::Length, "The half-wave top section"));
        p.push_back(P("stub", "Stub", (id == "jpole" ? 0.25 : 0.24) * L, 0.15 * L, 0.35 * L, PKind::Length, "The quarter-wave matching section"));
        p.push_back(P("s", "Spacing", std::max(0.01, L / 50), 0.003, 0.1 * L, PKind::Length));
        p.push_back(P("tap", "Feed tap height", (id == "jpole" ? 0.018 : 0.02) * L, 0.003 * L, 0.12 * L, PKind::Length, "Above the shorted bottom: move it to find 50 ohm"));
        if (id == "slimjim") p.push_back(P("gap", "Gap", 0.012 * L, 0.002 * L, 0.05 * L, PKind::Length));
        H(0); R(r);
    } else if (id == "helix") {
        p.push_back(P("turns", "Turns", 6, 2, 20, PKind::Count));
        p.push_back(P("circ", "Turn circumference", 1.0 * L, 0.75 * L, 1.33 * L, PKind::Length, "3/4 to 4/3 of a wavelength (axial mode)"));
        p.push_back(P("pitch", "Pitch", 0.23 * L, 0.12 * L, 0.35 * L, PKind::Length, "Between turns: C tan(13 degrees) is the classic"));
        p.push_back(P("r", "Wire radius", std::max(0.0005, L / 400), 1e-4, 0.05, PKind::Length));
    } else if (id == "turnstile") {
        p.push_back(P("len", "Dipole length", 0.47 * L, 0.3 * L, 0.6 * L, PKind::Length));
        p.push_back(P("refl", "Reflectors", 1, 0, 1, PKind::Count, "1: crossed reflectors a quarter wave below (more gain overhead)"));
        p.push_back(P("rs", "Reflector spacing", 0.25 * L, 0.1 * L, 0.4 * L, PKind::Length));
        H(h); R(r);
    } else if (id == "lpda") {
        p.push_back(P("flo", "Lowest frequency", fm * 0.75, fm * 0.1, fm, PKind::Freq));
        p.push_back(P("fhi", "Highest frequency", fm * 1.5, fm, fm * 10, PKind::Freq));
        p.push_back(P("tau", "Scale factor tau", 0.9, 0.78, 0.97, PKind::Ratio, "Each element tau times the one before"));
        p.push_back(P("sigma", "Spacing factor sigma", 0.06, 0.03, 0.2, PKind::Ratio, "Spacing = 2 sigma times the element length"));
        p.push_back(P("fz0", "Feeder impedance", 100, 50, 300, PKind::Ohm));
        H(h); R(r);
    } else if (id == "discone") {
        p.push_back(P("flo", "Lowest frequency", fm, fm * 0.1, fm * 10, PKind::Freq));
        p.push_back(P("angle", "Cone half angle", 30, 15, 45, PKind::Angle));
        p.push_back(P("nw", "Wires each", 8, 4, 16, PKind::Count));
        H(hf(f) ? 5.0 : 0.0); R(r);
    }
    return p;
}

Model buildTemplate(const std::string& id, double f, const std::vector<TParam>& prm, std::string* note, const Ground* gnd) {
    Model m;
    m.name = id;
    const double L = lam(f);
    auto v = [&](const char* k, double d) { return paramValue(prm, k, d); };
    const double r = v("r", defRadius(f));
    double h = v("h", defHeight(f));
    m.ground.type = defGround(f);
    if (gnd) m.ground = *gnd;
    // in free space a height only shifts the model; over a ground it is the feed height
    std::string n;
    const bool ownBase = id == "vertical" || id == "groundplane" || id == "jpole" || id == "slimjim" || id == "helix" || id == "delta" ||
                         id == "quadloop" || id == "magloop";
    if (m.ground.type != GroundType::Free && !ownBase && h < 0.02 * L) {
        h = L / 2;
        n += "raised to half a wavelength: it was on the ground. ";
    }
    if (id == "dipole") {
        const double len = v("len", 0.4758 * L);
        W(m, {0, -len / 2, h}, {0, len / 2, h}, r); SRC(m, 0, 0.5);
    } else if (id == "invv") {
        const double leg = v("leg", 0.2435 * L), d = v("droop", 45) * kPi / 180;
        h = std::max(h, leg * std::sin(d) + 0.05 * L * (m.ground.type != GroundType::Free));
        W(m, {0, -leg * std::cos(d), h - leg * std::sin(d)}, {0, 0, h}, r);
        W(m, {0, 0, h}, {0, leg * std::cos(d), h - leg * std::sin(d)}, r);
        SRC(m, 0, 1.0);
        n += "the apex height is raised when the legs would touch the ground";
    } else if (id == "folded") {
        const double len = v("len", 0.47 * L), s = v("s", L / 100);
        W(m, {0, -len / 2, h}, {0, len / 2, h}, r);
        W(m, {0, len / 2, h}, {0, len / 2, h + s}, r);
        W(m, {0, len / 2, h + s}, {0, -len / 2, h + s}, r);
        W(m, {0, -len / 2, h + s}, {0, -len / 2, h}, r);
        SRC(m, 0, 0.5);
    } else if (id == "ocf") {
        const double len = v("len", 0.49 * L), off = v("off", 0.33);
        W(m, {0, -len / 2, h}, {0, len / 2, h}, r); SRC(m, 0, off);
    } else if (id == "efhw") {
        const double len = v("len", 0.49 * L), cp = v("cp", 0.05 * L);
        if (m.ground.type != GroundType::Free) h = std::max(h, cp + 0.01 * L);
        W(m, {0, 0, h}, {0, len, h}, r);
        W(m, {0, 0, h - cp}, {0, 0, h}, r);
        SRC(m, 0, 0.0);
        n += "the counterpoise stands for the coax screen; the real feed impedance depends on it";
    } else if (id == "g5rv") {
        const double top = v("top", 1.47 * L);
        W(m, {0, -top / 2, h}, {0, top / 2, h}, r);
        TLine t; t.wire1 = 0; t.pos1 = 0.5; t.z0 = v("lz0", 450); t.vf = v("lvf", 0.97); t.length = v("ladder", 0.49 * L);
        m.lines.push_back(t);
        Source s; s.line = 0; s.volts = 1.0; m.sources.push_back(s);
        n += "the source drives the bottom of the ladder line (where the coax joins); a tuner covers the other bands";
    } else if (id == "delta") {
        const double s = v("per", 1.02 * L) / 3;
        const double z0 = m.ground.type != GroundType::Free ? std::max(h, 0.02 * L) : h;
        W(m, {0, -s / 2, z0}, {0, s / 2, z0}, r);
        W(m, {0, s / 2, z0}, {0, 0, z0 + s * std::sqrt(3.0) / 2}, r);
        W(m, {0, 0, z0 + s * std::sqrt(3.0) / 2}, {0, -s / 2, z0}, r);
        SRC(m, 0, 0.5);
        n += "fed in the middle of the bottom side: horizontal polarisation, about 100-120 ohm (a 75-ohm quarter-wave transformer)";
    } else if (id == "quadloop") {
        const double s = v("per", 1.02 * L) / 4;
        const double z0 = m.ground.type != GroundType::Free ? std::max(h, 0.02 * L) : h - s / 2;
        W(m, {0, -s / 2, z0}, {0, s / 2, z0}, r);
        W(m, {0, s / 2, z0}, {0, s / 2, z0 + s}, r);
        W(m, {0, s / 2, z0 + s}, {0, -s / 2, z0 + s}, r);
        W(m, {0, -s / 2, z0 + s}, {0, -s / 2, z0}, r);
        SRC(m, 0, 0.5);
    } else if (id == "magloop") {
        const double D = v("D", 1.0), a = v("tube", 0.011), cpf = v("C", 100), cl = v("cl", 0.2);
        const double R0 = D / 2, zc = std::max(h, 0.0) + R0;
        const int N = 16;
        const int b = loopYZ(m, 0, 0, zc, R0 / std::cos(kPi / N), N, a);
        for (int i = b; i < b + N; ++i) m.wires[i].conductivity = 5.8e7;
        Load c; c.wire = b + N / 2; c.pos = 0.5; c.type = LoadType::SeriesRLC; c.c = cpf * 1e-12; m.loads.push_back(c);   // the top side
        const double rc = R0 * cl, rw = std::max(0.0008, a / 8);
        const double zcc = zc - R0 + rc + 4 * (a + rw);
        const int cb = loopYZ(m, 0, 0, zcc, rc / std::cos(kPi / 8), 8, rw);
        for (int i = cb; i < cb + 8; ++i) m.wires[i].conductivity = 5.8e7;
        SRC(m, cb, 0.5);
        n += "copper; tune C for the lowest SWR (the optimiser can): the bandwidth is only a few kHz on HF";
    } else if (id == "yagi") {
        const int ne = std::max(2, std::min(12, (int)std::lround(v("n", 3))));
        const double refl = v("refl", 0.495 * L), de = v("de", 0.473 * L), d1 = v("dir", 0.44 * L), sp = v("sp", 1.0);
        static const double kSp[] = {0.2, 0.1, 0.2, 0.25, 0.3, 0.3, 0.32, 0.34, 0.35, 0.35, 0.35};
        double x = 0;
        W(m, {x, -refl / 2, h}, {x, refl / 2, h}, r);
        x += kSp[0] * L * sp;
        W(m, {x, -de / 2, h}, {x, de / 2, h}, r);
        SRC(m, 1, 0.5);
        for (int i = 0; i < ne - 2; ++i) {
            x += kSp[1 + i] * L * sp;
            const double dl = std::max(0.38 * L, d1 - 0.004 * L * i);
            W(m, {x, -dl / 2, h}, {x, dl / 2, h}, r);
        }
        translate(m, {-x / 2, 0, 0});   // the boom centred on the origin
        n += "beam toward +x (phi 0); the driven element impedance drops with directors: trim it or match it (gamma / hairpin)";
    } else if (id == "quad") {
        const double sd = v("per", 1.02 * L) / 4, sr = v("rper", 1.05 * L) / 4, sp = v("sp", 0.15 * L);
        auto sq = [&](double x0, double s) {
            const double z0 = h - s / 2;
            W(m, {x0, -s / 2, z0}, {x0, s / 2, z0}, r);
            W(m, {x0, s / 2, z0}, {x0, s / 2, z0 + s}, r);
            W(m, {x0, s / 2, z0 + s}, {x0, -s / 2, z0 + s}, r);
            W(m, {x0, -s / 2, z0 + s}, {x0, -s / 2, z0}, r);
        };
        if (m.ground.type != GroundType::Free) h = std::max(h, sr / 2 + 0.05 * L);
        sq(-sp / 2, sr);
        const int de = (int)m.wires.size();
        sq(sp / 2, sd);
        SRC(m, de, 0.5);
        n += "beam toward +x; the loops' centres are at the height";
    } else if (id == "vertical" || id == "groundplane") {
        const double len = v("len", 0.2378 * L), rad = v("rad", 0.25 * L), d = v("droop", 0) * kPi / 180;
        const int nr = std::max(0, std::min(16, (int)std::lround(v("nr", 4))));
        if (!gnd && id == "vertical") m.ground.type = hf(f) ? GroundType::Real : GroundType::Perfect;
        const bool onGround = m.ground.type != GroundType::Free && h <= 1e-6;
        if (!onGround && m.ground.type != GroundType::Free) h = std::max(h, rad * std::sin(d) + 0.01 * L);
        W(m, {0, 0, onGround ? 0.0 : h}, {0, 0, (onGround ? 0.0 : h) + len}, r);
        if (!onGround)
            for (int i = 0; i < nr; ++i) {
                const double az = 2 * kPi * i / std::max(nr, 1);
                W(m, {0, 0, h}, {rad * std::cos(d) * std::cos(az), rad * std::cos(d) * std::sin(az), h - rad * std::sin(d)}, r);
            }
        SRC(m, 0, 0.0);
        if (onGround) n += "a monopole fed against the ground (the radials are the ground here)";
        else if (nr == 0) n += "no radials: the feed has nothing to work against (add radials)";
    } else if (id == "jpole" || id == "slimjim") {
        const double rad = v("rad", 0.48 * L), stub = v("stub", 0.25 * L), s = v("s", L / 50), tap = std::min(v("tap", 0.018 * L), stub * 0.9);
        if (m.ground.type != GroundType::Free) h = std::max(h, 0.05 * L);
        W(m, {0, 0, h}, {0, 0, h + tap}, r);                 // 0 long leg, bottom part
        if (id == "jpole") {
            W(m, {0, 0, h + tap}, {0, 0, h + stub + rad}, r);   // 1 long leg up to the top
            W(m, {0, s, h}, {0, s, h + tap}, r);                 // 2 short leg bottom
            W(m, {0, s, h + tap}, {0, s, h + stub}, r);          // 3 short leg top
        } else {
            const double gap = v("gap", 0.012 * L);
            W(m, {0, 0, h + tap}, {0, 0, h + stub + rad}, r);   // 1: the long leg up through the folded half-wave
            W(m, {0, s, h}, {0, s, h + tap}, r);
            W(m, {0, s, h + tap}, {0, s, h + stub}, r);
            W(m, {0, 0, h + stub + rad}, {0, s, h + stub + rad}, r);
            W(m, {0, s, h + stub + rad}, {0, s, h + stub + gap}, r);
        }
        W(m, {0, 0, h}, {0, s, h}, r);                       // the shorted bottom
        const int fw = (int)m.wires.size();
        W(m, {0, 0, h + tap}, {0, s, h + tap}, r);           // the feed across the stub at the tap
        SRC(m, fw, 0.5);
        n += id == "jpole" ? "move the tap for 50 ohm; the stub's own length sets the resonance" : "the folded top is a half-wave of wire bent in two";
    } else if (id == "helix") {
        const int turns = std::max(1, (int)std::lround(v("turns", 6)));
        const double C = v("circ", L), S = v("pitch", 0.23 * L), R0 = C / (2 * kPi);
        if (!gnd) m.ground.type = GroundType::Perfect;
        const double z0 = 0.02 * L;
        const int per = 16;
        W(m, {R0, 0, 0}, {R0, 0, z0}, r);
        Vec3 prev{R0, 0, z0};
        for (int i = 1; i <= turns * per; ++i) {
            const double a = 2 * kPi * i / per;
            const Vec3 p{R0 * std::cos(a), R0 * std::sin(a), z0 + S * i / per};
            W(m, prev, p, r, 0, 1);
            prev = p;
        }
        SRC(m, 0, 0.0);
        n += "over a perfect ground standing for the reflector (a real one is about a wavelength across); beam straight up";
    } else if (id == "turnstile") {
        const double len = v("len", 0.47 * L), rs = v("rs", 0.25 * L);
        const bool refl = v("refl", 1) > 0.5;
        if (m.ground.type != GroundType::Free) h = std::max(h, rs + 0.05 * L);
        W(m, {-len / 2, 0, h}, {len / 2, 0, h}, r);
        W(m, {0, -len / 2, h + 6 * r}, {0, len / 2, h + 6 * r}, r);   // a little above the first: crossed, not joined
        SRC(m, 0, 0.5, 1.0); SRC(m, 1, 0.5, cd(0, -1));
        if (refl) {
            const double rl = len * 1.05;
            W(m, {-rl / 2, 0, h - rs}, {rl / 2, 0, h - rs}, r);
            W(m, {0, -rl / 2, h - rs + 6 * r}, {0, rl / 2, h - rs + 6 * r}, r);
        }
        n += "two sources 90 degrees apart (the phasing line); circular overhead, the beam straight up";
    } else if (id == "lpda") {
        const double flo = v("flo", f / 1e6 * 0.75) * 1e6, fhi = v("fhi", f / 1e6 * 1.5) * 1e6, tau = v("tau", 0.9), sg = v("sigma", 0.06);
        double len = 0.5 * kC0 / flo * 1.05, x = 0;
        const double lmin = 0.5 * kC0 / fhi * 0.9;
        std::vector<double> xs;
        for (int i = 0; i < 40 && len >= lmin; ++i) {
            W(m, {x, -len / 2, h}, {x, len / 2, h}, r);
            xs.push_back(x);
            x += 2 * sg * len;
            len *= tau;
        }
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            TLine t; t.wire1 = (int)i; t.wire2 = (int)i + 1; t.pos1 = t.pos2 = 0.5; t.z0 = v("fz0", 100); t.vf = 1; t.crossed = true;
            t.length = xs[i + 1] - xs[i];
            m.lines.push_back(t);
        }
        SRC(m, (int)xs.size() - 1, 0.5);
        translate(m, {-xs.back() / 2, 0, 0});
        if (xs.size() > 20) n += "many elements: a smaller frequency range or a smaller tau solves faster";
        else n += std::to_string(xs.size()) + " elements; fed at the short (front) end, beam toward +x";
    } else if (id == "discone") {
        const double flo = v("flo", f / 1e6) * 1e6, ang = v("angle", 30) * kPi / 180;
        const int nw = std::max(4, std::min(16, (int)std::lround(v("nw", 8))));
        const double slant = 0.25 * kC0 / flo, rc = slant * std::sin(ang), rdisc = 0.7 * rc;
        const double gap = std::max(0.004 * kC0 / flo, 4 * r);
        if (m.ground.type != GroundType::Free) h = std::max(h, slant * std::cos(ang) + 0.05 * kC0 / flo);
        W(m, {0, 0, h}, {0, 0, h + gap}, r, 0, 2);   // 0: the feed between the cone's apex and the disc
        for (int i = 0; i < nw; ++i) {
            const double a = 2 * kPi * i / nw;
            W(m, {0, 0, h + gap}, {rdisc * std::cos(a), rdisc * std::sin(a), h + gap}, r);
            W(m, {0, 0, h}, {rc * std::cos(a), rc * std::sin(a), h - slant * std::cos(ang)}, r);
        }
        SRC(m, 0, 0.5);
        n += "a sketch: real discones use many more wires or sheet metal; the impedance is the rough one";
    }
    if (note) *note = n;
    return m;
}

} // namespace dect2::antd
