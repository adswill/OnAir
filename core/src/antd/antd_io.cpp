// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: files. NEC-2 card decks (the common geometry, ground, source, load, line and frequency cards) both ways, the OnAir
// pattern file the propagation tool reads (docs/ANTENNA.md), sweep and current tables.
#include "dect2/antd/antd_tools.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace dect2::antd {

namespace {

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f); std::vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

// the NEC segment holding position pos on a wire of n segments (1-based), and the count to write so a centre feed is on a segment centre
int segOf(double pos, int n) { return std::max(1, std::min(n, (int)std::floor(pos * n) + 1)); }

} // namespace

std::string writeNec(const Model& m, double f0, double f1, int nf) {
    std::string o;
    o += "CM " + (m.name.empty() ? std::string("OnAir antenna") : m.name) + "\n";
    o += "CM written by OnAir (Tools > Antenna designer)\n";
    for (const auto& t : m.lines) if (t.vf != 1 || t.lossDbPerM != 0) { o += "CM transmission line velocity factor written as a longer line; loss not in NEC TL\n"; break; }
    o += "CE\n";
    const double fmax = std::max(f0, f1);
    std::vector<int> nseg(m.wires.size());
    for (size_t i = 0; i < m.wires.size(); ++i) {
        const Wire& w = m.wires[i];
        int n = w.segments > 0 ? w.segments : autoSegments(w, fmax, m.segPerWavelength, 1);
        bool centre = false;
        for (const auto& s : m.sources) if (s.line < 0 && s.wire == (int)i && std::fabs(s.pos - 0.5) < 1e-6) centre = true;
        for (const auto& l : m.loads) if (l.wire == (int)i && std::fabs(l.pos - 0.5) < 1e-6) centre = true;
        for (const auto& t : m.lines) if ((t.wire1 == (int)i && std::fabs(t.pos1 - 0.5) < 1e-6) || (t.wire2 == (int)i && std::fabs(t.pos2 - 0.5) < 1e-6)) centre = true;
        if (centre && n % 2 == 0) ++n;
        nseg[i] = n;
        o += fmt("GW %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n", (int)i + 1, n, w.a.x, w.a.y, w.a.z, w.b.x, w.b.y, w.b.z, w.radius);
    }
    // a source driving a line end needs a wire: a short stub away from the antenna
    std::map<int, int> stubTag;
    {
        int tag = (int)m.wires.size() + 1;
        const double ext = modelExtent(m), lam = kC0 / std::max(fmax, 1.0);
        double xmin = 0, zmax = 0;
        for (const auto& w : m.wires) { xmin = std::min({xmin, w.a.x, w.b.x}); zmax = std::max({zmax, w.a.z, w.b.z}); }
        for (const auto& s : m.sources) {
            if (s.line < 0) continue;
            const double x = xmin - std::max(ext, lam), z = m.ground.type != GroundType::Free ? std::max(1.0, zmax / 2) : 0;
            const double l = lam / 200;
            o += fmt("GW %d 1 %.6g 0 %.6g %.6g 0 %.6g %.6g\n", tag, x, z, x + l, z, l / 100);
            stubTag[s.line] = tag++;
        }
    }
    o += fmt("GE %d\n", m.ground.type == GroundType::Free ? 0 : 1);
    if (m.ground.type == GroundType::Perfect) o += "GN 1\n";
    else if (m.ground.type == GroundType::Real) o += fmt("GN 0 0 0 0 %.6g %.6g\n", m.ground.epsr, m.ground.sigma);
    for (size_t i = 0; i < m.wires.size(); ++i)
        if (m.wires[i].conductivity > 0) o += fmt("LD 5 %d 0 0 %.6g\n", (int)i + 1, m.wires[i].conductivity);
    for (const auto& l : m.loads) {
        if (l.wire < 0 || l.wire >= (int)m.wires.size()) continue;
        const int sg = segOf(l.pos, nseg[l.wire]);
        if (l.type == LoadType::RX) o += fmt("LD 4 %d %d %d %.6g %.6g\n", l.wire + 1, sg, sg, l.r, l.x);
        else o += fmt("LD %d %d %d %d %.6g %.6g %.6g\n", l.type == LoadType::SeriesRLC ? 0 : 1, l.wire + 1, sg, sg, l.r, l.l, l.c);
    }
    for (size_t i = 0; i < m.lines.size(); ++i) {
        const TLine& t = m.lines[i];
        if (t.wire1 < 0 || t.wire1 >= (int)m.wires.size()) continue;
        int tag2, s2;
        if (stubTag.count((int)i)) { tag2 = stubTag[(int)i]; s2 = 1; }
        else { if (t.wire2 < 0 || t.wire2 >= (int)m.wires.size()) continue; tag2 = t.wire2 + 1; s2 = segOf(t.pos2, nseg[t.wire2]); }
        const double len = t.length > 0 ? t.length / std::max(t.vf, 0.01) : 0;
        o += fmt("TL %d %d %d %d %.6g %.6g 0 0 0 0\n", t.wire1 + 1, segOf(t.pos1, nseg[t.wire1]), tag2, s2, t.crossed ? -t.z0 : t.z0, len);
    }
    o += fmt("FR 0 %d 0 0 %.6g %.6g\n", std::max(1, nf), f0 / 1e6, nf > 1 ? (f1 - f0) / 1e6 / (nf - 1) : 0.0);
    for (const auto& s : m.sources) {
        if (s.line >= 0) { if (stubTag.count(s.line)) o += fmt("EX 0 %d 1 0 %.6g %.6g\n", stubTag[s.line], s.volts.real(), s.volts.imag()); continue; }
        if (s.wire < 0 || s.wire >= (int)m.wires.size()) continue;
        o += fmt("EX 0 %d %d 0 %.6g %.6g\n", s.wire + 1, segOf(s.pos, nseg[s.wire]), s.volts.real(), s.volts.imag());
    }
    o += m.ground.type == GroundType::Free ? "RP 0 37 73 1000 0 0 5 5\n" : "RP 0 19 73 1000 0 0 5 5\n";
    o += "EN\n";
    return o;
}

