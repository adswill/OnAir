// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: the thin-wire method of moments (mesh, impedance matrix, excitation, currents, far field, near field). Clean-room from
// the textbook mixed-potential EFIE (Harrington, "Field Computation by Moment Methods"; Balanis, "Antenna Theory" ch. 8) - see docs/ANTENNA.md.
#include "dect2/antd/antd.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace dect2::antd {

double Vec3::norm() const { return std::sqrt(x * x + y * y + z * z); }
double wireLength(const Wire& w) { return (w.b - w.a).norm(); }

namespace {

const cd J(0, 1);

// Gauss-Legendre nodes and weights on [0, 1]
struct Rule { std::vector<double> x, w; };
Rule makeRule(int n) {
    Rule r; r.x.resize(n); r.w.resize(n);
    for (int i = 0; i < n; ++i) {
        double z = std::cos(kPi * (i + 0.75) / (n + 0.5)), pp = 0;
        for (int it = 0; it < 100; ++it) {
            double p1 = 1, p2 = 0;
            for (int j = 1; j <= n; ++j) { const double p3 = p2; p2 = p1; p1 = ((2.0 * j - 1) * z * p2 - (j - 1.0) * p3) / j; }
            pp = n * (z * p1 - p2) / (z * z - 1);
            const double z1 = z; z -= p1 / pp;
            if (std::fabs(z - z1) < 1e-15) break;
        }
        r.x[i] = 0.5 * (1 - z); r.w[i] = 1.0 / ((1 - z * z) * pp * pp);
    }
    return r;
}
const Rule& rule(int n) {
    static const Rule r2 = makeRule(2), r3 = makeRule(3), r4 = makeRule(4), r6 = makeRule(6), r8 = makeRule(8);
    switch (n) { case 2: return r2; case 3: return r3; case 4: return r4; case 6: return r6; default: return r8; }
}

Vec3 mirrorZ(const Vec3& v) { return {v.x, v.y, -v.z}; }

// the ground's image factor for the near interactions: quasi-static K = (eps_c - 1) / (eps_c + 1); 1 for a perfect ground
cd imageK(const Ground& g, double fHz) {
    if (g.type == GroundType::Perfect) return 1.0;
    if (g.type == GroundType::Free) return 0.0;
    const cd ec(g.epsr, -g.sigma / (2 * kPi * fHz * kEps0));
    return (ec - 1.0) / (ec + 1.0);
}

struct PairInt { cd s, v[2][2]; };   // v[test shape][source shape], shape 0 = rise (u/L), 1 = fall (1 - u/L)

// the integrals of G = e^{-jkR} / (4 pi R) between a test segment (pa, tp, lp) and a source segment (qa, tq, lq, radius aq)
PairInt pairIntegrals(const Vec3& pa, const Vec3& tp, double lp, const Vec3& qa, const Vec3& tq, double lq, double aq, double k) {
    PairInt r{};
    const Vec3 pc = pa + tp * (lp * 0.5), qc = qa + tq * (lq * 0.5);
    const double dc = (pc - qc).norm();
    const bool isNear = dc < 2.0 * (lp + lq) + 10 * aq;
    const double inv4pi = 1.0 / (4 * kPi);
    auto accum = [&](double v, double wv, cd g0, cd g1) {
        const double tr = v / lp, tf = 1 - tr;
        r.s += wv * g0;
        r.v[0][0] += wv * tr * g1; r.v[0][1] += wv * tr * (g0 - g1);
        r.v[1][0] += wv * tf * g1; r.v[1][1] += wv * tf * (g0 - g1);
    };
    if (!isNear) {
        const Rule& rt = rule(3); const Rule& rs = rule(dc < 6 * (lp + lq) ? 4 : 3);
        for (size_t i = 0; i < rt.x.size(); ++i) {
            const double v = rt.x[i] * lp; const Vec3 p = pa + tp * v;
            cd g0 = 0, g1 = 0;
            for (size_t j = 0; j < rs.x.size(); ++j) {
                const double u = rs.x[j] * lq; const Vec3 d = p - (qa + tq * u);
                const double R = std::sqrt(d.dot(d) + aq * aq);
                const cd g = std::exp(-J * (k * R)) * (inv4pi / R) * (rs.w[j] * lq);
                g0 += g; g1 += g * rs.x[j];
            }
            accum(v, rt.w[i] * lp, g0, g1);
        }
        return r;
    }
    // near: the static part in closed form, the smooth rest by Gauss; the test integral composite over two halves (log peaks at the ends)
    const Rule& rt = rule(6); const Rule& rs = rule(4);
    for (int half = 0; half < 2; ++half)
        for (size_t i = 0; i < rt.x.size(); ++i) {
            const double v = (half + rt.x[i]) * 0.5 * lp; const Vec3 p = pa + tp * v;
            const Vec3 d = p - qa;
            const double z0 = d.dot(tq);
            const double c2 = std::max(d.dot(d) - z0 * z0, 0.0) + aq * aq, c = std::sqrt(c2);
            const double i0 = std::asinh((lq - z0) / c) + std::asinh(z0 / c);
            const double i1 = std::sqrt((lq - z0) * (lq - z0) + c2) - std::sqrt(z0 * z0 + c2) + z0 * i0;
            cd g0 = i0 * inv4pi, g1 = i1 * inv4pi / lq;
            for (size_t j = 0; j < rs.x.size(); ++j) {
                const double u = rs.x[j] * lq; const Vec3 e = p - (qa + tq * u);
                const double R = std::sqrt(e.dot(e) + aq * aq);
                const double kr = k * R;
                // (e^{-jkR} - 1) / R without cancellation trouble at small kR
                cd f;
                if (kr < 1e-3) f = cd(-0.5 * kr * k, -k * (1 - kr * kr / 6));
                else f = (std::exp(-J * kr) - 1.0) / R;
                f *= inv4pi * rs.w[j] * lq;
                g0 += f; g1 += f * rs.x[j];
            }
            accum(v, rt.w[i] * 0.5 * lp, g0, g1);
        }
    return r;
}

// the internal impedance per metre of a round wire (skin effect, DC at low frequency)
cd wireZs(double radius, double sigma, double fHz) {
    if (sigma <= 0 || radius <= 0) return 0.0;
    const double rdc = 1.0 / (sigma * kPi * radius * radius);
    const double rs = std::sqrt(2 * kPi * fHz * kMu0 / (2 * sigma));
    const double rhf = rs / (2 * kPi * radius);
    return cd(std::sqrt(rdc * rdc + rhf * rhf), rhf);
}

struct PieceRef { int basis; int shape; int sign; };   // shape 0 rise, 1 fall

} // namespace

