// SSTV test signal: the test picture sent in one of the modes (with the VIS header unless asked otherwise), as audio for the HF digital
// generator (hfdig_gen.h), and the mode table's public accessors. Nothing is transmitted.
#include "dect2/hfdig_sstv.h"
#include "dect2/hfdig_gen.h"
#include "hfdig_sstv_modes.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

int sstvModeCount() { return (int)sstv::table().size(); }
const SstvModeInfo& sstvModeInfo(int index) {
    const auto& t = sstv::table();
    return t[(size_t)std::max(0, std::min((int)t.size() - 1, index))].info;
}

// colour bars on top, a grey gradient, then "OnAir" in blocks on dark blue; all edges on even pixels and rows, in cells of 8 (or 16) pixels
void sstvTestPicture(int w, int h, std::vector<uint8_t>& rgb) {
    rgb.assign((size_t)w * h * 3, 0);
    static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0}};
    static const char* const glyph[5][6] = {
        {".###.", "#...#", "#...#", "#...#", "#...#", ".###."},   // O
        {"#.##.", "##..#", "#...#", "#...#", "#...#", "#...#"},   // n
        {".###.", "#...#", "#####", "#...#", "#...#", "#...#"},   // A
        {"..#..", ".....", "..#..", "..#..", "..#..", "..#.."},   // i
        {"#.##.", "##..#", "#....", "#....", "#....", "#...."}};  // r
    const int cell = h >= 400 ? 16 : 8;
    const int yGrad = (h * 3 / 5) / 8 * 8, yText = (h * 4 / 5) / cell * cell;
    const int cw = w / 40, x0 = (w - 30 * cw) / 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* p = &rgb[((size_t)y * w + x) * 3];
            if (y < yGrad) {
                const uint8_t* b = bars[x * 8 / w];
                p[0] = b[0]; p[1] = b[1]; p[2] = b[2];
            } else if (y < yText) {
                p[0] = p[1] = p[2] = (uint8_t)(x * 255 / (w - 1));
            } else {
                p[0] = 0; p[1] = 0; p[2] = 96;
                const int gy = (y - yText) / cell, gx = (x - x0) / cw;
                if (x >= x0 && gy < 6 && gx >= 0 && gx < 30 && gx % 6 < 5) {
                    if (glyph[gx / 6][gy][gx % 6] == '#') p[0] = p[1] = p[2] = 255;
                }
            }
        }
    }
}

namespace {

using namespace sstv;

class SstvTestAudio : public HfdigTestAudio {
public:
    explicit SstvTestAudio(const SynthConfig& c) {
        const auto& tab = table();
        sp_ = &tab[(size_t)std::max(0, std::min((int)tab.size() - 1, c.modeOpt[1]))];
        withVis_ = c.modeOpt[2] == 0;
        offHz_ = c.modeVal[0];
        scale_ = (1.0 - c.sroPpm * 1e-6) / 8.0;     // ms per sample
        const int w = sp_->info.width, h = sp_->info.height;
        sstvTestPicture(w, h, rgb_);
        y_.resize((size_t)w * h); ry_.resize(y_.size()); by_.resize(y_.size());
        for (size_t i = 0; i < y_.size(); i++) rgbToYuv(rgb_[i * 3], rgb_[i * 3 + 1], rgb_[i * 3 + 2], y_[i], ry_[i], by_[i]);
        visMs_ = withVis_ ? 910.0 : 0.0;
        imgMs_ = sp_->totalLines * sp_->info.lineMs;
        cycleMs_ = kLeadMs + visMs_ + imgMs_ + kTailMs;
    }
    void generate(float* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            double t = (double)pos_ * scale_;
            pos_++;
            if (t >= cycleMs_) { pos_ = 1; t = 0; }
            const double f = freqAt(t);
            if (f <= 0) { out[i] = 0.f; continue; }
            ph_ += 2 * 3.14159265358979323846 * (f + offHz_) / 8000.0;
            if (ph_ > 6.283185307179586) ph_ -= 6.283185307179586;
            out[i] = 0.5f * (float)std::sin(ph_);
        }
    }

private:
    static constexpr double kLeadMs = 400, kTailMs = 1500;

