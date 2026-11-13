// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital front end demodulators: a Robot 36 SSTV picture sent in narrow FM (as the ISS sends on 145.800 MHz: 3 kHz deviation, a
// Doppler offset of +1.5 kHz, no pre-emphasis) decodes with NFM; the same picture in upper sideband still decodes with USB (the default);
// Auto gives USB below 30 MHz and NFM at and above it.
#include "dect2/audio/hf_demod.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/engine/mode_synth.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;
#include "../common/test_util.h"

static constexpr double kRate = 1e6, kPi = dect2::math::kPi;

struct Noise {
    uint64_t s = 777;
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(2 * kPi * u()); }
};

// Robot 36 test audio, FM-modulated at the dial (-tuneOffsetHz in the samples) plus cfo; deviation in Hz per unit of audio
class FmSstv : public ModeSynth {
public:
    FmSstv(double devPerUnit, double cfo) : dev_(devPerUnit), cfo_(cfo) {
        SynthConfig c; c.modeOpt[1] = 0;
        audio_ = makeSstvTestAudio(c);
    }
    double sampleRate() const override { return kRate; }
    void generate(cf32* out, size_t n) override {
        const int up = (int)(kRate / 8000);
        const double fc = cfo_ - hfdigTuning().tuneOffsetHz;
        for (size_t i = 0; i < n; i++) {
            if (k_ == 0) { a0_ = a1_; audio_->generate(&a1_, 1); }
            const double a = a0_ + (a1_ - a0_) * k_ / up;   // linear interpolation to the radio rate
            if (++k_ >= up) k_ = 0;
            ph_ += 2 * kPi * (fc + dev_ * a) / kRate;
            if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
            out[i] = cf32((float)(0.3 * std::cos(ph_) + 0.01 * nz_.g()), (float)(0.3 * std::sin(ph_) + 0.01 * nz_.g()));
        }
    }
private:
    std::unique_ptr<HfdigTestAudio> audio_;
    double dev_, cfo_, ph_ = 0;
    float a0_ = 0, a1_ = 0;
    int k_ = 0;
    Noise nz_;
};

// feeds the receiver until an SSTV picture is done; returns its telemetry
static HfdigSstvTelemetry run(ModeSynth& src, int demod, double maxSec) {
    HfdigReceiver r;
    r.setSilent(true);
    r.setDecoders(2);
    r.setDemod(demod);
    r.configure(kRate);
    r.setSignalOffset(-hfdigTuning().tuneOffsetHz);
    std::vector<cf32> buf(100000);
    HfdigTelemetry t;
    uint64_t seq = 0;
    for (int blk = 0; blk < (int)(maxSec * kRate / buf.size()); blk++) {
        src.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        if (r.telemetry(t, seq)) { seq = t.seq; if (t.sstv.picturesDone >= 1) break; }
    }
    r.telemetry(t, 0);
    return t.sstv;
}

static double mae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255;
    double s = 0;
    for (size_t i = 0; i < a.size(); i++) s += std::abs((int)a[i] - (int)b[i]);
    return s / (double)a.size();
}

static void check(const char* what, const HfdigSstvTelemetry& s) {
    std::vector<uint8_t> ref;
    sstvTestPicture(320, 240, ref);
    const double e = s.image ? mae(s.image->rgb, ref) : 255;
    printf("%-22s '%s' lines %d done %d, mean error %.2f\n", what, s.mode.c_str(), s.lines, (int)s.picturesDone, e);
    CHECK(s.picturesDone >= 1 && s.mode == "Robot 36" && s.lines == 240, "%s: done %d mode '%s' lines %d", what, (int)s.picturesDone, s.mode.c_str(), s.lines);
    CHECK(e < 20, "%s: mean error %.2f", what, e);
}

int main() {
    CHECK(hfDemodResolve(HfDemodAuto, 14.230) == HfDemodUsb, "auto at 14.230 MHz");
    CHECK(hfDemodResolve(HfDemodAuto, 145.800) == HfDemodNfm, "auto at 145.800 MHz");
    CHECK(hfDemodResolve(HfDemodAuto, 30.0) == HfDemodNfm, "auto at 30 MHz");
    CHECK(hfDemodResolve(HfDemodLsb, 145.8) == HfDemodLsb, "explicit LSB kept");
    {   // ISS-style FM: the test audio peaks near 0.5, so 6 kHz per unit is about 3 kHz deviation
        FmSstv fm(6000, 1500);
        check("NFM, Robot 36 in FM", run(fm, HfDemodNfm, 45));
    }
    {   // the same through Auto at a VHF dial resolves to NFM
        FmSstv fm(6000, -1500);
        check("Auto@145.8, FM", run(fm, hfDemodResolve(HfDemodAuto, 145.8), 45));
    }
    {   // upper sideband (the HF test signal) with the default demodulator
        SynthConfig c;
        c.modeOpt[0] = 1; c.modeOpt[1] = 0; c.snrDb = 25;
        auto usb = makeHfdigSynth(c, kRate);
        check("USB, Robot 36 in SSB", run(*usb, HfDemodUsb, 45));
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