int autoSegments(const Wire& w, double fMaxHz, double spw, int minSeg) {
    const double len = wireLength(w);
    const double lam = kC0 / std::max(fMaxHz, 1.0);
    int n = (int)std::ceil(len / lam * std::max(spw, 1.0) - 1e-9);
    n = std::max(n, minSeg);
    return std::max(1, std::min(n, 4000));
}

double modelExtent(const Model& m) {
    if (m.wires.empty()) return 1;
    Vec3 lo = m.wires[0].a, hi = lo;
    for (const auto& w : m.wires)
        for (const Vec3& p : {w.a, w.b}) {
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }
    return std::max((hi - lo).norm(), 1e-6);
}

Mesh buildMesh(const Model& m, double fMaxHz, double segScale) {
    Mesh me;
    const double ext = modelExtent(m);
    double minLen = ext;
    for (const auto& w : m.wires) if (wireLength(w) > 0) minLen = std::min(minLen, wireLength(w));
    const double tol = std::max(1e-7, std::min(1e-4 * ext, 0.05 * minLen));
    const bool gnd = m.ground.type != GroundType::Free;
    // which wire ends touch another wire's end, or the ground: those may have a single segment
    auto connected = [&](int wi, const Vec3& p) {
        if (gnd && std::fabs(p.z) < tol) return true;
        for (int j = 0; j < (int)m.wires.size(); ++j) {
            if (j == wi) continue;
            if ((m.wires[j].a - p).norm() < tol || (m.wires[j].b - p).norm() < tol) return true;
        }
        return false;
    };
    // segments, with a node at every source / load / line position
    std::vector<int> segWireStart(m.wires.size() + 1, 0);
    for (int wi = 0; wi < (int)m.wires.size(); ++wi) {
        const Wire& w = m.wires[wi];
        segWireStart[wi] = (int)me.segs.size();
        const double len = wireLength(w);
        if (len <= 0) { me.warnings.push_back("wire " + std::to_string(wi + 1) + " has zero length (skipped)"); continue; }
        const bool both = connected(wi, w.a) && connected(wi, w.b);
        int n = w.segments > 0 ? w.segments : autoSegments(w, fMaxHz, m.segPerWavelength, both ? 1 : m.minSegments);
        n = std::max(1, (int)std::ceil(n * segScale - 1e-9));
        std::vector<double> br = {0.0, 1.0};
        auto addBr = [&](int wire, double pos) { if (wire == wi && pos > 1e-6 && pos < 1 - 1e-6) br.push_back(pos); };
        for (const auto& s : m.sources) addBr(s.wire, s.pos);
        for (const auto& l : m.loads) addBr(l.wire, l.pos);
        for (int li = 0; li < (int)m.lines.size(); ++li) {
            const TLine& t = m.lines[li];
            addBr(t.wire1, t.pos1);
            bool driven = false;
            for (const auto& s : m.sources) if (s.line == li) driven = true;
            if (!driven) addBr(t.wire2, t.pos2);
        }
        std::sort(br.begin(), br.end());
        std::vector<double> nodes;   // fractions
        for (size_t i = 0; i + 1 < br.size(); ++i) {
            const double f0 = br[i], f1 = br[i + 1];
            if (f1 - f0 < 1e-9) continue;
            const int k = std::max(1, (int)std::lround(n * (f1 - f0)));
            for (int j = 0; j < k; ++j) nodes.push_back(f0 + (f1 - f0) * j / k);
        }
        nodes.push_back(1.0);
        const Vec3 d = w.b - w.a;
        for (size_t i = 0; i + 1 < nodes.size(); ++i) {
            Segment s; s.a = w.a + d * nodes[i]; s.b = w.a + d * nodes[i + 1];
            s.radius = w.radius; s.conductivity = w.conductivity; s.wire = wi;
            me.segs.push_back(s);
        }
        const double sl = len / std::max<size_t>(1, nodes.size() - 1);
        if (w.radius > 0 && sl / w.radius < 2)
            me.warnings.push_back("wire " + std::to_string(wi + 1) + ": segments only " + std::to_string(sl / w.radius).substr(0, 4) +
                                  " radii long (thin-wire limit; use fewer segments or a thinner wire)");
        if (sl > kC0 / std::max(fMaxHz, 1.0) / 10 * 1.01)
            me.warnings.push_back("wire " + std::to_string(wi + 1) + ": segments longer than a tenth of a wavelength");
    }
    segWireStart[m.wires.size()] = (int)me.segs.size();
    // nodes: merge coinciding segment ends
    struct NodeEnd { int seg, end; };
    std::vector<Vec3> nodePos;
    std::vector<std::vector<NodeEnd>> nodeEnds;
    std::map<std::tuple<long long, long long, long long>, std::vector<int>> grid;
    auto key = [&](const Vec3& p) { return std::make_tuple((long long)std::floor(p.x / tol / 4), (long long)std::floor(p.y / tol / 4), (long long)std::floor(p.z / tol / 4)); };
    auto findNode = [&](const Vec3& p) {
        auto k0 = key(p);
        for (long long dx = -1; dx <= 1; ++dx) for (long long dy = -1; dy <= 1; ++dy) for (long long dz = -1; dz <= 1; ++dz) {
            auto it = grid.find(std::make_tuple(std::get<0>(k0) + dx, std::get<1>(k0) + dy, std::get<2>(k0) + dz));
            if (it == grid.end()) continue;
            for (int id : it->second) if ((nodePos[id] - p).norm() < tol) return id;
        }
        return -1;
    };
    for (int s = 0; s < (int)me.segs.size(); ++s)
        for (int e = 0; e < 2; ++e) {
            const Vec3 p = e ? me.segs[s].b : me.segs[s].a;
            int id = findNode(p);
            if (id < 0) { id = (int)nodePos.size(); nodePos.push_back(p); nodeEnds.emplace_back(); grid[key(p)].push_back(id); }
            nodeEnds[id].push_back({s, e});
        }
    // basis functions
    std::vector<int> nodeFirstBasis(nodePos.size(), -1), nodeBasisCount(nodePos.size(), 0);
    auto piece = [&](Basis& b, int k, const NodeEnd& ne, bool inflow) {
        b.seg[k] = ne.seg;
        b.rise[k] = ne.end == 1;   // the peak at b: rising toward b
        // inflow (toward the node): along a->b when the node is b; outflow (away): along a->b when the node is a
        b.sign[k] = inflow ? (ne.end == 1 ? 1 : -1) : (ne.end == 0 ? 1 : -1);
    };
    for (int nd = 0; nd < (int)nodePos.size(); ++nd) {
        const auto& ends = nodeEnds[nd];
        const bool onGround = gnd && std::fabs(nodePos[nd].z) < tol;
        nodeFirstBasis[nd] = (int)me.basis.size();
        if (onGround) {
            for (const auto& ne : ends) {
                Basis b; b.nPieces = 1; b.node = nodePos[nd];
                piece(b, 0, ne, false);
                me.basis.push_back(b);
            }
        } else if (ends.size() >= 2) {
            for (size_t i = 1; i < ends.size(); ++i) {
                Basis b; b.nPieces = 2; b.node = nodePos[nd];
                piece(b, 0, ends[0], true); piece(b, 1, ends[i], false);
                me.basis.push_back(b);
            }
            if (ends.size() > 2) {
                double rmin = 1e30, rmax = 0;
                for (const auto& ne : ends) { rmin = std::min(rmin, me.segs[ne.seg].radius); rmax = std::max(rmax, me.segs[ne.seg].radius); }
                if (rmax > 3 * rmin) me.warnings.push_back("a junction joins wires of very different radius (less accurate)");
            }
        }
        nodeBasisCount[nd] = (int)me.basis.size() - nodeFirstBasis[nd];
    }
    // place sources, loads and line ports on a basis
    auto place = [&](int wire, double pos, int& sign) -> int {
        sign = 1;
        if (wire < 0 || wire >= (int)m.wires.size()) return -1;
        const Wire& w = m.wires[wire];
        const Vec3 p = w.a + (w.b - w.a) * std::min(1.0, std::max(0.0, pos));
        // the nearest node on this wire
        int best = -1; double bd = 1e30;
        for (int s = segWireStart[wire]; s < segWireStart[wire + 1]; ++s)
            for (int e = 0; e < 2; ++e) {
                const double d = ((e ? me.segs[s].b : me.segs[s].a) - p).norm();
                if (d < bd) { bd = d; best = findNode(e ? me.segs[s].b : me.segs[s].a); }
            }
        if (best < 0) return -1;
        int pick = -1, pickK = 0;
        for (int bi = nodeFirstBasis[best]; bi < nodeFirstBasis[best] + nodeBasisCount[best]; ++bi) {
            const Basis& b = me.basis[bi];
            for (int k = 0; k < b.nPieces; ++k)
                if (me.segs[b.seg[k]].wire == wire && (pick < 0 || k == 1)) { pick = bi; pickK = k; }
        }
        if (pick < 0) return -1;
        sign = me.basis[pick].sign[pickK];
        return pick;
    };
    for (const auto& s : m.sources) {
        if (s.line >= 0) { me.srcBasis.push_back(-1); me.srcSign.push_back(1); continue; }   // drives a line end
        int sg; const int b = place(s.wire, s.pos, sg);
        me.srcBasis.push_back(b); me.srcSign.push_back(sg);
        if (b < 0) me.warnings.push_back("a source is not on a wire point that carries current (free end?)");
    }
    for (const auto& l : m.loads) {
        int sg; const int b = place(l.wire, l.pos, sg);
        me.loadBasis.push_back(b);
        if (b < 0) me.warnings.push_back("a load is not on a wire point that carries current");
    }
    for (int i = 0; i < (int)m.lines.size(); ++i) {
        const TLine& t = m.lines[i];
        bool driven = false;
        for (const auto& s : m.sources) if (s.line == i) driven = true;
        int s1 = 1, s2 = 1;
        const int b1 = place(t.wire1, t.pos1, s1);
        const int b2 = driven ? -2 : place(t.wire2, t.pos2, s2);   // -2: the source terminal
        me.tlBasis1.push_back(b1); me.tlBasis2.push_back(b2); me.tlSign1.push_back(s1); me.tlSign2.push_back(s2);
        if (b1 < 0 || b2 == -1 || b1 == b2) me.warnings.push_back("a transmission line end is not on a usable wire point");
    }
    return me;
}

