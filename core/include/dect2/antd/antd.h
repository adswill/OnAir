// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: a clean-room thin-wire method-of-moments solver (pure functions, no I/O, no global state). docs/ANTENNA.md.
//  - the electric-field integral equation in mixed-potential form, Galerkin with triangle (rooftop) basis and testing functions on straight
//    segments, the reduced thin-wire kernel R = sqrt(|r - r'|^2 + a^2); the static 1/R part is integrated in closed form, the rest by Gauss
//  - junctions of any number of wires (Kirchhoff by one basis per extra wire), wire ends on the ground plane (half basis into the image)
//  - delta-gap voltage sources (several, complex), lumped loads (R + jX, series or parallel RLC), lossy wires (skin effect),
//    two-port transmission lines between two feed points (optionally crossed), as NEC's TL card
//  - ground: free space, perfect ground (images), real ground: quasi-static complex images for the currents (as MININEC-class programs)
//    and the Fresnel reflection coefficients for the far field (as NEC's reflection-coefficient approximation)
//  - outputs: input impedance, SWR, currents, far-field gain (dBi, total, vertical and horizontal), efficiency, pattern figures
// Coordinates: metres, z up. Pattern angles: theta from the zenith, phi from +x counter-clockwise (NEC convention); elevation = 90 - theta.
#pragma once
#include <complex>
#include <functional>
#include <string>
#include <vector>

namespace dect2::antd {

using cd = std::complex<double>;
constexpr double kC0 = 299792458.0;
constexpr double kMu0 = 1.25663706212e-6;
constexpr double kEps0 = 8.8541878128e-12;
constexpr double kEta0 = 376.730313668;
constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double norm() const;
};

struct Wire {
    Vec3 a, b;                 // ends, metres
    double radius = 0.001;     // metres
    int segments = 0;          // 0: automatic (Model::segPerWavelength at the highest frequency)
    double conductivity = 0;   // S/m; 0 = perfect conductor (copper 5.8e7, aluminium 3.5e7)
    int tag = 0;               // NEC tag (informational; 0 = the wire's index + 1)
};

struct Source {   // a delta-gap voltage source at a position along a wire
    int wire = 0;
    double pos = 0.5;          // 0..1 along the wire from a to b (0 or 1 only where that end touches the ground or another wire)
    cd volts = 1.0;
    int line = -1;             // >= 0: the source drives end 2 of that transmission line instead (a feeder: G5RV ladder line, coax run)
};

enum class LoadType { RX = 0, SeriesRLC, ParallelRLC };
struct Load {
    int wire = 0;
    double pos = 0.5;
    LoadType type = LoadType::RX;
    double r = 0, x = 0;       // RX: fixed R + jX ohms (all frequencies)
    double l = 0, c = 0;       // RLC: henry and farad (c = 0 in a series RLC means no capacitor; l = 0 in parallel means no inductor)
};

// a transmission line between two ports (as NEC TL): wire1/pos1 to wire2/pos2. A port is across the gap at that wire point: the networks,
// lines and a source at one point are in parallel; the wire's current flows into them. End 2 may instead be a source terminal (Source::line).
struct TLine {
    int wire1 = 0, wire2 = 0;
    double pos1 = 0.5, pos2 = 0.5;
    double z0 = 50;            // ohms
    double length = 0;         // physical metres; 0 = the straight distance between the two ports
    double vf = 1;             // velocity factor
    double lossDbPerM = 0;     // matched loss at the model frequency
    bool crossed = false;      // the line's wires swapped at one end (log-periodic feeders)
};

enum class GroundType { Free = 0, Perfect, Real };
struct Ground {
    GroundType type = GroundType::Free;
    double epsr = 13, sigma = 0.005;   // relative permittivity, conductivity S/m (average ground)
};

struct Model {
    std::string name;
    std::vector<Wire> wires;
    std::vector<Source> sources;
    std::vector<Load> loads;
    std::vector<TLine> lines;
    Ground ground;
    double segPerWavelength = 20;   // automatic segmentation density
    int minSegments = 3;            // per wire with a free end (connected wires may have 1)
};

// ---- segmentation --------------------------------------------------------------------------------------------------------------------------
struct Segment { Vec3 a, b; double radius = 0, conductivity = 0; int wire = 0; };
struct Basis {   // one triangle (two pieces) or a half triangle (one piece, at the ground)
    int nPieces = 0;
    int seg[2] = {-1, -1};
    int sign[2] = {1, 1};     // +1: the current flows from the segment's a to b
    bool rise[2] = {true, true};   // the triangle rises toward b (shape u/L) or falls (1 - u/L)
    Vec3 node;                // the peak of the triangle
};
struct Mesh {
    std::vector<Segment> segs;
    std::vector<Basis> basis;
    std::vector<int> srcBasis, loadBasis;   // per Model::sources / loads: the basis index (-1 = could not be placed)
    std::vector<int> srcSign;               // +1: the basis flows along the source wire's a -> b
    std::vector<int> tlBasis1, tlBasis2, tlSign1, tlSign2;
    std::vector<std::string> warnings;      // segmentation guidance (length / radius, segment length, junctions)
};
// fMaxHz: the highest frequency the mesh must hold (automatic segment counts); a node is put at every source, load and line position
Mesh buildMesh(const Model& m, double fMaxHz, double segScale = 1.0);
int autoSegments(const Wire& w, double fMaxHz, double segPerWavelength, int minSeg);
double wireLength(const Wire& w);

