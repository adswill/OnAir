// The radio side of AX.25 packet, shared by the APRS / Packet receiver (packet_rx.cpp) and the CubeSat telemetry receiver (cubesat_rx.cpp):
// an FM front end (mix out the signal offset, decimate to about 100 kHz, resample to exactly 96 kHz, AFC, channel filter, discriminator,
// carrier and SNR from 20 ms power blocks), the 1200 baud Bell 202 AFSK demodulator and the direct FSK slicer of G3RUH 9600 baud (and of
// other plain 2-FSK / GMSK speeds, with or without the descrambler).
#pragma once
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "packet_ax25.h"
#include "dect2/common/ring.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace pktdemod {

constexpr double kMidRate = 96000.0;           // the front end's output rate
constexpr double kAudioRate = 48000.0;         // the AFSK demodulators' rate
constexpr size_t kBlock = 1920;                // power blocks of 20 ms at 96 kHz

// FM front end. process() gives the discriminator in Hz at 96 kHz; commit() afterwards moves the sample clock n96 on (the demodulators
// running on the output in between still see the clock of the block's start, as their callbacks expect).
struct FmFront {
    // pass / stop: the channel filter (Hz); clipHz: FM clicks are cut to this deviation; afcMaxHz: how far the AFC follows a carrier
    double chanPass = 9000.0, chanStop = 17000.0;
    // optional second, narrower channel with its own discriminator (disc2) on the same signal: the 1200 baud tones need much less than the
    // 9600 baud data, and a narrower IF filter is worth 2-3 dB at the threshold. 0: off (disc2 is then the wide discriminator)
    double chanPass2 = 0, chanStop2 = 0;
    // optional third channel with its own discriminator (disc3): a receiver's per-framing narrow path (CubeSat 9600 baud FSK needs about
    // +-8 kHz with the AFC's residual, not the +-12 kHz the wide channel passes for every framing). 0: off (disc3 is then the wide discriminator)
    double chanPass3 = 0, chanStop3 = 0;
    float clipHz = 8000.f;
    double afcMaxHz = 12000.0;
    // the input is already FM-demodulated audio (a sound card on a radio's discriminator or speaker output): its real part is used, with
    // no offset, AFC or discriminator; the audio comes out scaled as if full scale were 4 kHz of deviation
    bool afInput = false;
    double fs = 0, offsetHz = 0, offPhase = 0;
    bool ready = false;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    dmr::Fir<cf32> chan, chan2, chan3;
    cf32 prevY2 = cf32(1.f, 0.f), prevY3 = cf32(1.f, 0.f);
    std::vector<cf32> ch2, ch3;
    std::vector<float> disc2, disc3;
    cf32 prevY = cf32(1.f, 0.f);
    std::vector<cf32> bufA, bufB, mixBuf, o2, ch, clean, afcBuf;
    std::vector<float> disc;
    // level, carrier, AFC
    double blkSum = 0, blkDisc = 0; size_t blkN = 0;
    std::deque<double> blocks;
    double nf = 0; bool nfKnown = false;
    bool carrier = false;
    int64_t lastCarrier = -(int64_t)1e12, carrierSince = 0;
    double snrEma = 0, cfoEma = 0; bool haveSnr = false;
    double afcHz = 0, afcEst = 0, afcPhase = 0, lastBlkCfo = 0;
    double inPower = 0; int64_t inN = 0;
    int64_t n96 = 0;
    size_t pending = 0;

    // false when the rate is under minRate or cannot be brought to 96 kHz
    bool configure(double rate, double minRate);
    void setOffset(double hz) { offsetHz = hz; offPhase = 0; }
    void reset();
    const std::vector<float>& process(const cf32* x, size_t n);
    const std::vector<float>& narrowDisc() const { return chanPass2 > 0 ? disc2 : disc; }   // valid after process()
    const std::vector<float>& thirdDisc() const { return chanPass3 > 0 ? disc3 : disc; }    // valid after process()
    void commit() { n96 += (int64_t)pending; pending = 0; }
    double cfoHz() const { return afcHz + cfoEma; }
    float takeLevelDb() { const float l = inN ? (float)(10 * std::log10(inPower / (double)inN + 1e-20)) : -200.f; inPower = 0; inN = 0; return l; }
    bool carrierRecent(double sec) const { return carrier || (n96 - lastCarrier) < (int64_t)(sec * kMidRate); }
private:
    void blockEnd();
};

