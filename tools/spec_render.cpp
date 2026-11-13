// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Offscreen render harness for the spectrum and waterfall (no window, no GPU, no App): an IQ recording or a built-in realistic signal
// goes through the app's own pipeline (core SpectrumAnalyzer, the trace average, Waterfall history + auto range, plt::WfView) and the
// app's own drawing (plt plots with the look, gfx/spec_paint.h), and the ImGui draw lists are rasterised in software into a PNG.
//
//   spec_render --in rec.cs8 --fs 10e6 --fc 474 [--fmt cs8|cu8|cf32|wav] [--gen wide|hf|narrow] [--look 1] [--seconds 60]
//               [--view lo hi (MHz)] [--vfo f bw (MHz)] [--w 1400] [--h 800] [--blocks 2] [--fft 0] [--old] [--bench] --out pic.png
// --old: the pipeline as it was before 3.0 (16 blocks a frame, the fixed -100..-32 dB range): for before/after pictures and CPU.
// --bench: prints the CPU time per second of signal spent in the pipeline (analyser, trace, waterfall rows, view).
#include "../app/gfx/plot.h"
#include "../app/gfx/spec_paint.h"
#include "../app/gfx/waterfall.h"
#include "../app/gfx/colormap.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/dsp/dsp_compat.h"
#include "imgui.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../app/ui/touch.h"

using cf = std::complex<float>;

// the plots ask about the touch interface: not here
namespace touch {
bool on() { return false; }
bool longPressHeld() { return false; }
const GestureFrame& gesture() { static GestureFrame g; return g; }
}

// ------------------------------------------------------------------------------------------------ software rasteriser for ImGui draw data
struct Tex { int w = 0, h = 0; bool linear = false; std::vector<uint32_t> px; };   // RGBA (0xAABBGGRR); linear: sampled with a linear filter (pictures; not the font atlas)
static std::map<ImTextureID, Tex> gTex;
static ImTextureID gNextTex = 1;

struct Canvas {
    int w, h;
    std::vector<float> r, g, b;
    Canvas(int W, int H) : w(W), h(H), r((size_t)W * H, 0), g((size_t)W * H, 0), b((size_t)W * H, 0) {}
};

static inline void sample(const Tex& t, float u, float v, bool linear, float out[4]) {
    if (t.px.empty()) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    auto texel = [&](int x, int y, float* c) {
        x = std::max(0, std::min(t.w - 1, x)); y = std::max(0, std::min(t.h - 1, y));
        const uint32_t p = t.px[(size_t)y * t.w + x];
        c[0] = (p & 255) / 255.f; c[1] = ((p >> 8) & 255) / 255.f; c[2] = ((p >> 16) & 255) / 255.f; c[3] = (p >> 24) / 255.f;
    };
    if (!linear) { texel((int)std::floor(u * t.w), (int)std::floor(v * t.h), out); return; }
    const float fx = u * t.w - 0.5f, fy = v * t.h - 0.5f;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float ax = fx - x0, ay = fy - y0;
    float c00[4], c10[4], c01[4], c11[4];
    texel(x0, y0, c00); texel(x0 + 1, y0, c10); texel(x0, y0 + 1, c01); texel(x0 + 1, y0 + 1, c11);
    for (int k = 0; k < 4; k++) out[k] = (c00[k] * (1 - ax) + c10[k] * ax) * (1 - ay) + (c01[k] * (1 - ax) + c11[k] * ax) * ay;
}