bool readNec(const std::string& text, Model& m, double* f0, double* f1, int* nf, std::string* err) {
    m = Model();
    std::istringstream in(text);
    std::string line;
    struct WT { int tag, nseg; };
    std::vector<WT> wt;   // per wire
    struct Pend { char kind; std::vector<double> v; };
    std::vector<Pend> pend;   // EX, LD, TL: resolved after the geometry
    int ln = 0;
    bool any = false;
    double scale = 1;
    (void)scale;
    auto tagWires = [&](int tag) { std::vector<int> r; for (size_t i = 0; i < wt.size(); ++i) if (wt[i].tag == tag || tag == 0) r.push_back((int)i); return r; };
    auto addW = [&](int tag, int ns, Vec3 a, Vec3 b, double rad) {
        Wire w; w.a = a; w.b = b; w.radius = rad; w.segments = ns; w.tag = tag; m.wires.push_back(w); wt.push_back({tag, ns});
    };
    while (std::getline(in, line)) {
        ++ln;
        for (char& c : line) if (c == ',' || c == '\t' || c == '\r') c = ' ';
        if (line.size() < 2) continue;
        std::string card = line.substr(0, 2);
        for (char& c : card) c = (char)std::toupper((unsigned char)c);
        std::istringstream ls(line.substr(2));
        std::vector<double> v; std::string tok;
        while (ls >> tok) { char* e = nullptr; const double d = std::strtod(tok.c_str(), &e); if (e && *e == 0) v.push_back(d); else break; }
        auto at = [&](size_t i) { return i < v.size() ? v[i] : 0.0; };
        if (card == "CM" || card == "CE") { if (card == "CM" && m.name.empty()) { m.name = line.size() > 3 ? line.substr(3) : ""; } continue; }
        if (card == "GW") {
            if (v.size() < 9) { if (err) *err = "line " + std::to_string(ln) + ": GW needs 9 numbers"; return false; }
            addW((int)at(0), (int)at(1), {at(2), at(3), at(4)}, {at(5), at(6), at(7)}, at(8));
            any = true;
        } else if (card == "GA") {   // arc in the x-z plane around the origin
            const int tag = (int)at(0), ns = std::max(1, (int)at(1));
            const double r = at(2), a1 = at(3) * kPi / 180, a2 = at(4) * kPi / 180, rad = at(5);
            for (int i = 0; i < ns; ++i) {
                const double t0 = a1 + (a2 - a1) * i / ns, t1 = a1 + (a2 - a1) * (i + 1) / ns;
                addW(tag, 1, {r * std::cos(t0), 0, r * std::sin(t0)}, {r * std::cos(t1), 0, r * std::sin(t1)}, rad);
            }
            any = true;
        } else if (card == "GH") {   // NEC-2 helix: ITG NS S HL A1 B1 A2 B2 RAD (along +z from the origin)
            const int tag = (int)at(0), ns = std::max(1, (int)at(1));
            const double s = at(2), hl = at(3), a1 = at(4), b1 = at(5), a2 = at(6), b2 = at(7), rad = at(8);
            const double turns = s != 0 ? std::fabs(hl) / s : 1, hand = hl < 0 ? -1 : 1;
            Vec3 prev;
            for (int i = 0; i <= ns; ++i) {
                const double t = (double)i / ns, ang = 2 * kPi * turns * t * hand;
                const double ax = a1 + (a2 - a1) * t, by = (b1 != 0 || b2 != 0) ? b1 + (b2 - b1) * t : ax;
                const Vec3 p{ax * std::cos(ang), by * std::sin(ang), std::fabs(hl) * t};
                if (i) addW(tag, 1, prev, p, rad);
                prev = p;
            }
            any = true;
        } else if (card == "GS") {
            const double k = at(2) != 0 ? at(2) : 1;
            for (auto& w : m.wires) { w.a = w.a * k; w.b = w.b * k; w.radius *= k; }
        } else if (card == "GM") {   // ITGI NRPT ROX ROY ROZ XS YS ZS ITS
            const int itgi = (int)at(0), nrpt = (int)at(1), its = (int)at(8);
            const double rx = at(2) * kPi / 180, ry = at(3) * kPi / 180, rz = at(4) * kPi / 180;
            const Vec3 d{at(5), at(6), at(7)};
            auto xf = [&](Vec3 p) {
                p = {p.x, p.y * std::cos(rx) - p.z * std::sin(rx), p.y * std::sin(rx) + p.z * std::cos(rx)};
                p = {p.x * std::cos(ry) + p.z * std::sin(ry), p.y, -p.x * std::sin(ry) + p.z * std::cos(ry)};
                p = {p.x * std::cos(rz) - p.y * std::sin(rz), p.x * std::sin(rz) + p.y * std::cos(rz), p.z};
                return p + d;
            };
            std::vector<int> sel;
            for (size_t i = 0; i < wt.size(); ++i) if (wt[i].tag >= its) sel.push_back((int)i);
            if (nrpt == 0) { for (int i : sel) { m.wires[i].a = xf(m.wires[i].a); m.wires[i].b = xf(m.wires[i].b); } }
            else {
                std::vector<int> last = sel;
                for (int r = 0; r < nrpt; ++r) {
                    std::vector<int> next;
                    for (int i : last) {
                        Wire w = m.wires[i]; w.a = xf(w.a); w.b = xf(w.b);
                        const int tag = wt[i].tag > 0 ? wt[i].tag + itgi : 0;
                        addW(tag, wt[i].nseg, w.a, w.b, w.radius);
                        m.wires.back().conductivity = w.conductivity;
                        next.push_back((int)m.wires.size() - 1);
                    }
                    last = next;
                }
            }
        } else if (card == "GE" || card == "GX" || card == "GR" || card == "GC" || card == "GF" || card == "SP" || card == "SM" || card == "SC") {
            if (card != "GE") { if (err) *err = "line " + std::to_string(ln) + ": " + card + " is not supported (wires, arcs, helices, moves only)"; return false; }
        } else if (card == "GN") {
            const int t = (int)at(0);
            if (t == -1) m.ground.type = GroundType::Free;
            else if (t == 1) m.ground.type = GroundType::Perfect;
            else { m.ground.type = GroundType::Real; if (at(4) > 0) m.ground.epsr = at(4); if (at(5) > 0) m.ground.sigma = at(5); }
        } else if (card == "EX" || card == "LD" || card == "TL") {
            pend.push_back({card[0], v});
        } else if (card == "FR") {
            const int n = std::max(1, (int)at(1));
            const double fa = at(4) * 1e6, st = at(5) * 1e6;
            if (f0) *f0 = fa;
            if (f1) *f1 = (int)at(0) == 1 ? fa * std::pow(std::max(st / 1e6, 1.0), n - 1) : fa + st * (n - 1);
            if (nf) *nf = n;
        } else if (card == "EN") break;
        // RP, XQ, NE, NH, PT, PQ, EK, KH, NX, WG: output requests and options, ignored
    }
    if (!any) { if (err) *err = "no wires (GW / GA / GH cards)"; return false; }
    auto posOf = [&](int wi, int seg) { const int n = std::max(1, wt[wi].nseg); return std::min(1.0, std::max(0.0, (seg - 0.5) / n)); };
    auto findSeg = [&](int tag, int seg, int& wi, double& pos) {
        // NEC numbers a tag's segments through all its wires (GA / GH arcs are one tag)
        int count = 0;
        for (size_t i = 0; i < wt.size(); ++i) {
            if (wt[i].tag != tag) continue;
            const int n = std::max(1, wt[i].nseg);
            if (seg <= count + n) { wi = (int)i; pos = posOf((int)i, seg - count); return true; }
            count += n;
        }
        return false;
    };
    for (const Pend& p : pend) {
        auto at = [&](size_t i) { return i < p.v.size() ? p.v[i] : 0.0; };
        if (p.kind == 'E') {
            if ((int)at(0) != 0) continue;   // only voltage sources
            Source s; double pos; int wi;
            if (!findSeg((int)at(1), (int)at(2), wi, pos)) continue;
            // a wire end on the ground fed at its first segment: the feed is at the ground (monopole)
            const Wire& w = m.wires[wi];
            if (m.ground.type != GroundType::Free && wt[wi].nseg > 0 && (int)at(2) == 1 && std::fabs(w.a.z) < 1e-6) pos = 0;
            s.wire = wi; s.pos = pos; s.volts = cd(at(4), at(5));
            m.sources.push_back(s);
        } else if (p.kind == 'L') {
            const int type = (int)at(0), tag = (int)at(1);
            if (type == 5) { for (int wi : tagWires(tag)) m.wires[wi].conductivity = at(4); continue; }
            if (type != 0 && type != 1 && type != 4) continue;
            int wi; double pos;
            const int seg = std::max(1, (int)at(2));
            if (!findSeg(tag, seg, wi, pos)) continue;
            Load l; l.wire = wi; l.pos = pos;
            if (type == 4) { l.type = LoadType::RX; l.r = at(4); l.x = at(5); }
            else { l.type = type == 0 ? LoadType::SeriesRLC : LoadType::ParallelRLC; l.r = at(4); l.l = at(5); l.c = at(6); }
            m.loads.push_back(l);
        } else {
            TLine t; int w1, w2; double p1, p2;
            if (!findSeg((int)at(0), (int)at(1), w1, p1) || !findSeg((int)at(2), (int)at(3), w2, p2)) continue;
            t.wire1 = w1; t.pos1 = p1; t.wire2 = w2; t.pos2 = p2;
            t.z0 = std::fabs(at(4)); t.crossed = at(4) < 0; t.length = at(5);
            m.lines.push_back(t);
        }
    }
    // NEC segment counts from GW are kept; the solver adds a node at each source / load position
    if (m.name.empty()) m.name = "NEC import";
    while (!m.name.empty() && m.name.front() == ' ') m.name.erase(m.name.begin());
    return true;
}