// 1200 baud demodulator: correlate the 48 kHz audio with the mark and space tones over a window of about one bit; the stronger tone is
// the level; a clock loop picks the level once per bit; the levels go to the HDLC layer (which does NRZI, so the polarity does not matter).
struct Afsk1200 {
    int len = 40;
    float gain = 1.f, inertia = 0.75f, inertiaLocked = 0.85f;
    std::vector<float> mr, mi, sr, si;
    std::vector<float> ext;
    float ph = -0.5f;
    bool lev = false, prevLev = false;
    ax25::HdlcRx hdlc;
    int64_t pos = 0;
    int snapBits_ = 0;               // after a good frame: snap the clock on edges while the next preamble arrives

    void init(int L, bool hann, float g, float inert, float inertLocked) {
        using dect2::math::kPi;
        len = L; gain = g; inertia = inert; inertiaLocked = inertLocked;
        mr.assign((size_t)L, 0.f); mi = mr; sr = mr; si = mr;
        for (int t = 0; t < L; t++) {
            const int k = L - 1 - t;
            const double w = hann ? 0.5 - 0.5 * std::cos(2 * kPi * (k + 0.5) / L) : 1.0;
            mr[(size_t)t] = (float)(w * std::cos(2 * kPi * 1200.0 * k / kAudioRate));
            mi[(size_t)t] = (float)(w * std::sin(2 * kPi * 1200.0 * k / kAudioRate));
            sr[(size_t)t] = (float)(w * std::cos(2 * kPi * 2200.0 * k / kAudioRate));
            si[(size_t)t] = (float)(w * std::sin(2 * kPi * 2200.0 * k / kAudioRate));
        }
        reset();
    }
    void reset() {
        ext.assign((size_t)len - 1, 0.f);
        ph = -0.5f; lev = prevLev = false; pos = 0; snapBits_ = 0;
        hdlc.reset();
    }
    void process(const float* x, size_t n) {
        const size_t h = (size_t)len - 1;
        ext.resize(h + n);
        std::copy(x, x + n, ext.begin() + (ptrdiff_t)h);
        const float step = (float)(1200.0 / kAudioRate);
        for (size_t i = 0; i < n; i++) {
            const float* p = &ext[i];
            float a = 0, b = 0, c = 0, d = 0;
            for (int t = 0; t < len; t++) {
                const float v = p[t];
                a += v * mr[(size_t)t]; b += v * mi[(size_t)t];
                c += v * sr[(size_t)t]; d += v * si[(size_t)t];
            }
            lev = (a * a + b * b) > gain * gain * (c * c + d * d);
            if (lev != prevLev) {
                if (snapBits_ > 0) { ph = 0.f; snapBits_--; }
                else ph *= hdlc.flagSeen() ? inertiaLocked : inertia;
            }
            prevLev = lev;
            ph += step;
            if (ph >= 0.5f) {
                ph -= 1.f;
                hdlc.bit(lev ? 1 : 0);
                if (hdlc.takeClockResync()) { ph = 0.f; snapBits_ = 96; }
            }
            pos++;
        }
        std::copy(ext.end() - (ptrdiff_t)h, ext.end(), ext.begin());
        ext.resize(h);
    }
};

// 1200 baud with several slicers on one pair of tone correlators (the "multi-slicer" idea of good software TNCs). Each slicer compares the
// mark and space magnitudes with its own weight (a transmitter's pre-emphasis, a radio's de-emphasis or its audio response leave one tone
// several dB under the other: the slicer whose weight matches gets the frame), one more compares each tone against its own tracked envelope
// (an AGC per tone, any balance), and every slicer has its own clock loop and HDLC receiver. markDb() - spaceDb() is the measured tone
// balance (twist) of the signal of the last 40 ms or so.
struct AfskMulti {
    struct Chain {
        float gain = 1.f;            // mark wins when |mark| > gain * |space|
        bool agc = false;            // compare against the tracked envelopes instead
        float ph = -0.5f;
        bool lev = false, prevLev = false;
        int snapBits = 0;
        ax25::HdlcRx hdlc;
    };
    int len = 40;
    float inertia = 0.75f, inertiaLocked = 0.85f;
    std::vector<float> mr, mi, sr, si, ext;
    std::vector<Chain> chains;
    float mPk = 0, sPk = 0;
    double markAvg = 0, spaceAvg = 0;
    int64_t pos = 0;