static void rasterTri(Canvas& cv, const ImDrawVert& A, const ImDrawVert& B, const ImDrawVert& C, const Tex* tex, const ImVec4& clip) {
    const ImVec2 a = A.pos, b = B.pos, c = C.pos;
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-12f) return;
    const int x0 = std::max({0, (int)std::floor(std::min({a.x, b.x, c.x})), (int)std::floor(clip.x)});
    const int x1 = std::min({cv.w - 1, (int)std::ceil(std::max({a.x, b.x, c.x})), (int)std::ceil(clip.z) - 1});
    const int y0 = std::max({0, (int)std::floor(std::min({a.y, b.y, c.y})), (int)std::floor(clip.y)});
    const int y1 = std::min({cv.h - 1, (int)std::ceil(std::max({a.y, b.y, c.y})), (int)std::ceil(clip.w) - 1});
    auto col = [](ImU32 p, float* o) { o[0] = (p & 255) / 255.f; o[1] = ((p >> 8) & 255) / 255.f; o[2] = ((p >> 16) & 255) / 255.f; o[3] = (p >> 24) / 255.f; };
    float ca[4], cb[4], cc[4]; col(A.col, ca); col(B.col, cb); col(C.col, cc);
    // a texture that is not the font atlas (a picture) is sampled with a linear filter, as the app's back ends do
    const bool linear = tex && tex->linear;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            if (px < clip.x || px >= clip.z || py < clip.y || py >= clip.w) continue;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const float w2 = 1.f - w0 - w1;
            if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
            float s[4];
            for (int k = 0; k < 4; k++) s[k] = ca[k] * w0 + cb[k] * w1 + cc[k] * w2;
            if (tex) {
                const float u = A.uv.x * w0 + B.uv.x * w1 + C.uv.x * w2, v = A.uv.y * w0 + B.uv.y * w1 + C.uv.y * w2;
                float t[4]; sample(*tex, u, v, linear, t);
                for (int k = 0; k < 4; k++) s[k] *= t[k];
            }
            const size_t i = (size_t)y * cv.w + x;
            cv.r[i] = s[0] * s[3] + cv.r[i] * (1 - s[3]);
            cv.g[i] = s[1] * s[3] + cv.g[i] * (1 - s[3]);
            cv.b[i] = s[2] * s[3] + cv.b[i] * (1 - s[3]);
        }
}

static void rasterise(Canvas& cv, ImDrawData* dd) {
    if (dd->Textures)
        for (ImTextureData* t : *dd->Textures) {
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
                ImTextureID id = t->TexID != ImTextureID_Invalid ? t->TexID : gNextTex++;
                Tex& x = gTex[id];
                x.w = t->Width; x.h = t->Height; x.linear = true; x.px.resize((size_t)x.w * x.h);
                const unsigned char* p = (const unsigned char*)t->GetPixels();
                for (int i = 0; i < x.w * x.h; i++)
                    x.px[(size_t)i] = t->Format == ImTextureFormat_Alpha8 ? (0x00FFFFFFu | ((uint32_t)p[i] << 24)) : ((const uint32_t*)p)[i];
                t->SetTexID(id); t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantDestroy) { t->SetTexID(ImTextureID_Invalid); t->SetStatus(ImTextureStatus_Destroyed); }
        }
    for (int l = 0; l < dd->CmdListsCount; l++) {
        const ImDrawList* d = dd->CmdLists[l];
        for (const ImDrawCmd& c : d->CmdBuffer) {
            if (c.UserCallback) continue;
            auto it = gTex.find(c.GetTexID());
            const Tex* tex = it == gTex.end() ? nullptr : &it->second;
            for (unsigned i = 0; i + 2 < c.ElemCount; i += 3) {
                const ImDrawVert& a = d->VtxBuffer[(int)c.VtxOffset + d->IdxBuffer[(int)(c.IdxOffset + i)]];
                const ImDrawVert& b = d->VtxBuffer[(int)c.VtxOffset + d->IdxBuffer[(int)(c.IdxOffset + i + 1)]];
                const ImDrawVert& e = d->VtxBuffer[(int)c.VtxOffset + d->IdxBuffer[(int)(c.IdxOffset + i + 2)]];
                rasterTri(cv, a, b, e, tex, c.ClipRect);
            }
        }
    }
}

struct ImgStore : gfx::Image {   // the waterfall view texture, kept in gTex
    ImTextureID id;
    ImgStore(int w, int h, uint32_t fill) { id = gNextTex++; Tex& t = gTex[id]; t.w = w; t.h = h; t.linear = true; t.px.assign((size_t)w * h, fill); }
    ImTextureID texture() const override { return id; }
    void update(int x, int y, int w, int h, const uint32_t* rgba) override {
        Tex& t = gTex[id];
        for (int r = 0; r < h; r++) std::memcpy(&t.px[(size_t)(y + r) * t.w + x], rgba + (size_t)r * w, (size_t)w * 4);
    }
};