cd loadImpedance(const Load& l, double fHz) {
    const double w = 2 * kPi * fHz;
    switch (l.type) {
    case LoadType::RX: return cd(l.r, l.x);
    case LoadType::SeriesRLC: {
        cd z(l.r, w * l.l);
        if (l.c > 0) z += cd(0, -1.0 / (w * l.c));
        return z;
    }
    case LoadType::ParallelRLC: {
        cd y = 0;
        if (l.r > 0) y += 1.0 / l.r;
        if (l.l > 0) y += cd(0, -1.0 / (w * l.l));
        if (l.c > 0) y += cd(0, w * l.c);
        if (std::abs(y) < 1e-15) return cd(1e12, 0);   // nothing: an open circuit
        return 1.0 / y;
    }
    }
    return 0.0;
}

cd reflection(cd z, double z0) { return (z - z0) / (z + z0); }
double vswr(cd z, double z0) {
    const double g = std::abs(reflection(z, z0));
    return g >= 0.999999 ? 1e6 : (1 + g) / (1 - g);
}

Solution solveMesh(const Model& m, const Mesh& mesh, double fHz, int threads) {
    Solution sol;
    sol.fHz = fHz;
    sol.mesh = mesh;
    const int nb = (int)mesh.basis.size(), ns = (int)mesh.segs.size();
    if (nb == 0) { sol.error = "no current can flow: no wire has a connected point (check the feed and the wire ends)"; return sol; }
    if (fHz <= 0) { sol.error = "the frequency must be above 0"; return sol; }
    const double w = 2 * kPi * fHz, k = w / kC0;
    const cd jwmu = J * (w * kMu0), sc = 1.0 / (J * (w * kEps0));
    const cd K = imageK(m.ground, fHz);
    const bool img = m.ground.type != GroundType::Free;
    std::vector<std::vector<PieceRef>> pieces(ns);
    for (int b = 0; b < nb; ++b)
        for (int q = 0; q < mesh.basis[b].nPieces; ++q)
            pieces[mesh.basis[b].seg[q]].push_back({b, mesh.basis[b].rise[q] ? 0 : 1, mesh.basis[b].sign[q]});
    std::vector<Vec3> t(ns); std::vector<double> len(ns);
    for (int s = 0; s < ns; ++s) { const Vec3 d = mesh.segs[s].b - mesh.segs[s].a; len[s] = d.norm(); t[s] = d * (1.0 / std::max(len[s], 1e-30)); }
    std::vector<cd> Z((size_t)nb * nb, 0.0);
    std::vector<std::mutex> rowLock(std::min(nb, 4096));
    if (threads <= 0) threads = (int)std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    std::atomic<int> next{0};
    auto work = [&] {
        std::vector<PairInt> row(ns), rowImg(img ? ns : 0);
        for (;;) {
            const int p = next.fetch_add(1);
            if (p >= ns) break;
            if (pieces[p].empty()) continue;
            for (int q = 0; q < ns; ++q) {
                if (pieces[q].empty()) continue;
                row[q] = pairIntegrals(mesh.segs[p].a, t[p], len[p], mesh.segs[q].a, t[q], len[q], mesh.segs[q].radius, k);
                if (img) {
                    const Vec3 ia = mirrorZ(mesh.segs[q].a), it = mirrorZ(t[q]);
                    rowImg[q] = pairIntegrals(mesh.segs[p].a, t[p], len[p], ia, it, len[q], mesh.segs[q].radius, k);
                }
            }
            for (const PieceRef& pm : pieces[p]) {
                const double dfm = pm.sign * (pm.shape == 0 ? 1.0 : -1.0) / len[p];
                std::lock_guard<std::mutex> lk(rowLock[pm.basis % rowLock.size()]);
                cd* zr = Z.data() + (size_t)pm.basis * nb;
                for (int q = 0; q < ns; ++q) {
                    if (pieces[q].empty()) continue;
                    const PairInt& pi = row[q];
                    const double tt = t[p].dot(t[q]);
                    cd timg = 0;
                    if (img) {   // the image current: (-K tx, -K ty, +K tz) on the mirrored segment
                        timg = -K * (t[p].x * t[q].x + t[p].y * t[q].y) + K * (t[p].z * t[q].z);
                    }
                    for (const PieceRef& pn : pieces[q]) {
                        const double dfn = pn.sign * (pn.shape == 0 ? 1.0 : -1.0) / len[q];
                        cd z = jwmu * (double)(pm.sign * pn.sign) * tt * pi.v[pm.shape][pn.shape] + sc * (dfm * dfn) * pi.s;
                        if (img) {
                            const PairInt& pj = rowImg[q];
                            z += jwmu * (double)(pm.sign * pn.sign) * timg * pj.v[pm.shape][pn.shape] - sc * (dfm * dfn) * K * pj.s;
                        }
                        zr[pn.basis] += z;
                    }
                }
            }
        }
    };
    {
        const int T = ns > 60 ? threads : 1;
        std::vector<std::thread> th;
        for (int i = 1; i < T; ++i) th.emplace_back(work);
        work();
        for (auto& x : th) x.join();
    }
    // lossy wires: + integral of Zs f_m f_n
    std::vector<cd> zs(ns, 0.0);
    for (int s = 0; s < ns; ++s) {
        zs[s] = wireZs(mesh.segs[s].radius, mesh.segs[s].conductivity, fHz);
        if (zs[s] == 0.0) continue;
        for (const PieceRef& a : pieces[s])
            for (const PieceRef& b : pieces[s])
                Z[(size_t)a.basis * nb + b.basis] += zs[s] * len[s] * (double)(a.sign * b.sign) * (a.shape == b.shape ? 1.0 / 3 : 1.0 / 6);
    }
    // lumped loads and transmission lines
    for (size_t i = 0; i < m.loads.size(); ++i) {
        const int b = i < mesh.loadBasis.size() ? mesh.loadBasis[i] : -1;
        if (b >= 0) Z[(size_t)b * nb + b] += loadImpedance(m.loads[i], fHz);
    }
    // excitation: sources in the wire gaps
    std::vector<cd> v(nb, 0.0);
    bool anySrc = false;
    std::vector<int> srcAt(nb, -1);
    for (size_t i = 0; i < m.sources.size(); ++i) {
        if (m.sources[i].line >= 0) { anySrc = true; continue; }
        const int b = i < mesh.srcBasis.size() ? mesh.srcBasis[i] : -1;
        if (b < 0) continue;
        v[b] += m.sources[i].volts * (double)mesh.srcSign[i];
        if (srcAt[b] < 0) srcAt[b] = (int)i;
        anySrc = true;
    }
    if (!anySrc) { sol.error = "no source: put a feed point on a wire"; return sol; }
    // transmission lines: every line end is a port across a wire gap (or a source terminal); the lines at one gap are in parallel.
    // Y (ports) = the sum of the lines' admittance matrices. Ports without a source (F) become Z += Yff^-1 between their bases,
    // ports with a source (S) have their voltage fixed: v_F -= Yff^-1 Yfs Vs.
    struct Port { int basis; int src; cd vs; };
    std::vector<Port> ports;
    auto portOf = [&](int basis, int src) {
        for (size_t i = 0; i < ports.size(); ++i) if (basis >= 0 ? ports[i].basis == basis : (ports[i].basis < 0 && ports[i].src == src)) return (int)i;
        ports.push_back({basis, src, 0.0});
        return (int)ports.size() - 1;
    };
    struct Y2 { int p1, p2; cd y11, y12; };
    std::vector<Y2> ylines;
    for (size_t i = 0; i < m.lines.size() && i < mesh.tlBasis1.size(); ++i) {
        const int b1 = mesh.tlBasis1[i], b2 = mesh.tlBasis2[i];
        if (b1 < 0 || b2 == -1 || b1 == b2) continue;
        int drv = -1;
        for (size_t k2 = 0; k2 < m.sources.size(); ++k2) if (m.sources[k2].line == (int)i) { drv = (int)k2; break; }
        const TLine& tl = m.lines[i];
        double l = tl.length;
        if (l <= 0) l = b2 >= 0 ? (mesh.basis[b1].node - mesh.basis[b2].node).norm() : 0;
        l = std::max(l, 1e-6);
        const double beta = w / (kC0 * std::max(tl.vf, 0.01));
        const double alpha = std::max(tl.lossDbPerM / 8.685889638, 1e-7 / l);
        const cd gl = cd(alpha, beta) * l;
        const cd z11 = tl.z0 / std::tanh(gl), z12 = tl.z0 / std::sinh(gl);
        const double s12 = (tl.crossed ? -1.0 : 1.0) * mesh.tlSign1[i] * (b2 >= 0 ? mesh.tlSign2[i] : 1);
        const cd det = z11 * z11 - z12 * z12;
        const int p1 = portOf(b1, -1), p2 = b2 >= 0 ? portOf(b2, -1) : portOf(-1, drv);
        ylines.push_back({p1, p2, z11 / det, -s12 * z12 / det});
    }
    const int np = (int)ports.size();
    std::vector<cd> Y((size_t)np * np, 0.0);
    for (const Y2& y : ylines) {
        Y[(size_t)y.p1 * np + y.p1] += y.y11; Y[(size_t)y.p2 * np + y.p2] += y.y11;
        Y[(size_t)y.p1 * np + y.p2] += y.y12; Y[(size_t)y.p2 * np + y.p1] += y.y12;
    }
    std::vector<int> F, S;
    for (int i = 0; i < np; ++i) {
        Port& pt = ports[i];
        if (pt.basis >= 0 && srcAt[pt.basis] >= 0) { pt.src = srcAt[pt.basis]; pt.vs = m.sources[pt.src].volts * (double)mesh.srcSign[pt.src]; }
        else if (pt.basis < 0) pt.vs = m.sources[pt.src].volts;
        if (pt.basis >= 0 && pt.src < 0) F.push_back(i); else S.push_back(i);
    }
    std::vector<cd> zff;      // Yff^-1, |F| x |F|
    std::vector<cd> yfsVs(F.size(), 0.0);
    if (!F.empty()) {
        const int nf = (int)F.size();
        std::vector<cd> yff((size_t)nf * nf);
        for (int i = 0; i < nf; ++i) for (int j = 0; j < nf; ++j) yff[(size_t)i * nf + j] = Y[(size_t)F[i] * np + F[j]];
        std::vector<int> pv;
        if (!luFactor(yff, nf, pv, 1)) { sol.error = "the transmission lines form a singular network"; return sol; }
        zff.assign((size_t)nf * nf, 0.0);
        for (int j = 0; j < nf; ++j) {
            std::vector<cd> e(nf, 0.0); e[j] = 1.0;
            luSolve(yff, nf, pv, e);
            for (int i = 0; i < nf; ++i) zff[(size_t)i * nf + j] = e[i];
        }
        for (int i = 0; i < nf; ++i) for (int s2 : S) yfsVs[i] += Y[(size_t)F[i] * np + s2] * ports[s2].vs;
        for (int i = 0; i < nf; ++i) {
            cd add = 0;
            for (int j = 0; j < nf; ++j) {
                Z[(size_t)ports[F[i]].basis * nb + ports[F[j]].basis] += zff[(size_t)i * nf + j];
                add += zff[(size_t)i * nf + j] * yfsVs[j];
            }
            v[ports[F[i]].basis] -= add;
        }
    }
    std::vector<int> piv;
    if (!luFactor(Z, nb, piv, threads)) { sol.error = "the matrix is singular (overlapping wires or a zero-length segment?)"; return sol; }
    luSolve(Z, nb, piv, v);
    sol.coef = v;
    sol.segI.assign(ns, SegCurrent{0.0, 0.0});
    for (int b = 0; b < nb; ++b) {
        const Basis& bs = mesh.basis[b];
        for (int q = 0; q < bs.nPieces; ++q) {
            const cd c = v[b] * (double)bs.sign[q];
            if (bs.rise[q]) sol.segI[bs.seg[q]].ib += c; else sol.segI[bs.seg[q]].ia += c;
        }
    }
    // the port voltages and the currents into the line network
    std::vector<cd> vp(np, 0.0), inet(np, 0.0);
    for (int s2 : S) vp[s2] = ports[s2].vs;
    if (!F.empty()) {
        const int nf = (int)F.size();
        for (int i = 0; i < nf; ++i) {
            cd acc = 0;
            for (int j = 0; j < nf; ++j) acc += zff[(size_t)i * nf + j] * (v[ports[F[j]].basis] + yfsVs[j]);
            vp[F[i]] = -acc;
        }
    }
    for (int i = 0; i < np; ++i) for (int j = 0; j < np; ++j) inet[i] += Y[(size_t)i * np + j] * vp[j];
    sol.pIn = 0;
    for (size_t i = 0; i < m.sources.size(); ++i) {
        const int b = i < mesh.srcBasis.size() ? mesh.srcBasis[i] : -1;
        cd I = 0; bool have = false;
        for (int s2 : S) if (ports[s2].src == (int)i) {
            I = ports[s2].basis >= 0 ? (v[ports[s2].basis] + inet[s2]) * (double)mesh.srcSign[i] : inet[s2];
            have = true;
        }
        if (!have && b >= 0) { I = v[b] * (double)mesh.srcSign[i]; have = true; }
        if (!have) { sol.srcZ.push_back(cd(NAN, NAN)); sol.srcI.push_back(0.0); continue; }
        sol.srcI.push_back(I);
        sol.srcZ.push_back(std::abs(I) > 0 ? m.sources[i].volts / I : cd(1e12, 0));
        sol.pIn += 0.5 * (m.sources[i].volts * std::conj(I)).real();
    }
    // (several sources on one basis share the current; their impedance is then the total)
    for (size_t i = 0; i < m.sources.size(); ++i) if (std::isfinite(sol.srcZ[i].real())) { sol.zin = sol.srcZ[i]; break; }
    sol.pLoss = 0;
    for (size_t i = 0; i < m.loads.size(); ++i) {
        const int b = i < mesh.loadBasis.size() ? mesh.loadBasis[i] : -1;
        if (b >= 0) sol.pLoss += 0.5 * std::norm(v[b]) * loadImpedance(m.loads[i], fHz).real();
    }
    for (int s = 0; s < ns; ++s) {
        if (zs[s] == 0.0) continue;
        const cd ia = sol.segI[s].ia, ib = sol.segI[s].ib;
        sol.pLoss += 0.5 * zs[s].real() * len[s] * (std::norm(ia) + std::norm(ib) + (ia * std::conj(ib)).real()) / 3;
    }
    sol.ok = true;
    return sol;
}