    void init(int L, bool hann, const std::vector<float>& gains, bool withAgc, float inert, float inertLocked) {
        using dect2::math::kPi;
        len = L; inertia = inert; inertiaLocked = inertLocked;
        mr.assign((size_t)L, 0.f); mi = mr; sr = mr; si = mr;
        for (int t = 0; t < L; t++) {
            const int k = L - 1 - t;
            const double w = hann ? 0.5 - 0.5 * std::cos(2 * kPi * (k + 0.5) / L) : 1.0;
            mr[(size_t)t] = (float)(w * std::cos(2 * kPi * 1200.0 * k / kAudioRate));
            mi[(size_t)t] = (float)(w * std::sin(2 * kPi * 1200.0 * k / kAudioRate));
            sr[(size_t)t] = (float)(w * std::cos(2 * kPi * 2200.0 * k / kAudioRate));
            si[(size_t)t] = (float)(w * std::sin(2 * kPi * 2200.0 * k / kAudioRate));
        }
        chains.clear();
        chains.resize(gains.size() + (withAgc ? 1 : 0));
        for (size_t i = 0; i < gains.size(); i++) chains[i].gain = gains[i];
        if (withAgc) chains.back().agc = true;
        reset();
    }
    void reset() {
        ext.assign((size_t)len - 1, 0.f);
        for (Chain& c : chains) { c.ph = -0.5f; c.lev = c.prevLev = false; c.snapBits = 0; c.hdlc.reset(); }
        mPk = sPk = 0; markAvg = spaceAvg = 0; pos = 0;
    }
    double markDb() const { return 20 * std::log10(markAvg + 1e-12); }
    double spaceDb() const { return 20 * std::log10(spaceAvg + 1e-12); }
    void process(const float* x, size_t n) {
        const size_t h = (size_t)len - 1;
        ext.resize(h + n);
        std::copy(x, x + n, ext.begin() + (ptrdiff_t)h);
        const float step = (float)(1200.0 / kAudioRate);
        const float decay = 1.f / (float)(len * 6);         // the envelopes fall over about six bits
        for (size_t i = 0; i < n; i++) {
            const float* p = &ext[i];
            float a = 0, b = 0, c = 0, d = 0;
            for (int t = 0; t < len; t++) {
                const float v = p[t];
                a += v * mr[(size_t)t]; b += v * mi[(size_t)t];
                c += v * sr[(size_t)t]; d += v * si[(size_t)t];
            }
            const float m = std::sqrt(a * a + b * b), s = std::sqrt(c * c + d * d);
            mPk += (m > mPk ? 0.3f : decay) * (m - mPk);
            sPk += (s > sPk ? 0.3f : decay) * (s - sPk);
            if (m > s) markAvg += 0.0005 * (m - markAvg); else spaceAvg += 0.0005 * (s - spaceAvg);
            const float mn = m * sPk, sn = s * mPk;            // m / mPk against s / sPk
            for (Chain& ch : chains) {
                ch.lev = ch.agc ? mn > sn : m > ch.gain * s;
                if (ch.lev != ch.prevLev) {
                    if (ch.snapBits > 0) { ch.ph = 0.f; ch.snapBits--; }
                    else ch.ph *= ch.hdlc.flagSeen() ? inertiaLocked : inertia;
                }
                ch.prevLev = ch.lev;
                ch.ph += step;
                if (ch.ph >= 0.5f) {
                    ch.ph -= 1.f;
                    ch.hdlc.bit(ch.lev ? 1 : 0);
                    if (ch.hdlc.takeClockResync()) { ch.ph = 0.f; ch.snapBits = 96; }
                }
            }
            pos++;
        }
        std::copy(ext.end() - (ptrdiff_t)h, ext.end(), ext.begin());
        ext.resize(h);
    }
};