// ------------------------------------------------------------------------------------------------ signal sources
struct Source {
    virtual ~Source() {}
    virtual double rate() const = 0;
    virtual double centreMhz() const = 0;
    // n samples at time t (seconds): true samples, as a radio delivers them
    virtual void get(cf* out, size_t n, double t) = 0;
};

struct FileSource : Source {
    FILE* f = nullptr; std::string fmt; double fs, fc; long dataOff = 0;
    std::vector<int8_t> b8; std::vector<uint8_t> u8; std::vector<float> f32;
    FileSource(const char* path, const std::string& fm, double rate, double fcMhz) : fmt(fm), fs(rate), fc(fcMhz) {
        f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
        if (fmt == "wav") { dataOff = 44; fseek(f, 44, SEEK_SET); }
    }
    double rate() const override { return fs; }
    double centreMhz() const override { return fc; }
    size_t rd(void* p, size_t sz, size_t n) {
        size_t got = fread(p, sz, n, f);
        if (got < n) { fseek(f, dataOff, SEEK_SET); got += fread((char*)p + got * sz, sz, n - got, f); }   // loop the recording
        return got;
    }
    void get(cf* out, size_t n, double) override {
        if (fmt == "cs8") { b8.resize(2 * n); rd(b8.data(), 1, 2 * n); for (size_t i = 0; i < n; i++) out[i] = cf(b8[2 * i] / 128.f, b8[2 * i + 1] / 128.f); }
        else if (fmt == "cu8") { u8.resize(2 * n); rd(u8.data(), 1, 2 * n); for (size_t i = 0; i < n; i++) out[i] = cf((u8[2 * i] - 127.5f) / 128.f, (u8[2 * i + 1] - 127.5f) / 128.f); }
        else { f32.resize(2 * n); rd(f32.data(), 4, 2 * n); for (size_t i = 0; i < n; i++) out[i] = cf(f32[2 * i], f32[2 * i + 1]); }
    }
};

