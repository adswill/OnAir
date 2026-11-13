// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// See audio_if.h. The shift is done on the analytic signal so the other sideband stays suppressed: shifting the real samples would
// fold the image on top of the wanted signal.
#include "dect2/source/audio_if.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace dect2 {

namespace { std::atomic<float> gLevel{0}; std::atomic<bool> gClip{false}; std::atomic<int> gLayout{0}; }

AudioIfConfig audioIfConfigOf(const TuneSettings& s) {
    AudioIfConfig c;
    const int m = (int)radioNumber(s, "audio_iq", 0);
    c.mode = (m >= 0 && m <= 3) ? (AudioIqMode)m : AudioIqMode::StereoIq;
    c.chan = std::max(0, std::min(2, (int)radioNumber(s, "audio_chan", 0)));
    c.rate = std::max(8000, std::min(192000, (int)radioNumber(s, "audio_rate", 48000)));
    c.ifHz = std::max(-0.5 * c.rate, std::min(0.5 * c.rate, radioNumber(s, "audio_if_hz", 0)));   // beyond Nyquist means nothing
    c.invert = radioNumber(s, "audio_invert", 0) != 0;
    c.gainDb = (float)std::max(-60.0, std::min(60.0, radioNumber(s, "audio_gain_db", 0)));
    c.dc = radioNumber(s, "audio_dc", 1) != 0;
    c.minOutRate = radioNumber(s, "audio_min_rate", 0);
    return c;
}

int audioOutFactor(double rate, double minOut) {
    if (rate <= 0 || minOut <= rate) return 1;
    const int f = (int)std::ceil(minOut / rate - 1e-9);
    return rate * f > 1.5e6 ? 1 : f;
}

