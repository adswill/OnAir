// SSTV test signal: the test picture sent in one of the modes (with the VIS header unless asked otherwise), as audio for the HF digital
// generator (hfdig_gen.h), and the mode table's public accessors. Nothing is transmitted.
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "hfdig_sstv_modes.h"
#include "dect2/source/synth_media.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

int sstvModeCount() { return (int)sstv::table().size(); }
const SstvModeInfo& sstvModeInfo(int index) {
    const auto& t = sstv::table();
    return t[(size_t)std::max(0, std::min((int)t.size() - 1, index))].info;
}

// colour bars on top, a grey gradient, then "OnAir" in blocks on dark blue; all edges on even pixels and rows, in cells of 8 (or 16) pixels
void sstvStampText(int w, int h, std::vector<uint8_t>& rgb, const std::string& text) {
    if (w <= 0 || h <= 0 || (int)rgb.size() < w * h * 3 || text.empty()) return;
    auto glyph = [](char ch, int row) -> unsigned {
        const char u = (char)std::toupper((unsigned char)ch);
        // 5-bit rows, bit 4 = left. Space is blank.
        static const unsigned A[][7] = {
            {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},
            {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
            {0x0F,0x10,0x10,0x13,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
            {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
            {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
            {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
            {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
            {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
            {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
        };
        static const unsigned D[][7] = {
            {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
            {0x1F,0x01,0x02,0x06,0x01,0x11,0x0E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
            {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
            {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}
        };
        if (row < 0 || row > 6) return 0;
        if (u >= 'A' && u <= 'Z') return A[u - 'A'][row];
        if (u >= '0' && u <= '9') return D[u - '0'][row];
        if (u == '-') return row == 3 ? 0x1F : 0;
        if (u == '/') return (unsigned)(1 << (4 - row / 2));
        return 0;
    };
    const int cw = std::max(6, w / 24), ch = std::max(6, h / 28);
    const int n = std::min((int)text.size(), w / (cw + 2));
    const int x0 = (w - n * (cw + 2)) / 2, y0 = std::max(0, h - ch * 7 - 2);
    for (int i = 0; i < n; i++) for (int row = 0; row < 7; row++) {
        const unsigned bits = glyph(text[(size_t)i], row);
        for (int col = 0; col < 5; col++) {
            if (((bits >> (4 - col)) & 1) == 0) continue;
            for (int dy = 0; dy < ch; dy++) for (int dx = 0; dx < cw / 5 + 1; dx++) {
                const int x = x0 + i * (cw + 2) + col * (cw / 5) + dx;
                const int y = y0 + row * ch + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                uint8_t* p = &rgb[((size_t)y * w + x) * 3];
                p[0] = p[1] = p[2] = 255;
            }
        }
    }
}

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
    explicit SstvTestAudio(const SynthConfig& c)
        : SstvTestAudio(c.modeOpt[1], mediaPicture(c), c.modeOpt[2] == 0, c.modeVal[0], c.sroPpm) {}
    // Synthetic content: the user's picture, fitted into the size of the chosen SSTV mode (empty: the built-in test picture)
    static std::vector<uint8_t> mediaPicture(const SynthConfig& c) {
        if (c.mediaKind == 0 || c.mediaPath.empty()) return {};
        const auto& tab = table();
        const auto& sp = tab[(size_t)std::max(0, std::min((int)tab.size() - 1, c.modeOpt[1]))];
        return synthMediaPicture(c, sp.info.width, sp.info.height);
    }
    SstvTestAudio(int modeIdx, const std::vector<uint8_t>& rgbIn, bool withVis = true, double offHz = 0, double sroPpm = 0) {
        const auto& tab = table();
        sp_ = &tab[(size_t)std::max(0, std::min((int)tab.size() - 1, modeIdx))];
        withVis_ = withVis;
        offHz_ = offHz;
        scale_ = (1.0 - sroPpm * 1e-6) / 8.0;     // ms per sample
        const int w = sp_->info.width, h = sp_->info.height;
        if ((int)rgbIn.size() == w * h * 3) rgb_ = rgbIn;
        else sstvTestPicture(w, h, rgb_);
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
            ph_ += 2 * dect2::math::kPi * (f + offHz_) / 8000.0;
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
std::unique_ptr<HfdigTestAudio> makeSstvTx(int modeIdx, const std::vector<uint8_t>& rgb) {
    return std::make_unique<SstvTestAudio>(modeIdx, rgb);
}

} // namespace dect2