Solution solve(const Model& m, double fHz, const SolveOptions& o) {
    const Mesh mesh = buildMesh(m, fHz, o.segScale);
    return solveMesh(m, mesh, fHz, o.threads);
}

// ---- far field -----------------------------------------------------------------------------------------------------------------------------
namespace {
// the radiation integral of one segment's linear current, direction rhat: returns the vector sum of I(u) e^{jk rhat . r(u)} du t
void segRad(const Vec3& a, const Vec3& tt, double l, cd ia, cd ib, const Vec3& rh, double k, cd out[3], double fx, double fy, double fz) {
    const Rule& r = rule(l * k > 1.5 ? 8 : 4);
    cd s = 0;
    for (size_t i = 0; i < r.x.size(); ++i) {
        const double u = r.x[i] * l;
        const Vec3 p = a + tt * u;
        const cd I = ia * (1 - r.x[i]) + ib * r.x[i];
        s += I * std::exp(J * (k * rh.dot(p))) * (r.w[i] * l);
    }
    out[0] += s * (tt.x * fx); out[1] += s * (tt.y * fy); out[2] += s * (tt.z * fz);
}
} // namespace

FarField farField(const Model& m, const Solution& s, double thDeg, double phDeg) {
    FarField f{0.0, 0.0};
    if (!s.ok) return f;
    const bool gnd = m.ground.type != GroundType::Free;
    if (gnd && thDeg > 90.0 + 1e-9) return f;
    const double th = thDeg * kPi / 180, ph = phDeg * kPi / 180;
    const double st = std::sin(th), ct = std::cos(th), sp = std::sin(ph), cp = std::cos(ph);
    const Vec3 rh(st * cp, st * sp, ct), thh(ct * cp, ct * sp, -st), phh(-sp, cp, 0);
    const double w = 2 * kPi * s.fHz, k = w / kC0;
    cd dir[3] = {0.0, 0.0, 0.0}, im[3] = {0.0, 0.0, 0.0};
    const Mesh& me = s.mesh;
    for (size_t i = 0; i < me.segs.size(); ++i) {
        const Vec3 d = me.segs[i].b - me.segs[i].a; const double l = d.norm();
        if (l <= 0) continue;
        const Vec3 tt = d * (1 / l);
        segRad(me.segs[i].a, tt, l, s.segI[i].ia, s.segI[i].ib, rh, k, dir, 1, 1, 1);
        if (gnd) segRad(mirrorZ(me.segs[i].a), mirrorZ(tt), l, s.segI[i].ia, s.segI[i].ib, rh, k, im, -1, -1, -1);   // the PEC image current: -I along the mirrored segment
    }
    const cd c = -J * (w * kMu0 / (4 * kPi));
    auto dotc = [](const cd v[3], const Vec3& u) { return v[0] * u.x + v[1] * u.y + v[2] * u.z; };
    f.eth = c * dotc(dir, thh); f.eph = c * dotc(dir, phh);
    if (gnd) {
        cd it = c * dotc(im, thh), ip = c * dotc(im, phh);
        if (m.ground.type == GroundType::Real) {
            const cd ec(m.ground.epsr, -m.ground.sigma / (w * kEps0));
            const double sps = ct, cps2 = st * st;   // elevation psi: sin psi = cos theta
            const cd root = std::sqrt(ec - cps2);
            const cd rv = (ec * sps - root) / (ec * sps + root), rhc = (sps - root) / (sps + root);
            it *= rv; ip *= -rhc;
        }
        f.eth += it; f.eph += ip;
    }
    return f;
}