// ---- dense complex LU (blocked, partial pivoting, parallel trailing update for large matrices) ----------------------------------------------
// a: n x n row-major, overwritten by L and U; piv: row swaps. Returns false when singular.
bool luFactor(std::vector<cd>& a, int n, std::vector<int>& piv, int threads = 0);
void luSolve(const std::vector<cd>& lu, int n, const std::vector<int>& piv, std::vector<cd>& b);

// ---- solve one frequency ------------------------------------------------------------------------------------------------------------------
struct SegCurrent { cd ia, ib; };   // the current at the segment's a and b end, along a -> b
struct Solution {
    bool ok = false;
    std::string error;
    double fHz = 0;
    Mesh mesh;
    std::vector<cd> coef;             // basis amplitudes (A)
    std::vector<SegCurrent> segI;     // per segment
    std::vector<cd> srcZ, srcI;       // per source: input impedance and current
    cd zin = 0;                       // the first source's impedance
    double pIn = 0;                   // W, all sources (0.5 Re V I*)
    double pLoss = 0;                 // W in lumped loads and lossy wires
};
struct SolveOptions { int threads = 0; double segScale = 1.0; };
Solution solve(const Model& m, double fHz, const SolveOptions& o = SolveOptions());
Solution solveMesh(const Model& m, const Mesh& mesh, double fHz, int threads = 0);
cd loadImpedance(const Load& l, double fHz);
double vswr(cd z, double z0);
cd reflection(cd z, double z0);

// ---- far field ----------------------------------------------------------------------------------------------------------------------------
struct FarField { cd eth, eph; };   // r e^{jkr} E (V), the field times the distance without the phase
FarField farField(const Model& m, const Solution& s, double thetaDeg, double phiDeg);
struct Gain { double total = -999, vert = -999, horiz = -999; };   // dBi relative to the input power
Gain gainAt(const Model& m, const Solution& s, double thetaDeg, double phiDeg);

struct Pattern {
    double thStep = 5, phStep = 5;    // grid: theta 0..180 (0..90 over ground), phi 0..360 inclusive
    int nTh = 0, nPh = 0;
    std::vector<float> gdbi;          // nTh x nPh, total gain dBi (row = theta)
    std::vector<float> gv, gh;        // vertical / horizontal parts
    double maxDbi = -999, maxTheta = 0, maxPhi = 0;
    double pRad = 0;                  // W, integrated over the grid
    double efficiency = 0;            // pRad / pIn (ground loss included over a real ground), clamped to 0..1
    double directivityDbi = -999;
    double thAt(int i) const { return i * thStep; }
    double phAt(int j) const { return j * phStep; }
    float at(int i, int j) const { return gdbi[(size_t)i * nPh + j]; }
};
Pattern pattern(const Model& m, const Solution& s, double thStep = 5, double phStep = 5);

struct Figures {   // the numbers a pattern is judged by (from a pattern and cuts through its maximum)
    double maxDbi = -999, maxDbd = -999;
    double azMaxDeg = 0, elMaxDeg = 0;    // phi and elevation of the maximum
    double fbDb = 0;                       // front to back: the maximum against phi + 180 at the same elevation
    double fsDb = 0;                       // front to side (phi + 90)
    double bwAzDeg = 0, bwElDeg = 0;       // -3 dB widths in the azimuth cut (at elMax) and the elevation cut (at azMax); 360 / 180 = no edge
    double takeoffDeg = 0;                 // over ground: the elevation of the maximum
    double efficiency = 0;
};
// cuts: one-degree steps through the maximum of p
Figures figures(const Model& m, const Solution& s, const Pattern& p);
std::vector<double> azimuthCut(const Model& m, const Solution& s, double elDeg, double stepDeg = 1);    // gain dBi, phi 0..360
std::vector<double> elevationCut(const Model& m, const Solution& s, double phiDeg, double stepDeg = 1); // elevation -90..90 (0..90 over ground)

// ---- sweeps and checks --------------------------------------------------------------------------------------------------------------------
struct SweepPoint { double fHz = 0; cd z = 0; double swr = 0; double gainDbi = -999; double fbDb = 0; bool ok = false; };
// gainDir: also the gain at the maximum of a coarse pattern (slower). threads: frequencies in parallel.
std::vector<SweepPoint> sweep(const Model& m, double f0Hz, double f1Hz, int n, double z0, bool gainDir = false, int threads = 0,
                              const std::function<bool()>& cancel = nullptr);
struct Convergence { cd z1, z2; double changePct = 0; int n1 = 0, n2 = 0; bool converged = false; };
Convergence convergence(const Model& m, double fHz);   // the mesh at its density and at twice it: < 2 % change = converged

// ---- near field (optional output): the total E field at a point (V/m, peak), the source currents radiating, the image included ---------------
struct NearE { cd ex, ey, ez; };
NearE nearField(const Model& m, const Solution& s, const Vec3& p);

// ---- model helpers -------------------------------------------------------------------------------------------------------------------------
double modelExtent(const Model& m);
void translate(Model& m, const Vec3& d);
void mirror(Model& m, int axis, bool keepOriginal = true);              // axis 0 x, 1 y, 2 z: reflect in the plane through the origin
void arrayCopy(Model& m, const std::vector<int>& wires, int copies, const Vec3& step);
void rotateZ(Model& m, double deg);
std::vector<std::string> checkModel(const Model& m, double fHz);   // problems a user should fix (empty = fine)

} // namespace dect2::antd
