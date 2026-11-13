// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "RF camera": the spatial spectrum of a coherent multi-channel receiver (df.h) laid over a camera frame or a panorama picture as a heat map.
// Pure maths, no image decoding and no I/O: the angle <-> pixel mapping with a two-point field-of-view calibration, a sub-band split of the
// capture (so every transmitter keeps its own frequency and a band filter can pick some), the heat map blend and the peak marker layout.
// Azimuth only: the array is planar, so there is no elevation estimate; the heat map is constant down every column.
#pragma once
#include "dect2/df/df.h"
#include "dect2/df/source.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::df {

enum class Projection { Rectilinear, Equirect };   // a normal lens (pinhole) / a panorama whose columns are linear in azimuth

struct View {
    int w = 640, h = 360;
    double hfovDeg = 60;          // horizontal field of view of the picture (Equirect: the angle the whole width covers, 360 = full panorama)
    double centerAzDeg = 0;       // true azimuth (clockwise from north) of the picture's centre column
    Projection proj = Projection::Rectilinear;
};

// Column of azimuth `azDeg` (pixel centre coordinates, may be fractional); false when it is outside the picture (Rectilinear) / off the panorama
bool azToX(const View& v, double azDeg, double& x);
// Azimuth (0..360) of column x; for Rectilinear x outside the picture is still mapped (the lens model extended), never NaN inside |dAz| < 90
double xToAz(const View& v, double x);

// Calibrate from two landmarks: column x1 shows azimuth az1, column x2 shows az2. Fills hfovDeg and centerAzDeg for the given projection.
// False when the points are degenerate (same column / azimuth, or no consistent lens).
bool calibrateView(View& v, double x1, double az1, double x2, double az2);

struct BandSpec { double loHz, hiHz; };            // baseband offsets from the centre frequency

struct BandResult {
    BandSpec band{0, 0};
    double freqHz = 0;                              // absolute centre frequency of the band
    double snrDb = 0;                               // dominant eigenvalue over the median of the rest
    double weight = 0;                              // 0..1 how much signal the band holds (0 = noise only)
    Result res;
};

struct CamOptions {
    Options df;                                     // estimator settings (compassOffsetDeg is applied)
    int bands = 16;                                 // sub-bands over the selected range
    int fftSize = 256;                              // power of two
    double filterLoHz = 0, filterHiHz = 0;          // band filter relative to the centre; both 0 = the whole sample rate
    double minSnrDb = 3;                            // below this a band counts as empty
};

struct CamPeak { double bearingDeg; double freqHz; double strength; };   // strength 0..1

struct CamResult {
    std::vector<double> bearings;                   // grid (true azimuth, compass applied), degrees
    std::vector<double> spectrum;                   // 0..1, max over the bands of (band spectrum * band weight)
    std::vector<BandResult> bands;
    std::vector<CamPeak> peaks;                     // strongest first
};

// Split x (n channels, T samples at `sampleRate`) into sub-bands, estimate each, combine. `centerHz` is the tuned frequency.
CamResult analyse(const Frame& x, const Array& arr, double centerHz, double sampleRate, const CamOptions& opt);

struct HeatOptions { double floorDb = 12; double maxAlpha = 0.65; double threshold = 0.08; };   // spectrum range shown, strongest opacity, below which nothing is drawn

// 0..1 display level of a normalised spectrum value: 0 at -floorDb, 1 at the top
double displayLevel(double v, double floorDb);
// Heat map colour (RGBA bytes packed 0xAABBGGRR) of level 0..1: transparent, blue, cyan, yellow, red
uint32_t heatColor(double level, double maxAlpha);
// Blends the heat map over `px` (w*h, 0xAABBGGRR as plat::decodeImage). `spectrum` is indexed by `bearings` (a closed circle grid).
void blendHeat(std::vector<uint32_t>& px, const View& v, const std::vector<double>& bearings, const std::vector<double>& spectrum, const HeatOptions& o = {});
// Spectrum value at an azimuth (linear interpolation on the circular grid)
double sampleSpectrum(const std::vector<double>& bearings, const std::vector<double>& spectrum, double azDeg);

struct Marker {
    double x = 0, y = 0;            // pixel position of the peak marker (y = row of the marker tip)
    double bearingDeg = 0, freqHz = 0, strength = 0;
    int labelRow = -1;              // 0 = nearest the marker, -1 = label dropped (no free room)
    std::string label;              // "433.920 MHz"
};
struct MarkerOptions { double minStrength = 0.15; double mergeDeg = 3; int maxMarkers = 6; double labelWidthPx = 80; int labelRows = 3; double yFrac = 0.5; };
// Peaks visible in the view, nearby ones merged (the stronger kept), strongest first, labels stacked in rows so they do not overlap.
std::vector<Marker> layoutMarkers(const std::vector<CamPeak>& peaks, const View& v, const MarkerOptions& o = {});

std::string formatFreq(double hz);   // "433.920 MHz", "1.2 kHz" below 1 MHz

}   // namespace dect2::df