std::string writePatternCsv(const Model& m, const Solution& s, double azStep, double elStep) {
    azStep = std::max(0.5, azStep); elStep = std::max(0.5, elStep);
    const bool gnd = m.ground.type != GroundType::Free;
    std::string o = "# OnAir antenna pattern v1\n";
    o += "# name: " + (m.name.empty() ? std::string("antenna") : m.name) + "\n";
    o += fmt("# freq_mhz: %.6f\n", s.fHz / 1e6);
    if (m.ground.type == GroundType::Free) o += "# ground: free\n";
    else if (m.ground.type == GroundType::Perfect) o += "# ground: perfect\n";
    else o += fmt("# ground: real epsr=%.3g sigma=%.4g\n", m.ground.epsr, m.ground.sigma);
    double gmax = -999;
    std::string rows;
    const double elMin = gnd ? 0 : -90;
    for (double el = elMin; el <= 90 + 1e-9; el += elStep)
        for (double az = 0; az < 360 - 1e-9; az += azStep) {
            const double th = 90 - el, ph = std::fmod(90 - az + 720, 360);   // compass bearing -> phi from +x counter-clockwise
            const Gain g = gainAt(m, s, th, ph);
            gmax = std::max(gmax, g.total);
            rows += fmt("%g,%g,%.2f,%.2f,%.2f\n", az, el, std::max(g.total, -99.0), std::max(g.vert, -99.0), std::max(g.horiz, -99.0));
        }
    o += fmt("# max_gain_dbi: %.2f\n", gmax);
    o += fmt("# az_step_deg: %g\n# el_step_deg: %g\n# el_min_deg: %g\n", azStep, elStep, elMin);
    o += "# azimuth: compass bearing in degrees, 0 = the model's +y (north), 90 = +x (east)\n";
    o += "# elevation: degrees above the horizon; gains in dBi (total, vertical, horizontal), -99 = none\n";
    o += "az_deg,el_deg,gain_dbi,gain_v_dbi,gain_h_dbi\n";
    return o + rows;
}