// First-order audio emphasis on the 48 kHz audio: de-emphasis (a pole at 300 Hz: undoes a transmitter's 6 dB per octave pre-emphasis,
// 2200 Hz comes down about 5 dB against 1200 Hz) or pre-emphasis (a zero at 300 Hz, a pole at 8 kHz: undoes a radio's de-emphasised speaker
// audio). Bilinear transform; unit gain at 1200 Hz.
struct Emphasis {
    int mode = 0;                    // 0 none, 1 de-emphasis, 2 pre-emphasis
    double b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    void set(int m) {
        using dect2::math::kPi;
        mode = m; x1 = y1 = 0;
        const double fs = kAudioRate;
        double B0, B1, A0, A1;
        if (m == 1) { const double k = fs / (kPi * 300.0); B0 = 1; B1 = 1; A0 = 1 + k; A1 = 1 - k; }
        else if (m == 2) { const double k1 = fs / (kPi * 300.0), k2 = fs / (kPi * 8000.0); B0 = 1 + k1; B1 = 1 - k1; A0 = 1 + k2; A1 = 1 - k2; }
        else { b0 = 1; b1 = a1 = 0; return; }
        const double w = 2 * kPi * 1200.0 / fs;
        const double nr = B0 + B1 * std::cos(w), ni = -B1 * std::sin(w), dr = A0 + A1 * std::cos(w), di = -A1 * std::sin(w);
        const double g = std::sqrt((dr * dr + di * di) / (nr * nr + ni * ni));
        b0 = g * B0 / A0; b1 = g * B1 / A0; a1 = A1 / A0;
    }
    void reset() { x1 = y1 = 0; }
    void process(float* x, size_t n) {
        if (!mode) return;
        for (size_t i = 0; i < n; i++) {
            const double y = b0 * x[i] + b1 * x1 - a1 * y1;
            x1 = x[i]; y1 = y;
            x[i] = (float)y;
        }
    }
};

// Direct 2-FSK on the discriminator output (G3RUH 9600 baud, or GMSK / FSK at another speed): slicer threshold from tracked peaks, clock
// loop, then optionally the G3RUH descrambler (x^17 + x^12 + 1). The bits go to the HDLC layer (NRZI there), or, with bitSink set, out as
// they are (an ASM framer; then `locked` tells the clock loop to slow down inside a frame).
struct FskSlicer {
    double rate = kMidRate, baud = 9600.0;
    bool descramble = true;
    float inertia = 0.7f, inertiaLocked = 0.8f;
    float hi = 0, lo = 0, ph = -0.5f;
    bool lev = false, prevLev = false;
    uint32_t sr = 0;
    ax25::HdlcRx hdlc;
    std::function<void(int)> bitSink;
    bool locked = false;
    int64_t pos = 0;
    void init(float inert, float inertLocked) { inertia = inert; inertiaLocked = inertLocked; reset(); }
    void reset() { hi = lo = 0; ph = -0.5f; lev = prevLev = false; sr = 0; pos = 0; locked = false; hdlc.reset(); }
    void process(const float* x, size_t n) {
        const float step = (float)(baud / rate);
        for (size_t i = 0; i < n; i++) {
            const float v = x[i];
            if (v > hi) hi += 0.25f * (v - hi); else hi += 0.0015f * (v - hi);
            if (v < lo) lo += 0.25f * (v - lo); else lo += 0.0015f * (v - lo);
            lev = v > 0.5f * (hi + lo);
            if (lev != prevLev) ph *= (bitSink ? locked : hdlc.flagSeen()) ? inertiaLocked : inertia;
            prevLev = lev;
            ph += step;
            if (ph >= 0.5f) {
                ph -= 1.f;
                const uint32_t raw = lev ? 1u : 0u;
                uint32_t d = raw;
                if (descramble) {
                    d = raw ^ ((sr >> 11) & 1u) ^ ((sr >> 16) & 1u);
                    sr = ((sr << 1) | raw) & 0x1FFFFu;
                }
                if (bitSink) bitSink((int)d);
                else hdlc.bit((int)d);
            }
            pos++;
        }
    }
};

} // namespace pktdemod
} // namespace dect2
