// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Image enhancement shared by every picture mode (WEFAX, SSTV, NOAA APT, Meteor LRPT, GOES/Elektro/METEOSAT, DRM/DAB slide shows, RF camera,
// TV frame grabs, KAIT, Wenet/SSDV). Pure functions on float pictures, classical algorithms only, deterministic for any thread count
// (row bands on ThreadPool::shared(), each band writes only its own rows). See docs/IMAGE-ENHANCE.md.
//
// NON-DESTRUCTIVE by design: nothing here modifies the input picture; enhance() returns a new picture. The receivers keep and save the
// raw decoded picture; the enhanced one is a view and an extra file.
//
// Stages of enhance(), in order (each switched by Params):
//   geometry  slant (clock error) from the line-to-line drift + manual slant, phasing (cyclic line-start shift so the picture is not split),
//             repair of missed / corrupted lines, line re-registration (jitter), aspect / IOC width
//   noise     switching median (impulses only), destripe (per-line gain/offset) and deband (per-column), bilateral or non-local means
//   tone      chart levels (bimodal black/white tuning for fax charts) or auto levels (percentile stretch, protected black/white), gamma,
//             CLAHE, palette (APT false colour, temperature)
//   detail    chroma smoothing + chroma phase fix + grey-world (colour pictures), deinterlace, unsharp mask, Lanczos x2 without ringing
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::image {

// A picture with 1 (grey) or 3 (R, G, B) planes of floats in 0..1, planar, row-major
struct ImageF {
    int w = 0, h = 0, ch = 1;
    std::vector<float> d;   // ch * w * h
    ImageF() = default;
    ImageF(int w_, int h_, int ch_, float v = 0.f) : w(w_), h(h_), ch(ch_), d((size_t)w_ * h_ * ch_, v) {}
    float* plane(int c) { return d.data() + (size_t)c * w * h; }
    const float* plane(int c) const { return d.data() + (size_t)c * w * h; }
    float& at(int c, int x, int y) { return d[((size_t)c * h + y) * w + x]; }
    float at(int c, int x, int y) const { return d[((size_t)c * h + y) * w + x]; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// conversions. fromRgba: grey when every pixel has r == g == b (or forceGrey); toRgba: alpha 255
ImageF fromRgba(const uint8_t* rgba, int w, int h, bool forceGrey = false);
ImageF fromGrey(const uint8_t* g, int w, int h);
std::vector<uint8_t> toRgba(const ImageF& im);
ImageF luma(const ImageF& rgb);                                   // 1 plane (BT.601)
void toYCbCr(ImageF& rgb);                                        // in place, planes Y, Cb+0.5, Cr+0.5
void fromYCbCr(ImageF& ycc);

// ---------------------------------------------------------------- geometry
struct SlantFit {
    double pxPerLine = 0;      // line start drift: line y is shifted right by pxPerLine * y (clock error * line length)
    double offset = 0;         // shift of line 0 (from the fit)
    double rms = 0;            // rms residual of the fit, px
    int used = 0;              // line pairs used by the fit
};
// Estimates the drift of line starts over the picture by circular cross-correlation of each line with the one before (a stable
// feature such as a sync bar or margin is the strongest correlation), cumulative sum, robust straight line fit. maxStep: largest
// shift between two lines searched, px.
SlantFit estimateSlant(const ImageF& im, int maxStep = 6);
// Resamples every line: line y is shifted left by pxPerLine * y + offset (circular when wrap, else edge clamped), linear interpolation
ImageF deslant(const ImageF& im, double pxPerLine, double offset = 0, bool wrap = true);
// Line-start (phasing) alignment: the column where the picture should start, found as the most constant vertical band of bandPct
// percent of the width (a sync bar or margin, the same on every line). Returns the cyclic shift to apply (0..w-1).
int findPhase(const ImageF& im, double bandPct = 4.0);
ImageF rotateColumns(const ImageF& im, int shift);              // column x of the result = column (x + shift) mod w of the input
// Missed or corrupted lines: a line whose correlation with its neighbours collapses (or that is flat / saturated while its
// neighbours are not) is replaced by interpolation between the nearest good lines above and below. Returns the lines repaired.
int repairLines(ImageF& im, double minCorr = 0.35, std::vector<int>* fixed = nullptr);
// Re-registers every line to its neighbours by circular cross-correlation (sub-pixel), removing line jitter up to maxShift px
int registerLines(ImageF& im, int maxShift = 4);
ImageF resize(const ImageF& im, int nw, int nh);                // bilinear (area average when shrinking by more than 2)
// WEFAX IOC: picture width for an index of cooperation (576 -> 1810, 288 -> 905); returns im resized in width only
ImageF iocWidth(const ImageF& im, int ioc);

// ---------------------------------------------------------------- noise
// Switching median: only isolated pixels (at most one of the 8 neighbours alike) that differ from the 3x3 median by more than thr are
// replaced, so 1 px chart lines and text survive. Returns the pixels replaced.
int switchingMedian(ImageF& im, float thr = 0.25f);
ImageF bilateral(const ImageF& im, float sigmaSpace, float sigmaRange);
ImageF nlMeans(const ImageF& im, float h, int searchRadius = 4, int patchRadius = 1);
// denoise strength 0..100 to the filters above (nlm = non-local means, else bilateral)
ImageF denoise(const ImageF& im, int strength, bool nlm);
// Per-line gain and offset equalisation: each row's mean and spread are pulled to the running median of its neighbours (APT, LRPT stripes)
void destripe(ImageF& im, int window = 15);
// The same per column (vertical banding)
void deband(ImageF& im, int window = 31);
// Temporal average of repeated transmissions: every copy is aligned to the first (integer shift by SAD search up to maxShift, circular
// in x) and the copies are averaged (median of 3 or more). Copies of other sizes are skipped. dx/dy (optional) get the shifts found.
ImageF temporalAverage(const std::vector<ImageF>& copies, int maxShift = 32, std::vector<int>* dx = nullptr, std::vector<int>* dy = nullptr);

// ---------------------------------------------------------------- tone
// Percentile stretch on luma with protected black and white points: the gain is at most maxGain (a dark or flat picture is not
// stretched into noise), and the range is widened around its centre when needed. Colour pictures get one curve for all planes.
void autoLevels(ImageF& im, double loPct = 0.5, double hiPct = 99.5, float maxGain = 4.f);
void gamma(ImageF& im, float g);                                  // out = in^(1/g): above 1 brightens
// CLAHE on luma: tiles x tiles regions, clip limit (1 = none, 2..4 usual), bilinear interpolation of the tile curves
void clahe(ImageF& im, float clipLimit = 2.5f, int tiles = 8);
// Histogram matching: the luma of im gets the distribution of ref (satellite channels to a reference channel)
void histMatch(ImageF& im, const ImageF& ref);
// Weather chart levels: Otsu threshold between ink and paper, the two class means go to black and white with a smooth (never
// binary) curve, so thin lines keep their antialiasing. strength 0..1 = how hard the step around the threshold is.
void chartLevels(ImageF& im, float strength = 0.6f);
enum class Palette { None, SeaLandCloud, Temperature, Rainbow };
ImageF applyPalette(const ImageF& grey, Palette p);              // grey (luma of im) -> RGB

// ---------------------------------------------------------------- detail and colour
void unsharp(ImageF& im, float radius = 1.0f, float amount = 0.6f, float threshold = 0.01f);
ImageF lanczos2x(const ImageF& im, float sharpen = 0.3f);         // x2, clamped to the local range (no ringing)
// SSTV colour: chroma shifted to match luma (phase offset found by cross-correlation, |shift| <= maxShift px), chroma smoothed
// with sigmaC (much more than luma), returns the chroma shift found
double chromaFix(ImageF& rgb, float sigmaC = 1.5f, int maxShift = 4);
void greyWorld(ImageF& rgb, float maxGain = 1.5f);
// Deinterlace: lines of the second field that disagree with the average of the first field lines around them by more than thr
// are replaced by that average. Returns the pixels replaced.
int deinterlace(ImageF& im, float thr = 0.12f);
void gaussian(ImageF& im, float sigma);

// ---------------------------------------------------------------- the pipeline
enum class Kind { Generic, Fax, Sstv, Apt, Satellite, Slide, Tv };
Kind kindForSource(const std::string& source);                   // "Weather fax" -> Fax, "SSTV" -> Sstv, "NOAA APT" -> Apt ...
enum class Preset { Auto, Chart, Photo, Satellite, ColourSstv, Custom };
const char* presetName(Preset p);
constexpr int kPresets = 6;

struct Params {
    // geometry
    bool autoSlant = false;   float slant = 0;        // manual slant px/line, added to the automatic one
    bool autoPhase = false;   int phase = 0;          // manual cyclic shift added
    bool repair = false;      float repairCorr = 0.35f;
    bool registerRows = false;
    int ioc = 0;                                      // 0 = keep the width
    // noise
    bool impulse = false;     float impulseThr = 0.25f;
    bool destripe = false, deband = false;
    int denoise = 0;          bool nlm = false;       // strength 0..100
    // tone
    bool chart = false;       float chartStrength = 0.6f;
    bool levels = false;      float loPct = 0.5f, hiPct = 99.5f;
    float gamma = 1.f;
    float clahe = 0;                                  // clip limit, 0 = off
    Palette palette = Palette::None;
    // detail
    bool chroma = false, greyWorld = false, deinterlace = false;
    float sharpen = 0;                                // unsharp amount, 0 = off
    float sharpenRadius = 1.0f;
    bool upscale = false;
};
Params presetParams(Preset p, Kind k);
// Auto: the preset for the picture (a grey, mostly bimodal picture from a fax is a Chart, a colour picture Photo or Colour SSTV ...)
Preset autoPreset(const ImageF& im, Kind k);

struct Report {
    double slantPx = 0;       // automatic slant applied, px/line
    int phase = 0;            // cyclic shift applied
    int repaired = 0, impulses = 0, deint = 0;
    double chromaShift = 0;
    double ms = 0;            // time taken
};
// preview: the cheap path for a picture that is still arriving line by line (no non-local means, no upscaling, no registration)
ImageF enhance(const ImageF& in, const Params& p, Report* rep = nullptr, bool preview = false);
std::vector<uint8_t> enhanceRgba(const uint8_t* rgba, int w, int h, const Params& p, int& ow, int& oh, Report* rep = nullptr, bool preview = false);

// Before/after split: columns left of splitX (0..1 of the width) from before, the rest from after (both RGBA, the same size)
void splitView(const uint8_t* before, const uint8_t* after, int w, int h, float splitX, uint8_t* out);

// Params <-> text (for the preferences): "key=value;..." ; unknown keys ignored
std::string paramsToString(const Params& p);
Params paramsFromString(const std::string& s, const Params& base);

// ---------------------------------------------------------------- measures (tests, docs)
double psnr(const ImageF& a, const ImageF& b);                    // dB, on all planes, peak 1
double ssim(const ImageF& a, const ImageF& b);                    // mean SSIM on luma, 8-bit constants, gaussian window sigma 1.5

}  // namespace dect2::image