bool readPatternCsv(const std::string& text, PatternFile& p, std::string* err) {
    p = PatternFile();
    std::istringstream in(text);
    std::string line;
    struct Row { double az, el, g; };
    std::vector<Row> rows;
    bool header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            const size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string k = line.substr(1, c - 1), v = line.substr(c + 1);
            while (!k.empty() && k.front() == ' ') k.erase(k.begin());
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            if (k == "name") p.name = v;
            else if (k == "freq_mhz") p.fMHz = std::atof(v.c_str());
            else if (k == "max_gain_dbi") p.maxDbi = std::atof(v.c_str());
            continue;
        }
        if (!header && line.find("az_deg") != std::string::npos) { header = true; continue; }
        double az, el, g;
        if (std::sscanf(line.c_str(), "%lf,%lf,%lf", &az, &el, &g) == 3) rows.push_back({az, el, g});
    }
    if (rows.empty()) { if (err) *err = "no pattern rows"; return false; }
    std::set<double> azs, els;
    for (const Row& r : rows) { azs.insert(std::round(r.az * 1000) / 1000); els.insert(std::round(r.el * 1000) / 1000); }
    p.nAz = (int)azs.size(); p.nEl = (int)els.size();
    if (p.nAz < 2 || p.nEl < 1) { if (err) *err = "the pattern needs at least two azimuths"; return false; }
    const std::vector<double> av(azs.begin(), azs.end()), ev(els.begin(), els.end());
    p.azStep = av[1] - av[0]; p.elStep = p.nEl > 1 ? ev[1] - ev[0] : 1;
    p.elMin = ev.front(); p.elMax = ev.back();
    p.g.assign((size_t)p.nAz * p.nEl, -99.f);
    for (const Row& r : rows) {
        const int i = (int)std::lround((r.el - p.elMin) / p.elStep), j = (int)std::lround((r.az - av[0]) / p.azStep);
        if (i >= 0 && i < p.nEl && j >= 0 && j < p.nAz) p.g[(size_t)i * p.nAz + j] = (float)r.g;
    }
    if (p.maxDbi <= -999) for (float g : p.g) p.maxDbi = std::max(p.maxDbi, (double)g);
    return true;
}