const int* audioRateChoices(int& n) {
    static const int r[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
    n = (int)(sizeof r / sizeof r[0]);
    return r;
}

float audioSourceLevel() { return gLevel.load(); }
bool audioSourceClipping() { return gClip.load(); }
int audioSourceLayoutHint() { return gLayout.load(); }

double audioAfViewOffsetHz(const AudioIfConfig& c) {
    if (c.mode == AudioIqMode::StereoIq) return 0;
    const double d = 0.25 * c.rate - c.ifHz;   // the card's 0..Nyquist range is centred at rate/4; the IF centre goes to 0 Hz
    const bool conj = (c.mode == AudioIqMode::Lsb) != c.invert;
    return conj ? -d : d;
}

bool audioDetectCentre(const float* b, int n, double rate, double viewHz, const AudioIfConfig& cfg, double& centreHz, double& widthHz) {
    if (n < 16) return false;
    // the occupied part: bins within 12 dB of the strongest, after a 3-bin smooth; the noise floor is the median
    std::vector<float> s((size_t)n), t((size_t)n);
    for (int i = 0; i < n; i++) { const int a = std::max(0, i - 1), c = std::min(n - 1, i + 1); s[(size_t)i] = (b[a] + b[i] + b[c]) / 3.f; }
    t = s;
    std::nth_element(t.begin(), t.begin() + n / 2, t.end());
    const float floorDb = t[(size_t)n / 2];
    int pk = 0;
    for (int i = 0; i < n; i++) if (s[(size_t)i] > s[(size_t)pk]) pk = i;
    if (s[(size_t)pk] < floorDb + 10.f) return false;
    const float thr = std::max(floorDb + 6.f, s[(size_t)pk] - 12.f);
    int lo = pk, hi = pk;
    const int gap = std::max(2, n / 64);   // tones of a two-tone signal are bridged
    for (int i = pk, miss = 0; i >= 0; i--) { if (s[(size_t)i] >= thr) { lo = i; miss = 0; } else if (++miss > gap) break; }
    for (int i = pk, miss = 0; i < n; i++) { if (s[(size_t)i] >= thr) { hi = i; miss = 0; } else if (++miss > gap) break; }
    const double binHz = rate / n;
    const double fa = ((lo + hi) * 0.5 - n / 2) * binHz + viewHz;   // the signal centre as the decoders see it, from the dial
    widthHz = (hi - lo + 1) * binHz;
    const bool conj = cfg.mode == AudioIqMode::StereoIq ? cfg.invert : ((cfg.mode == AudioIqMode::Lsb) != cfg.invert);
    centreHz = conj ? cfg.ifHz - fa : cfg.ifHz + fa;
    return true;
}

void audioIfProcess(const float* stereo, int n, const AudioIfConfig& cfg, cf32* out, AudioIfState& st) {
    if (n <= 0) return;
    const float g = std::pow(10.f, cfg.gainDb / 20.f);
    const bool real = cfg.mode != AudioIqMode::StereoIq;
    std::vector<float> buf((size_t)n * 2);
    const float a = 1.f - std::exp(-2.f * (float)dect2::math::kPi * 20.f / (float)cfg.rate);   // 20 Hz corner: removes the card's offset, leaves a 100 Hz tone alone
    float pk = 0;
    const double dec = std::exp(-1.0 / cfg.rate);   // the layout statistics fade over about a second
    for (int i = 0; i < n; i++) {
        float l = stereo[2 * i] * g, r = stereo[2 * i + 1] * g;
        pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
        if (std::fabs(l) >= 0.995f || std::fabs(r) >= 0.995f) st.clipFrame = st.frames + 1;
        st.sLL = st.sLL * dec + (double)l * l; st.sRR = st.sRR * dec + (double)r * r; st.sLR = st.sLR * dec + (double)l * r;
        st.frames++;
        if (cfg.dc) { st.dcL += a * (l - st.dcL); st.dcR += a * (r - st.dcR); l -= st.dcL; r -= st.dcR; }
        if (real) {   // a real signal: one channel (or both averaged) into the Hilbert stage, which reads the left slot
            l = cfg.chan == 0 ? l : cfg.chan == 1 ? r : 0.5f * (l + r);
            r = 0;
        }
        buf[2 * (size_t)i] = l; buf[2 * (size_t)i + 1] = r;
    }
    st.peak = std::max(pk, st.peak * 0.9f);
    gLevel.store(st.peak);
    gClip.store(st.clipFrame != 0 && st.frames - st.clipFrame < (uint64_t)(2 * cfg.rate));
    {   // L and R the same signal: |correlation| near 1 (the two channels of an audio feed), against two different ones (I and Q of a pair)
        const double den = std::sqrt(st.sLL * st.sRR);
        gLayout.store(den < 1e-9 * cfg.rate ? 0 : std::fabs(st.sLR) / den > 0.98 ? 1 : 2);
    }
    // Real (mono) is analytic like USB/LSB: with Q = 0 it would hold the signal and its mirror image, which no decoder wants.
    // The sideband (LSB, or the invert switch) is applied after the shift so the IF lands on 0 Hz either way.
    const bool lsb = cfg.mode == AudioIqMode::Lsb;
    if (real) audioStereoToIq(buf.data(), n, AudioIqMode::Usb, out, st.hil);
    else for (int i = 0; i < n; i++) out[i] = cf32(buf[2 * (size_t)i], buf[2 * (size_t)i + 1]);
    const bool conj = real ? (lsb != cfg.invert) : cfg.invert;
    if (cfg.ifHz == 0 && !conj) return;
    const double w = -2 * dect2::math::kPi * cfg.ifHz / cfg.rate;
    for (int i = 0; i < n; i++) {
        const cf32 v = out[i] * cf32((float)std::cos(st.phase), (float)std::sin(st.phase));
        out[i] = conj ? std::conj(v) : v;
        st.phase += w;
        if (st.phase > dect2::math::kPi) st.phase -= 2 * dect2::math::kPi;
        else if (st.phase < -dect2::math::kPi) st.phase += 2 * dect2::math::kPi;
    }
}

} // namespace dect2