Gain gainAt(const Model& m, const Solution& s, double thDeg, double phDeg) {
    Gain g;
    if (!s.ok || s.pIn <= 0) return g;
    const FarField f = farField(m, s, thDeg, phDeg);
    const double c = 4 * kPi / (2 * kEta0 * s.pIn);
    auto db = [](double x) { return x > 1e-30 ? 10 * std::log10(x) : -999.0; };
    g.vert = db(c * std::norm(f.eth)); g.horiz = db(c * std::norm(f.eph)); g.total = db(c * (std::norm(f.eth) + std::norm(f.eph)));
    return g;
}

Pattern pattern(const Model& m, const Solution& s, double thStep, double phStep) {
    Pattern p;
    p.thStep = std::max(0.25, thStep); p.phStep = std::max(0.25, phStep);
    const bool gnd = m.ground.type != GroundType::Free;
    const double thMax = gnd ? 90 : 180;
    p.nTh = (int)std::lround(thMax / p.thStep) + 1; p.thStep = thMax / (p.nTh - 1);
    p.nPh = (int)std::lround(360 / p.phStep) + 1; p.phStep = 360.0 / (p.nPh - 1);
    p.gdbi.assign((size_t)p.nTh * p.nPh, -999.f); p.gv = p.gdbi; p.gh = p.gdbi;
    if (!s.ok || s.pIn <= 0) return p;
    const double c = 4 * kPi / (2 * kEta0 * s.pIn);
    std::vector<double> u((size_t)p.nTh * p.nPh, 0);
    auto rowJob = [&](int i0, int i1) {
        for (int i = i0; i < i1; ++i)
            for (int j = 0; j < p.nPh; ++j) {
                const FarField f = farField(m, s, p.thAt(i), p.phAt(j));
                const double gv = c * std::norm(f.eth), gh = c * std::norm(f.eph);
                const size_t ix = (size_t)i * p.nPh + j;
                u[ix] = gv + gh;
                p.gdbi[ix] = (float)(gv + gh > 1e-30 ? 10 * std::log10(gv + gh) : -999);
                p.gv[ix] = (float)(gv > 1e-30 ? 10 * std::log10(gv) : -999);
                p.gh[ix] = (float)(gh > 1e-30 ? 10 * std::log10(gh) : -999);
            }
    };
    const int T = (int)std::min<unsigned>(8, std::max(1u, std::thread::hardware_concurrency()));
    if (s.mesh.segs.size() * p.nTh * p.nPh > 200000 && T > 1) {
        std::vector<std::thread> th;
        for (int q = 0; q < T; ++q) th.emplace_back(rowJob, p.nTh * q / T, p.nTh * (q + 1) / T);
        for (auto& x : th) x.join();
    } else rowJob(0, p.nTh);
    // power: the gain (relative to pIn) integrated over the sphere / hemisphere: sum G sin(theta) dtheta dphi / (4 pi)
    double acc = 0;
    const double dth = p.thStep * kPi / 180, dph = p.phStep * kPi / 180;
    for (int i = 0; i < p.nTh; ++i) {
        const double wt = (i == 0 || i == p.nTh - 1) ? 0.5 : 1.0;
        const double st = std::sin(p.thAt(i) * kPi / 180);
        for (int j = 0; j + 1 < p.nPh; ++j) acc += wt * u[(size_t)i * p.nPh + j] * st * dth * dph;
        for (int j = 0; j < p.nPh; ++j) {
            const float g = p.at(i, j);
            if (g > p.maxDbi) { p.maxDbi = g; p.maxTheta = p.thAt(i); p.maxPhi = p.phAt(j); }
        }
    }
    const double frac = acc / (4 * kPi);   // pRad / pIn
    p.pRad = frac * s.pIn;
    p.efficiency = std::min(1.0, std::max(0.0, frac));
    p.directivityDbi = frac > 0 ? p.maxDbi - 10 * std::log10(frac) : -999;
    return p;
}

