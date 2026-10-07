// The analog TV test card: EBU colour bars, grey ramp and staircase, multiburst, resolution wedge, checkerboard, a circle, the text
// ONAIR TEST, a moving box, a running clock and a frame counter. Drawn at the picture size of the format (768 x 576 or 640 x 480) as
// luma and colour-difference planes (E'Y, E'U, E'V in the units of BT.470: Y 0..1, U and V the weighted differences).
// The picture is band-limited the way a 5 MHz video chain would: the generator can send it as it is.
#pragma once
#include "dect2/atv_std.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct AtvMbPacket { double startUs, endUs, mhz; };   // a burst of sine in the luminance, drawn by the generator at the sample rate

class AtvCard {
public:
    // pattern: 0 the test card, 1 EBU colour bars only, 2 grey ramp only
    AtvCard(const AtvFormat& f, int pattern = 0);
    int width() const { return w_; }
    int height() const { return h_; }
    // The visible lines of one field (0 = first, 1 = second) at time t seconds: fieldRows rows of width() floats in each plane
    void renderField(int field, double t, uint64_t frameNo, float* y, float* u, float* v) const;
    // Both fields as one frame (rows interleaved), as RGBA8; field 0 is rendered at time t, field 1 one field later (what a camera does)
    void referenceFrame(double t, uint64_t frameNo, std::vector<uint8_t>& rgba) const;
    // multiburst: the frame rows it occupies and its packets (the picture holds a flat 0.5 there)
    bool inMultiburst(int frameRow) const { return mbRow0_ >= 0 && frameRow >= mbRow0_ && frameRow < mbRow1_; }
    const std::vector<AtvMbPacket>& multiburst() const { return mb_; }
    // where the colour bars are (frame rows and columns), for tests
    void barsArea(int& x0, int& x1, int& y0, int& y1) const { x0 = barX0_; x1 = barX1_; y0 = barY0_; y1 = barY1_; }
    void rampArea(int& x0, int& x1, int& y0, int& y1) const { x0 = rampX0_; x1 = rampX1_; y0 = rampY0_; y1 = rampY1_; }

private:
    struct Canvas;
    void buildStatic();
    void drawDynamic(Canvas& c, double t, uint64_t frameNo) const;
    AtvFormat f_;
    int pattern_;
    int w_, h_;
    std::vector<float> sy_, su_, sv_;      // the static picture, h_ x w_
    std::vector<float> lpY_, lpC_;         // the video-band filters used on the dynamic parts
    int mbRow0_ = -1, mbRow1_ = -1;
    std::vector<AtvMbPacket> mb_;
    int barX0_ = 0, barX1_ = 0, barY0_ = 0, barY1_ = 0, rampX0_ = 0, rampX1_ = 0, rampY0_ = 0, rampY1_ = 0;
    // the moving box track and the clock strip (frame coordinates)
    int trackX0_ = 0, trackX1_ = 0, trackY0_ = 0, trackY1_ = 0, clockY0_ = 0, clockY1_ = 0, boxSize_ = 0;
    int textScale_ = 3;
    std::string label_;
};

} // namespace dect2