// A realistic synthetic band: the noise and every noise-like signal are made in the frequency domain per block (complex Gaussian bins
// with the power density of the moment, then an inverse FFT), so every FFT bin has the chi-square statistics of a real receiver; carriers
// and FM stations are added in the time domain. The noise floor tilts and rolls off at the band edges like a real front end.
struct GenSource : Source {
    std::string kind; double fs, fc; size_t N; int log2n;
    std::mt19937 rng{12345};
    std::normal_distribution<float> nd{0.f, 1.f};
    std::vector<float> re, im, psd;
    struct Carrier { double fHz, drift, dbfs; double ph = 0; int fm = 0; double dev = 0; double mph = 0; double mrate = 0; double burstOn = 0, burstPeriod = 0; };
    std::vector<Carrier> cw;
    GenSource(const std::string& k, size_t fft) : kind(k), N(fft) {
        log2n = (int)std::lround(std::log2((double)N));
        if (kind == "wide") { fs = 20e6; fc = 522.0; }
        else if (kind == "adsb") { fs = 4e6; fc = 1090.0; }
        else if (kind == "hf") { fs = 192e3; fc = 17.77; }
        else { fs = 48e3; fc = 7.1; }
        re.resize(N); im.resize(N); psd.resize(N);
        if (kind == "wide") {
            cw.push_back({-8.3e6, 0, -38, 0, 1, 75e3, 0, 900});   // FM-like wide carriers (75 kHz deviation)
            cw.push_back({-7.6e6, 0, -47, 0, 1, 75e3, 1, 1300});
            cw.push_back({ 7.25e6, 0, -50});                     // the PAL vision carrier
            cw.push_back({ 2.31e6, 40, -55});                     // narrow carriers, one drifting
            cw.push_back({ 4.07e6, 0, -64});
            cw.push_back({ 3.33e6, -900, -70});
            cw.push_back({-3.95e6, 0, -60, 0, 0, 0, 0, 0, 0.35, 2.3});   // a bursty carrier
        } else if (kind == "adsb") {
            cw.push_back({-1.62e6, 0, -70}); cw.push_back({0.4e6, 0, -78}); cw.push_back({1.38e6, 0, -66});
        } else if (kind == "hf") {
            cw.push_back({-26.3e3, 0, -62}); cw.push_back({ 31.2e3, 0, -60}); cw.push_back({-15.6e3, 3, -78}); cw.push_back({ 62.0e3, 0, -80});
            cw.push_back({ 0.0e3, 0, -66, 0, 2, 0, 0, 3});   // an AM carrier at the centre (its sidebands are noise-like voice below)
        } else {
            cw.push_back({-19.0e3, 0, -78}); cw.push_back({-16.1e3, 0, -66}); cw.push_back({-13.5e3, 0, -72}); cw.push_back({ 9.2e3, 0, -80});
        }
    }
    double rate() const override { return fs; }
    double centreMhz() const override { return fc; }
    // a smooth "voice" envelope 0..1 at time t: syllables (4 Hz), words, pauses
    static double voice(double t, double seed) {
        const double s = 0.5 + 0.5 * std::sin(2 * M_PI * 3.7 * t + seed) * std::sin(2 * M_PI * 0.61 * t + 2 * seed);
        const double w = std::sin(2 * M_PI * 0.23 * t + seed * 3) > -0.3 ? 1.0 : 0.05;
        return std::max(0.0, s) * w;
    }
    void makePsd(double t) {
        const double df = fs / N;
        for (size_t k = 0; k < N; k++) {
            const double f = ((double)k < N / 2.0 ? (double)k : (double)k - (double)N) * df;   // FFT order: 0..fs/2, -fs/2..0
            const double u = f / (fs / 2);
            double db = -101.0 + 2.5 * std::cos(u * 2.4) - 1.5 * u;                     // a gentle tilt
            db -= 14.0 * std::pow(std::max(0.0, std::fabs(u) - 0.86) / 0.14, 2.0);       // the front end's roll-off at the edges
            double p = std::pow(10.0, db / 10.0);
            auto blk = [&](double f0, double bw, double dbB, double steep) {             // a flat block with steep sides
                const double x = std::fabs(f - f0) / (bw / 2);
                p += std::pow(10.0, dbB / 10.0) / (1.0 + std::pow(x, steep));
            };
            if (kind == "wide") {
                {   // a DVB-T mux (8 MHz channel) through a two-path channel: a slow ripple across it that drifts in time
                    const double x = std::fabs(f + 1.2e6) / (7.61e6 / 2);
                    const double ripple = std::fabs(1.0 + 0.45 * std::cos(2 * M_PI * f * 1.9e-6 + 0.7 * t)) * (1.0 + 0.25 * std::cos(2 * M_PI * f * 0.31e-6 - 0.4 * t));
                    p += std::pow(10.0, -78.5 / 10.0) * ripple * ripple / (1.0 + std::pow(x, 60));
                }
                {   // an analogue PAL picture at the top of the span: vision carrier, its vestigial sideband, the colour subcarrier
                    const double fv = 7.25e6, d = f - fv;
                    if (d > -1.25e6 && d < 5.0e6) p += std::pow(10.0, (-84 - 3.0 * std::max(0.0, d) / 1e6) / 10.0) * (0.6 + 0.4 * std::fabs(std::sin(d / 15625.0 * M_PI)));
                }
                blk(5.0e6, 0.9e6, -95, 30);                // a weak digital block under the picture
                if (std::fmod(t, 1.7) < 0.9) blk(3.7e6, 1.08e6, -84, 30);   // an LTE-like burst block
                for (int i = 0; i < 6; i++) if (std::fmod(t + i * 0.41, 2.9) < 0.25) blk(-6.4e6 + i * 0.21e6, 25e3, -82, 6);   // short NFM bursts
                const double ssb = voice(t, 1.0);
                if (f > 1.402e6 && f < 1.4048e6) p += std::pow(10.0, (-95 + 22 * ssb) / 10.0);
            } else if (kind == "adsb") {   // Mode S replies: short wideband bursts around 1090 MHz in some frames
                if (std::fmod(t * 7.3, 1.0) < 0.12) blk(0.0, 1.2e6, -92, 4);
            } else if (kind == "hf") {
                // AM broadcast at the centre: 2 x 4.5 kHz sidebands of voice/music; SSB voices; weak data signals
                const double am = voice(t, 0.3);
                const double ff = std::fabs(f);
                if (ff > 80 && ff < 4.5e3) p += std::pow(10.0, (-84 + 16 * am - 4 * ff / 4.5e3) / 10.0) * (0.6 + 0.8 * std::fabs(std::sin(ff / 330.0 + t * 2)));
                const double s1 = voice(t, 2.1), s2 = voice(t * 1.13, 4.2);
                if (f > -42e3 && f < -39.3e3) p += std::pow(10.0, (-92 + 18 * s1) / 10.0);
                if (f > 48e3 && f < 50.7e3) p += std::pow(10.0, (-95 + 16 * s2) / 10.0);
                if (std::fmod(t, 3.1) < 1.6) blk(-52e3, 2.4e3, -86, 12);   // an HFDL-like data burst
                blk(78e3, 3e3, -90, 10);
            } else {
                // a 10 kHz-wide data / OFDM block (ref44's signal) with fading and a speech-like SSB beside it
                const double fade = 0.65 + 0.35 * std::sin(2 * M_PI * 0.11 * t);
                if (f > -5.0e3 && f < 5.0e3) p += std::pow(10.0, (-66 + 10 * std::log10(fade) + 2.0 * std::sin(f / 300.0)) / 10.0);
                const double s1 = voice(t, 0.7);
                if (f > -11.5e3 && f < -8.0e3) p += std::pow(10.0, (-94 + 14 * s1 - 3 * std::fabs(f + 9.5e3) / 1.5e3) / 10.0);
                if (f > 6.0e3 && f < 8.6e3) p += std::pow(10.0, (-93 + 12 * voice(t * 0.9, 2.5)) / 10.0);
            }
            psd[k] = (float)p;
        }
    }
    void get(cf* out, size_t n, double t) override {
        // whole blocks of N from the frequency domain (n is a multiple of N here)
        double tt = t;
        for (size_t o = 0; o < n; o += N) {
            makePsd(tt);
            const float scale = (float)N * 0.5f * 1.155f;   // measured: the inverse FFT, the 1/N and the window make a bin read psd dBFS/bin
            for (size_t k = 0; k < N; k++) {
                const float a = std::sqrt(psd[k] * 0.5f) * scale;
                re[k] = a * nd(rng); im[k] = a * nd(rng);
            }
            dect2::fftSplit(re.data(), im.data(), log2n, true);
            for (size_t i = 0; i < N && o + i < n; i++) out[o + i] = cf(re[i], im[i]) / (float)N;
            for (Carrier& c : cw) {
                const double amp = std::pow(10.0, c.dbfs / 20.0) * 1.633;   // Hann's coherent gain (0.5) vs its noise power (0.375): a carrier's bin reads c.dbfs
                if (c.burstPeriod > 0 && std::fmod(tt, c.burstPeriod) > c.burstOn * c.burstPeriod) continue;
                for (size_t i = 0; i < N && o + i < n; i++) {
                    const double ts = tt + (double)i / fs;
                    double inst = c.fHz + c.drift * ts;
                    double a = amp;
                    if (c.fm == 1) inst += c.dev * (0.6 * std::sin(2 * M_PI * c.mrate * ts + c.mph) + 0.4 * std::sin(2 * M_PI * 3.1 * c.mrate * ts));
                    if (c.fm == 2) a *= 1.0;
                    c.ph += 2 * M_PI * inst / fs;
                    out[o + i] += cf((float)(a * std::cos(c.ph)), (float)(a * std::sin(c.ph)));
                }
                c.ph = std::fmod(c.ph, 2 * M_PI);
            }
            tt += (double)N / fs;
        }
    }
};