    double visFreq(double t) const {
        if (t < 300) return 1900;
        if (t < 310) return 1200;
        if (t < 610) return 1900;
        const int j = (int)((t - 610) / 30);
        const int code = sp_->info.vis;
        if (j == 0 || j == 9) return 1200;
        const int bit = j <= 7 ? (code >> (j - 1)) & 1 : __builtin_popcount((unsigned)code) & 1;   // even parity
        return bit ? 1100 : 1300;
    }
    static double hzOf(double v) { return 1500.0 + 800.0 * v / 255.0; }
    double value(const Scan& s, int line, int px) const {
        const int w = sp_->info.width;
        auto at = [&](const std::vector<double>& pl, int x, int y) { return pl[(size_t)y * w + x]; };
        switch (sp_->kind) {
        case Kind::Seq: return rgb_[((size_t)line * w + px) * 3 + (s.comp == kR ? 0 : s.comp == kG ? 1 : 2)];
        case Kind::Robot36:
            if (s.comp == kY) return at(y_, px, line);
            else {
                const std::vector<double>& pl = (line & 1) ? by_ : ry_;
                const int y0 = line & ~1;
                return 0.25 * (at(pl, 2 * px, y0) + at(pl, 2 * px + 1, y0) + at(pl, 2 * px, y0 + 1) + at(pl, 2 * px + 1, y0 + 1));
            }
        case Kind::Robot72:
            if (s.comp == kY) return at(y_, px, line);
            else {
                const std::vector<double>& pl = s.comp == kRY ? ry_ : by_;
                return 0.5 * (at(pl, 2 * px, line) + at(pl, 2 * px + 1, line));
            }
        case Kind::PD:
            if (s.comp == kY) return at(y_, px, 2 * line);
            if (s.comp == kY2) return at(y_, px, 2 * line + 1);
            { const std::vector<double>& pl = s.comp == kRY ? ry_ : by_; return 0.5 * (at(pl, px, 2 * line) + at(pl, px, 2 * line + 1)); }
        }
        return 0;
    }
    double lineFreq(int line, double t) const {
        if (t >= sp_->syncOffMs && t < sp_->syncOffMs + sp_->info.syncMs) return 1200;
        for (const Scan& s : sp_->scans) {
            if (t >= s.startMs && t < s.startMs + s.lenMs) {
                const int px = std::min(s.px - 1, (int)((t - s.startMs) / s.lenMs * s.px));
                return hzOf(value(s, line, px));
            }
        }
        for (const Fixed& f : sp_->fixed) {
            if (t >= f.startMs && t < f.startMs + f.lenMs) return f.hz < 0 ? ((line & 1) ? 2300 : 1500) : f.hz;
        }
        return 1500;
    }
    double freqAt(double t) const {
        t -= kLeadMs;
        if (t < 0) return 0;
        if (t < visMs_) return visFreq(t);
        t -= visMs_;
        if (t >= imgMs_) return 0;
        const int line = std::min(sp_->totalLines - 1, (int)(t / sp_->info.lineMs));
        return lineFreq(line, t - line * sp_->info.lineMs);
    }

    const Spec* sp_ = nullptr;
    bool withVis_ = true;
    double offHz_ = 0, scale_ = 0.125, visMs_ = 0, imgMs_ = 0, cycleMs_ = 0, ph_ = 0;
    uint64_t pos_ = 0;
    std::vector<uint8_t> rgb_;
    std::vector<double> y_, ry_, by_;
};

} // namespace

std::unique_ptr<HfdigTestAudio> makeSstvTestAudio(const SynthConfig& cfg) { return std::make_unique<SstvTestAudio>(cfg); }

} // namespace dect2
