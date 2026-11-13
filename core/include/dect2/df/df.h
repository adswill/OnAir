// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Direction finding with a coherent multi-channel receiver (KrakenSDR style): array geometry, snapshot covariance, bearing estimators
// (Bartlett, Capon, MUSIC, root-MUSIC, correlative interferometer), element calibration, beamforming, confidence and circular statistics.
// Pure deterministic C++; the algorithms are the textbook ones (Bartlett 1948, Capon 1969, Schmidt 1986 MUSIC, Barabell 1983 root-MUSIC,
// Shan/Wax/Kailath 1985 spatial smoothing, Pillai/Kwon forward-backward averaging), implemented from scratch.
// Bearings are degrees clockwise from the array's north (x = east, y = north, metres).
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2::df {

using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;
constexpr double kC = 299792458.0;

struct Array {
    std::vector<std::array<double, 2>> pos;   // element positions (m), x east, y north
    bool isUla = false;                       // uniform linear array along x (needed for root-MUSIC and spatial smoothing)
    double spacing = 0;                       // ULA spacing (m)
    std::vector<double> z;                    // optional element heights (m) for a 3D array; empty = planar (elevation then has no effect)
    int n() const { return (int)pos.size(); }
    // Uniform circular array: n elements on a circle of `radius`, element 0 at `orientationDeg` (clockwise from north)
    static Array uca(int n, double radius, double orientationDeg = 0);
    // Uniform linear array along the east-west axis rotated to `orientationDeg` (the broadside is then orientationDeg+90 / -90)
    static Array ula(int n, double spacing, double orientationDeg = 90);
    static Array custom(std::vector<std::array<double, 2>> p);
    // Half-wavelength-ish radius for a UCA: the radius that gives `spacingWavelengths` between neighbours
    static double ucaRadiusForSpacing(int n, double wavelength, double spacingWavelengths = 0.5);
};

struct CMat {
    int n = 0;
    std::vector<cd> a;
    CMat() = default;
    explicit CMat(int n_) : n(n_), a((size_t)n_ * n_) {}
    cd& operator()(int r, int c) { return a[(size_t)r * n + c]; }
    const cd& operator()(int r, int c) const { return a[(size_t)r * n + c]; }
};

// Steering vector of the array for a plane wave from `bearingDeg`
std::vector<cd> steering(const Array& arr, double wavelength, double bearingDeg);

// Snapshot matrix x[channel][t] (n channels, T snapshots) -> sample covariance R = X X^H / T
CMat covariance(const std::vector<std::vector<cd>>& x);
CMat forwardBackward(const CMat& r);                    // (R + J R* J) / 2, decorrelates a coherent multipath pair on a ULA/UCA-like centro-symmetric array
CMat spatialSmooth(const CMat& r, int subarray);        // ULA only: average of the covariance of the sliding subarrays, size subarray x subarray

// Hermitian eigen decomposition, eigenvalues descending, vecs[k] is the k-th eigenvector (unit)
void eigh(const CMat& r, std::vector<double>& vals, std::vector<std::vector<cd>>& vecs);
// Solve A x = b (Gauss, partial pivoting); false when singular
bool solve(CMat a, std::vector<cd> b, std::vector<cd>& x);

enum class Method { Bartlett, Capon, Music, Interferometer };

struct Options {
    Method method = Method::Music;
    int sources = 1;                // MUSIC signal subspace size (0 = estimate with MDL)
    bool forwardBackward = false;
    int smoothing = 0;              // ULA subarray size for spatial smoothing (0 = none)
    double gridStepDeg = 1.0;
    double loading = 1e-3;          // diagonal loading (fraction of the mean eigenvalue) for Capon
    double compassOffsetDeg = 0;    // added to every result (array north vs true north)
};

struct Peak { double bearingDeg; double value; };

struct Result {
    std::vector<double> bearings;   // grid
    std::vector<double> spectrum;   // normalised to a max of 1
    std::vector<Peak> peaks;        // strongest first, sub-grid interpolated bearing
    double bearingDeg = 0;          // strongest peak (+ compass offset), 0..360
    double psldB = 0;               // peak-to-sidelobe ratio: strongest peak / second strongest local maximum (dB), large when unambiguous
    double coherence = 0;           // dominant eigenvalue / trace, 1 = one clean coherent source, 1/N = noise only
    double confidence = 0;          // 0..1 mix of coherence and PSL
    std::vector<double> eigenvalues;
    int sourcesUsed = 0;
};

Result estimate(const CMat& r, const Array& arr, double wavelength, const Options& opt);
// Estimate the number of sources from the eigenvalues with the MDL criterion (T snapshots)
int estimateSources(const std::vector<double>& eig, int snapshots);
// Root-MUSIC for a ULA: bearings (degrees, relative to the array's own broadside convention already converted to the array frame) of `sources` signals
std::vector<double> rootMusic(const CMat& r, const Array& arr, double wavelength, int sources);

// Per-element complex gain (amplitude + phase) from a covariance recorded while a calibration source was active.
// knownBearingDeg < -360 or NaN: a common noise source feeds all elements equally (steering vector all ones).
std::vector<cd> estimateCalibration(const CMat& r, const Array& arr, double wavelength, double knownBearingDeg);
CMat applyCalibration(const CMat& r, const std::vector<cd>& gains);          // removes the gains from a covariance
void applyCalibration(std::vector<std::vector<cd>>& x, const std::vector<cd>& gains);   // ... or from raw samples

// Weights listening in `lookDeg` with nulls at `nullDegs` (LCMV on white noise); apply with beamform().
std::vector<cd> beamWeights(const Array& arr, double wavelength, double lookDeg, const std::vector<double>& nullDegs = {});
std::vector<cd> beamform(const std::vector<std::vector<cd>>& x, const std::vector<cd>& w);

// ---- circular statistics (degrees)
double wrap360(double d);
double wrap180(double d);
double angDiff(double a, double b);               // signed a-b in (-180,180]
double circMean(const std::vector<double>& d, const std::vector<double>* w = nullptr);
double circResultant(const std::vector<double>& d, const std::vector<double>* w = nullptr);   // mean resultant length 0..1
double circStd(const std::vector<double>& d);     // degrees, sqrt(-2 ln R)

// Averages bearings over time (exponential, with confidence weights); the mean and the circular spread are exposed.
class BearingAverager {
public:
    explicit BearingAverager(int window = 20) : win_(window) {}
    void add(double bearingDeg, double weight = 1.0);
    bool empty() const { return b_.empty(); }
    double mean() const;
    double stdDeg() const;
    size_t size() const { return b_.size(); }
    void clear() { b_.clear(); w_.clear(); }
private:
    int win_;
    std::vector<double> b_, w_;
};

// ---- deterministic generator
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next();
    double uni();          // (0,1)
    double gauss();
};

}   // namespace dect2::df