std::vector<double> azimuthCut(const Model& m, const Solution& s, double elDeg, double step) {
    std::vector<double> g;
    const int n = (int)std::lround(360 / step) + 1;
    for (int i = 0; i < n; ++i) g.push_back(gainAt(m, s, 90 - elDeg, i * step).total);
    return g;
}

std::vector<double> elevationCut(const Model& m, const Solution& s, double phiDeg, double step) {
    // the vertical plane through phi: elevation -90..90 in front... as a full circle: alpha 0..360, alpha = elevation toward phi,
    // 90 = zenith, 180 = the horizon behind (phi + 180); over ground alpha 0..180 only
    std::vector<double> g;
    const bool gnd = m.ground.type != GroundType::Free;
    const double amax = gnd ? 180 : 360;
    const int n = (int)std::lround(amax / step) + 1;
    for (int i = 0; i < n; ++i) {
        const double a = i * step;
        double th, ph;
        if (a <= 90) { th = 90 - a; ph = phiDeg; }
        else if (a <= 270) { th = a - 90; ph = phiDeg + 180; }
        else { th = 450 - a; ph = phiDeg; }
        if (th > 180) th = 180;
        g.push_back(gainAt(m, s, th, std::fmod(ph, 360.0)).total);
    }
    return g;
}