double patternGainDbi(const PatternFile& p, double az, double el) {
    if (p.nAz < 2 || p.g.empty()) return -999;
    if (el < p.elMin - 1e-9 || el > p.elMax + 1e-9) return -999;
    az = std::fmod(az, 360.0); if (az < 0) az += 360;
    const double fj = az / p.azStep, fi = p.nEl > 1 ? (el - p.elMin) / p.elStep : 0;
    const int j0 = (int)std::floor(fj) % p.nAz, j1 = (j0 + 1) % p.nAz;
    const int i0 = std::max(0, std::min(p.nEl - 1, (int)std::floor(fi))), i1 = std::min(p.nEl - 1, i0 + 1);
    const double tj = fj - std::floor(fj), ti = std::min(1.0, std::max(0.0, fi - i0));
    auto g = [&](int i, int j) { return (double)p.g[(size_t)i * p.nAz + j]; };
    return (1 - ti) * ((1 - tj) * g(i0, j0) + tj * g(i0, j1)) + ti * ((1 - tj) * g(i1, j0) + tj * g(i1, j1));
}

std::string writeSweepCsv(const std::vector<SweepPoint>& pts, double z0) {
    std::string o = fmt("f_mhz,r_ohm,x_ohm,swr_%g,return_loss_db,gain_dbi,fb_db\n", z0);
    for (const auto& p : pts) {
        if (!p.ok) continue;
        const Mismatch mm = fromZ(p.z, z0);
        o += fmt("%.6f,%.3f,%.3f,%.3f,%.2f,%s,%s\n", p.fHz / 1e6, p.z.real(), p.z.imag(), p.swr, mm.returnLossDb,
                 p.gainDbi > -999 ? fmt("%.2f", p.gainDbi).c_str() : "", p.gainDbi > -999 ? fmt("%.2f", p.fbDb).c_str() : "");
    }
    return o;
}

std::string writeCurrentsCsv(const Model& m, const Solution& s) {
    (void)m;
    std::string o = "wire,segment,x_m,y_m,z_m,current_ma,phase_deg\n";
    std::vector<int> segNo(m.wires.size(), 0);
    for (size_t i = 0; i < s.mesh.segs.size(); ++i) {
        const Segment& sg = s.mesh.segs[i];
        const Vec3 c = (sg.a + sg.b) * 0.5;
        const cd I = 0.5 * (s.segI[i].ia + s.segI[i].ib);
        const int n = sg.wire >= 0 && sg.wire < (int)segNo.size() ? ++segNo[sg.wire] : 0;
        o += fmt("%d,%d,%.4f,%.4f,%.4f,%.4f,%.1f\n", sg.wire + 1, n, c.x, c.y, c.z, 1000 * std::abs(I), std::arg(I) * 180 / kPi);
    }
    return o;
}

} // namespace dect2::antd