// ------------------------------------------------------------------------------------------------ main
static double cpuSec() { return (double)clock() / CLOCKS_PER_SEC; }

int main(int argc, char** argv) {
    std::string in, fmt = "cs8", gen, out = "spec.png";
    double fs = 0, fc = 0, seconds = 30, vlo = NAN, vhi = NAN, vfo = NAN, vbw = 0;
    int look = 1, mapArg = -1, W = 1400, Hh = 800, blocks = (int)dect2::SpectrumAnalyzer::kDefaultBlocks, fft = 0;
    bool old = false, bench = false, rough = false;
    float below = NAN, span = NAN, dbLo = NAN, dbHi = NAN, rowTau = NAN;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto nx = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--in") in = nx(); else if (a == "--fmt") fmt = nx(); else if (a == "--gen") gen = nx();
        else if (a == "--fs") fs = atof(nx()); else if (a == "--fc") fc = atof(nx()); else if (a == "--seconds") seconds = atof(nx());
        else if (a == "--view") { vlo = atof(nx()); vhi = atof(nx()); } else if (a == "--vfo") { vfo = atof(nx()); vbw = atof(nx()); }
        else if (a == "--look") look = atoi(nx()); else if (a == "--map") mapArg = atoi(nx()); else if (a == "--rough") rough = true; else if (a == "--w") W = atoi(nx()); else if (a == "--h") Hh = atoi(nx());
        else if (a == "--blocks") blocks = atoi(nx()); else if (a == "--fft") fft = atoi(nx()); else if (a == "--old") old = true;
        else if (a == "--bench") bench = true; else if (a == "--out") out = nx();
        else if (a == "--below") below = (float)atof(nx()); else if (a == "--rowtau") rowTau = (float)atof(nx()); else if (a == "--span") span = (float)atof(nx());
        else if (a == "--db") { dbLo = (float)atof(nx()); dbHi = (float)atof(nx()); }
        else { fprintf(stderr, "unknown %s\n", a.c_str()); return 2; }
    }
    if (old) blocks = 16;
    Source* src = nullptr;
    size_t N = fft > 0 ? (size_t)fft : 0;
    if (!gen.empty()) { const double gfs = gen == "wide" ? 20e6 : gen == "adsb" ? 4e6 : gen == "hf" ? 192e3 : 48e3; if (!N) N = old ? (gen == "wide" ? 32768 : 8192) : dect2::SpectrumAnalyzer::autoSize(gfs); src = new GenSource(gen, N); }
    else { if (in.empty() || fs <= 0) { fprintf(stderr, "--in and --fs, or --gen\n"); return 2; } src = new FileSource(in.c_str(), fmt, fs, fc); }
    fs = src->rate(); fc = src->centreMhz();
    if (!N) N = old ? (fs / 8192 > 250 ? (fs / 16384 > 250 ? 32768 : 16384) : 8192) : dect2::SpectrumAnalyzer::autoSize(fs);   // --old: the auto size before 3.0
    if (!std::isfinite(vlo)) { vlo = fc - fs / 2e6; vhi = fc + fs / 2e6; }
    if (!std::isfinite(vfo)) { vfo = (vlo + vhi) / 2; vbw = (vhi - vlo) * 0.12; }

    // ---- the pipeline, frame by frame at the engine's 30 frames a second
    dect2::SpectrumAnalyzer an(N);
    an.setBlocksPerFrame((size_t)blocks);
    Waterfall wf;
    const slook::LookSet& L = slook::lookSet(0);   // OnAir's one look; --look 2 / --map pick the palette
    const int mapSel = mapArg >= 0 ? mapArg : look == 2 ? cmap::BlueGreen : cmap::NavyHeat;
    wf.map = mapSel;
    float pb = 0, ps = 0; slook::floorPlace(mapSel, pb, ps);
    if (old) { wf.autoTrack = false; wf.minDb = -100; wf.maxDb = -32; }
    else { wf.autoTrack = true; wf.autoBelow = std::isfinite(below) ? below : pb; wf.autoSpan = std::isfinite(span) ? span : ps; }
    wf.palette();
    wf.viewU0 = (vlo - (fc - fs / 2e6)) / (fs / 1e6); wf.viewU1 = (vhi - (fc - fs / 2e6)) / (fs / 1e6);
    if (std::isfinite(rowTau)) wf.rowTau = rowTau;
    if (old) { wf.rowTau = 0; wf.viewU0 = 0; wf.viewU1 = 1; }
    const int frames = (int)std::lround(seconds * 30);
    const size_t perFrame = (size_t)std::lround(fs / 30.0);
    std::vector<cf> buf;
    dect2::SpectrumFrame f;
    std::vector<float> smooth, sview, pw;
    double tPipe = 0, tSrc = 0;
    const bool isGen = !gen.empty();
    for (int fr = 0; fr < frames; fr++) {
        const double t = fr / 30.0;
        // a generator only makes the blocks the analyser transforms (the rest of a frame's samples would only feed its statistics)
        const size_t n = isGen ? (size_t)blocks * N : perFrame;
        buf.resize(n);
        const double s0 = cpuSec();
        src->get(buf.data(), n, t);
        const double s1 = cpuSec();
        an.feed(reinterpret_cast<const dect2::cf32*>(buf.data()), n);
        if (!an.takeFrame(f)) continue;
        // as ingestSpectrum: per Hz, the trace averaged in power with the time constant the bins per pixel ask for (--old: per bin, dB average)
        const size_t nb = f.dbfs.size();
        const float off = old ? 0.f : spipe::perHzOffset(fs, nb);
        sview.resize(nb);
        for (size_t i = 0; i < nb; i++) sview[i] = f.dbfs[i] - off;
        if (smooth.size() != nb) { smooth = sview; pw.clear(); }
        const double bpp = std::max(1.0, (double)nb * (vhi - vlo) / (fs / 1e6) / std::max(1, W));
        if (old) spipe::emaStep(smooth.data(), sview.data(), nb, 1.f / 30, 0.08f);
        else spipe::powerEma(pw, sview.data(), smooth.data(), nb, spipe::alphaFor(1.f / 30, spipe::traceTau(0.08f, bpp)));
        wf.setCols(specaxis::historyCols(old ? 0 : -1, nb, Waterfall::kMaxCols));
        wf.offer(sview, t);
        wf.stamp(t);
        if (wf.autoTrack && t - wf.trackT > 0.25) { wf.fitRange(smooth, wf.trackT == 0, 0.25f); wf.trackT = t; }
        tPipe += cpuSec() - s1; tSrc += s1 - s0;
    }
    if (rough && !smooth.empty()) {   // the trace at W pixel columns: its roughness (mean |neighbour difference|) and its floor, by numbers
        std::vector<float> col;
        const double dx = fs / 1e6 / (double)smooth.size(), x0 = fc - fs / 2e6;
        slook::decimate(smooth.data(), smooth.size(), x0, dx, vlo, vhi, W, L.interp, false, col);
        std::vector<float> c2(col), scr; const float fl = spipe::noiseFloor(c2.data(), c2.size(), scr);
        std::vector<float> quiet; for (float v : col) if (v < fl + 6) quiet.push_back(v);   // the noise floor's columns only
        printf("rough: fft %zu bins/px %.1f trace roughness %.2f dB (floor columns %.2f dB) floor %.1f dB%s\n", N, (double)smooth.size() * (vhi - vlo) / (fs / 1e6) / W,
               spipe::roughness(col.data(), col.size()), spipe::roughness(quiet.data(), quiet.size()), fl, old ? "/bin" : "/Hz");
    }
    if (bench) printf("bench: %s pipeline %.3f s CPU for %.1f s of signal (%.1f %% of one core), source %.2f s\n", old ? "old" : "new", tPipe, seconds, 100 * tPipe / seconds, tSrc);

    // ---- the picture: the app's plots with the look
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2((float)W, (float)Hh);
    io.DeltaTime = 1.f / 60;
    io.IniFilename = nullptr;
    const char* fontPath = "third_party/fonts/JetBrainsMono-Regular.ttf";
    ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, 13.0f);
    if (!font) { fprintf(stderr, "font %s missing (run from the repository root)\n", fontPath); return 2; }
    ImGui::StyleColorsDark();
    ImgStore view(Waterfall::W, Waterfall::H, 0xFF000000u);
    float yLo = dbLo, yHi = dbHi;
    if (!std::isfinite(yLo)) {   // the spectrum's dB range: as the user would set it (the references: the floor at the bottom / a fifth up)
        float fl = wf.floorTrack.floor;
        if (!std::isfinite(fl)) { std::vector<float> s; fl = spipe::noiseFloor(smooth.data(), smooth.size(), s); }
        yLo = fl - L.specBelow; yHi = yLo + 80;   // as the app does with specAutoDb
    }
    for (int pass = 0; pass < 3; pass++) {   // a few frames: the plots settle their ticks and the view texture
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        const float specH = std::round(Hh * 0.42f);
        const double dx = fs / 1e6 / (double)smooth.size(), x0 = fc - fs / 2e6;
        // spectrum
        plt::SetNextPlotLook(spaint::plotLook(L, true, true, font, std::round(13.f * L.labelScale), L.labelMode, vfo));
        if (plt::BeginPlot("##spec", ImVec2((float)W, specH), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Overlay | plt::Flags_Flush | plt::Flags_OpenBottom)) {
            plt::SetupAxes(nullptr, nullptr, 0, plt::AxisFlags_Inside);
            plt::SetupAxisLimits(plt::X1, vlo, vhi, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, yLo, yHi, plt::Cond_Always);
            if (!L.classic) spaint::band(L, vfo - vbw / 2, vfo + vbw / 2, vfo, true, false);
            const plt::Rect vis = plt::GetPlotLimits();
            const int px = std::max(1, (int)plt::GetPlotSize().x);
            std::vector<double> ex, ey;
            spaint::traceXY(smooth.data(), smooth.size(), x0, dx, vis, px, L.interp, false, L.smoothPasses, ex, ey);
            const float* tc = cmap::traceRgb(L.classic ? 0 : L.tracePreset, false);
            spaint::trace(ex, ey, ImVec4(tc[0], tc[1], tc[2], 1), L.classic ? 1.2f : L.traceWeight, L.fill || L.classic, L.fill ? L.fillTop : 0.18f, L.fill ? L.fillBottom : 0.03f, vis.Y.Min);
            plt::EndPlot();
        }
        // waterfall
        plt::SetNextPlotLook(spaint::plotLook(L, false, true, font, std::round(13.f * L.labelScale), L.labelMode, vfo));
        const int H = Waterfall::H;
        if (plt::BeginPlot("##wf", ImVec2((float)W, Hh - specH), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Overlay | plt::Flags_Flush | plt::Flags_OpenTop)) {
            plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
            plt::SetupAxisLimits(plt::X1, vlo, vhi, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, -H, 0, plt::Cond_Always);
            const ImVec2 psz = plt::GetPlotSize();
            const plt::Rect vis = plt::GetPlotLimits();
            const double wx0 = specaxis::imageLo(fc, fs / 1e6, smooth.size()), wx1 = wx0 + fs / 1e6;
            int c0 = 0, c1 = wf.cols;
            specaxis::visibleCols(vis.X.Min, vis.X.Max, wx0, fs / 1e6, wf.cols, c0, c1);
            const int k = plt::WfView::groupRows(vis.Y.Max - vis.Y.Min, psz.y, H);
            static std::vector<int> dirty; bool full = false;
            wf.view.mode = (!L.classic && L.wfSmooth ? plt::WfView::Smooth : 0);
            if (wf.view.update(wf.lv.data(), wf.cols, H, wf.writeRow, wf.rows - 1, wf.filled, wf.lut, wf.lutGen, c0, c1,
                               std::max(1, std::min(Waterfall::W - 1, (int)psz.x)), k, full, dirty)) {
                if (full) view.update(0, 0, wf.view.stride, wf.view.rowsAlloc, wf.view.px.data());
                else for (int r : dirty) view.update(0, r, wf.view.stride, 1, wf.view.px.data() + (size_t)r * wf.view.stride);
            }
            spaint::wfImage(wf.view, view.texture(), Waterfall::W, H, wx0, wx1, wf.cols, wf.rows, 0.0);
            if (!L.classic) spaint::band(L, vfo - vbw / 2, vfo + vbw / 2, vfo, false, false);
            plt::EndPlot();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::Render();
        if (pass < 2) { Canvas tmp(1, 1); rasterise(tmp, ImGui::GetDrawData()); continue; }   // textures created
        Canvas cv(W, Hh);
        rasterise(cv, ImGui::GetDrawData());
        std::vector<uint8_t> rgba((size_t)W * Hh * 4);
        for (size_t i = 0; i < (size_t)W * Hh; i++) {
            rgba[4 * i] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, cv.r[i])) * 255);
            rgba[4 * i + 1] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, cv.g[i])) * 255);
            rgba[4 * i + 2] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, cv.b[i])) * 255);
            rgba[4 * i + 3] = 255;
        }
        gfx::writePng(out.c_str(), rgba.data(), W, Hh);
    }
    printf("wrote %s: fs %.0f, fft %zu, blocks %d, rows %lld, wf range %.1f..%.1f dB, floor %.1f, spectrum %.0f..%.0f dB\n", out.c_str(), fs, N, blocks,
           wf.rows, wf.minDb, wf.maxDb, wf.floorTrack.floor, yLo, yHi);
    ImGui::DestroyContext();
    return 0;
}