namespace {
// the -3 dB width around index i0 of a cut (circular when wrap)
double width3(const std::vector<double>& g, int i0, double step, bool wrap) {
    const int n = (int)g.size() - (wrap ? 1 : 0);
    if (n <= 2) return 0;
    const double ref = g[i0] - 3;
    auto at = [&](int i) { if (wrap) { i %= n; if (i < 0) i += n; return g[i]; } return g[std::max(0, std::min((int)g.size() - 1, i))]; };
    double lo = 0, hi = 0; bool flo = false, fhi = false;
    for (int d = 1; d < n; ++d) {
        if (!wrap && i0 + d >= (int)g.size()) break;
        if (at(i0 + d) < ref) { const double a = at(i0 + d - 1), b = at(i0 + d); hi = (d - 1 + (a - ref) / std::max(a - b, 1e-12)) * step; fhi = true; break; }
    }
    for (int d = 1; d < n; ++d) {
        if (!wrap && i0 - d < 0) break;
        if (at(i0 - d) < ref) { const double a = at(i0 - d + 1), b = at(i0 - d); lo = (d - 1 + (a - ref) / std::max(a - b, 1e-12)) * step; flo = true; break; }
    }
    if (!flo || !fhi) return wrap ? 360 : 180;
    return lo + hi;
}
} // namespace

Figures figures(const Model& m, const Solution& s, const Pattern& p) {
    Figures f;
    if (!s.ok) return f;
    const bool gnd = m.ground.type != GroundType::Free;
    // refine the maximum with one-degree cuts around the coarse one
    double bestG = -999, bestTh = p.maxTheta, bestPh = p.maxPhi;
    for (double dt = -p.thStep; dt <= p.thStep + 1e-9; dt += 1)
        for (double dp = -p.phStep; dp <= p.phStep + 1e-9; dp += 1) {
            const double th = std::min(gnd ? 90.0 : 180.0, std::max(0.0, p.maxTheta + dt));
            const double g = gainAt(m, s, th, p.maxPhi + dp).total;
            if (g > bestG) { bestG = g; bestTh = th; bestPh = std::fmod(p.maxPhi + dp + 360, 360); }
        }
    f.maxDbi = bestG; f.maxDbd = bestG - 2.15;
    f.azMaxDeg = bestPh; f.elMaxDeg = 90 - bestTh;
    f.takeoffDeg = gnd ? f.elMaxDeg : 0;
    f.efficiency = p.efficiency;
    const double back = gainAt(m, s, bestTh, bestPh + 180).total, side = gainAt(m, s, bestTh, bestPh + 90).total;
    f.fbDb = bestG - back; f.fsDb = bestG - side;
    const std::vector<double> az = azimuthCut(m, s, f.elMaxDeg, 1);
    f.bwAzDeg = width3(az, (int)std::lround(bestPh) % 360, 1, true);
    const std::vector<double> el = elevationCut(m, s, bestPh, 1);
    f.bwElDeg = width3(el, (int)std::lround(std::max(0.0, f.elMaxDeg)), 1, !gnd);
    if (!gnd && f.bwElDeg > 180) f.bwElDeg = 360;
    return f;
}

std::vector<SweepPoint> sweep(const Model& m, double f0, double f1, int n, double z0, bool gainDir, int threads, const std::function<bool()>& cancel) {
    n = std::max(1, n);
    std::vector<SweepPoint> out(n);
    const double fmax = std::max(f0, f1);
    const Mesh mesh = buildMesh(m, fmax);   // one mesh for the whole sweep (dense enough at the top)
    if (threads <= 0) threads = (int)std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    const int T = std::min(threads, n);
    std::atomic<int> next{0};
    auto job = [&] {
        for (;;) {
            if (cancel && cancel()) return;
            const int i = next.fetch_add(1);
            if (i >= n) return;
            SweepPoint& sp = out[i];
            sp.fHz = n == 1 ? f0 : f0 + (f1 - f0) * i / (n - 1);
            const Solution s = solveMesh(m, mesh, sp.fHz, T > 1 ? 1 : threads);
            if (!s.ok) continue;
            sp.ok = true; sp.z = s.zin; sp.swr = vswr(s.zin, z0);
            if (gainDir) {
                const Pattern p = pattern(m, s, 10, 10);
                sp.gainDbi = p.maxDbi;
                sp.fbDb = p.maxDbi - gainAt(m, s, p.maxTheta, p.maxPhi + 180).total;
            }
        }
    };
    std::vector<std::thread> th;
    for (int i = 1; i < T; ++i) th.emplace_back(job);
    job();
    for (auto& x : th) x.join();
    return out;
}

Convergence convergence(const Model& m, double fHz) {
    Convergence c;
    SolveOptions o; o.segScale = 1;
    const Solution a = solve(m, fHz, o);
    o.segScale = 2;
    const Solution b = solve(m, fHz, o);
    c.n1 = (int)a.mesh.segs.size(); c.n2 = (int)b.mesh.segs.size();
    if (!a.ok || !b.ok) return c;
    c.z1 = a.zin; c.z2 = b.zin;
    c.changePct = 100 * std::abs(c.z2 - c.z1) / std::max(std::abs(c.z2), 1e-9);
    c.converged = c.changePct < 2;
    return c;
}

NearE nearField(const Model& m, const Solution& s, const Vec3& p) {
    NearE e{0.0, 0.0, 0.0};
    if (!s.ok) return e;
    const double w = 2 * kPi * s.fHz, k = w / kC0;
    const cd K = imageK(m.ground, s.fHz);
    const bool img = m.ground.type != GroundType::Free;
    const Rule& r = rule(8);
    // A (vector potential / mu) and phi (scalar potential * eps) at q
    auto pot = [&](const Vec3& q, cd A[3], cd& phi) {
        A[0] = A[1] = A[2] = 0.0; phi = 0.0;
        for (size_t i = 0; i < s.mesh.segs.size(); ++i) {
            const Segment& sg = s.mesh.segs[i];
            const Vec3 d = sg.b - sg.a; const double l = d.norm();
            if (l <= 0) continue;
            const Vec3 t = d * (1 / l);
            const cd ia = s.segI[i].ia, ib = s.segI[i].ib;
            const cd qline = -(ib - ia) / l / (J * w);   // charge per metre
            for (int im = 0; im < (img ? 2 : 1); ++im) {
                const Vec3 a0 = im ? mirrorZ(sg.a) : sg.a, tt = im ? mirrorZ(t) : t;
                cd gI = 0, g0 = 0;
                for (size_t j = 0; j < r.x.size(); ++j) {
                    const Vec3 x = a0 + tt * (r.x[j] * l);
                    const Vec3 dd = q - x;
                    const double R = std::sqrt(dd.dot(dd) + sg.radius * sg.radius);
                    const cd g = std::exp(-J * (k * R)) / (4 * kPi * R) * (r.w[j] * l);
                    gI += g * (ia * (1 - r.x[j]) + ib * r.x[j]); g0 += g;
                }
                if (!im) { A[0] += gI * t.x; A[1] += gI * t.y; A[2] += gI * t.z; phi += qline * g0; }
                else { A[0] += -K * gI * t.x; A[1] += -K * gI * t.y; A[2] += K * gI * t.z; phi += -K * qline * g0; }
            }
        }
    };
    cd A[3], phi0;
    pot(p, A, phi0);
    const double h = std::max(1e-4 * kC0 / s.fHz, 1e-6);
    cd grad[3];
    for (int ax = 0; ax < 3; ++ax) {
        Vec3 dp(ax == 0 ? h : 0, ax == 1 ? h : 0, ax == 2 ? h : 0);
        cd Ad[3], p1, p2;
        pot(p + dp, Ad, p1); pot(p - dp, Ad, p2);
        grad[ax] = (p1 - p2) / (2 * h) / kEps0;
    }
    e.ex = -J * w * kMu0 * A[0] - grad[0];
    e.ey = -J * w * kMu0 * A[1] - grad[1];
    e.ez = -J * w * kMu0 * A[2] - grad[2];
    return e;
}

// ---- model helpers -------------------------------------------------------------------------------------------------------------------------
void translate(Model& m, const Vec3& d) { for (auto& w : m.wires) { w.a = w.a + d; w.b = w.b + d; } }

void mirror(Model& m, int axis, bool keep) {
    auto refl = [axis](Vec3 v) { if (axis == 0) v.x = -v.x; else if (axis == 1) v.y = -v.y; else v.z = -v.z; return v; };
    const size_t n = m.wires.size();
    for (size_t i = 0; i < n; ++i) {
        Wire w = m.wires[i]; w.a = refl(w.a); w.b = refl(w.b);
        if (keep) { w.tag = 0; m.wires.push_back(w); } else m.wires[i] = w;
    }
}

void arrayCopy(Model& m, const std::vector<int>& wires, int copies, const Vec3& step) {
    for (int c = 1; c <= copies; ++c)
        for (int wi : wires) {
            if (wi < 0 || wi >= (int)m.wires.size()) continue;
            Wire w = m.wires[wi]; w.a = w.a + step * c; w.b = w.b + step * c; w.tag = 0;
            m.wires.push_back(w);
        }
}

void rotateZ(Model& m, double deg) {
    const double c = std::cos(deg * kPi / 180), s = std::sin(deg * kPi / 180);
    auto rot = [&](Vec3 v) { return Vec3(c * v.x - s * v.y, s * v.x + c * v.y, v.z); };
    for (auto& w : m.wires) { w.a = rot(w.a); w.b = rot(w.b); }
}

std::vector<std::string> checkModel(const Model& m, double fHz) {
    std::vector<std::string> out;
    if (m.wires.empty()) { out.push_back("no wires"); return out; }
    if (m.sources.empty()) out.push_back("no feed point: add a source");
    const double lam = kC0 / std::max(fHz, 1.0);
    for (size_t i = 0; i < m.wires.size(); ++i) {
        const Wire& w = m.wires[i];
        const std::string id = "wire " + std::to_string(i + 1);
        if (wireLength(w) <= 0) out.push_back(id + ": zero length");
        if (w.radius <= 0) out.push_back(id + ": the radius must be above 0");
        else if (w.radius > lam / 30) out.push_back(id + ": very thick for the thin-wire model (radius above wavelength / 30)");
        if (m.ground.type != GroundType::Free && (w.a.z < -1e-9 || w.b.z < -1e-9)) out.push_back(id + ": below the ground (buried wires are not modelled)");
    }
    for (const auto& s : m.sources) if (s.wire < 0 || s.wire >= (int)m.wires.size()) out.push_back("a source is on a missing wire");
    for (const auto& l : m.loads) if (l.wire < 0 || l.wire >= (int)m.wires.size()) out.push_back("a load is on a missing wire");
    // parallel wires closer than a few radii (accuracy) - only the cheap check of wire midpoints
    for (size_t i = 0; i < m.wires.size(); ++i)
        for (size_t j = i + 1; j < m.wires.size(); ++j) {
            const Vec3 ci = (m.wires[i].a + m.wires[i].b) * 0.5, cj = (m.wires[j].a + m.wires[j].b) * 0.5;
            const double d = (ci - cj).norm(), rr = m.wires[i].radius + m.wires[j].radius;
            if (d > 0 && d < 2 * rr) out.push_back("wires " + std::to_string(i + 1) + " and " + std::to_string(j + 1) + " overlap (closer than their radii)");
        }
    const Mesh me = buildMesh(m, fHz);
    for (const auto& w : me.warnings) out.push_back(w);
    return out;
}

} // namespace dect2::antd
