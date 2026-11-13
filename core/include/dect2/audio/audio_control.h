// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Audio control: the listening chain every audio mode shares (docs/AUDIO.md). Pure float DSP blocks (no allocation, no locks in process()),
// one AudioControl chain per AudioOut, all of them follow one AudioControlConfig that the UI fills from the settings of the current mode.
//
// Order: stereo -> high-pass / low-pass -> notch (manual + auto) -> clarifier -> EQ -> de-emphasis -> AGC -> squelch (with tail delay) -> meter.
// [slot] The spectrum-revamp noise blanker works on the IQ, not here; its audio-side switch will sit before "stereo" (kSlotBlanker).
// Noise reduction (spectral gating, off by default) is the Audio cleanup chain (audio_cleanup.h), which runs before this one.
// Everything is off or transparent by default: a mode that has not been touched sounds as before.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace actl {

// ---- filters ---------------------------------------------------------------------------------------------------------------------------
struct BiquadCoef { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };   // y = b0 x + z1; z1 = b1 x - a1 y + z2; z2 = b2 x - a2 y (transposed DF2)
BiquadCoef bqLowpass(float hz, float q, float rate);
BiquadCoef bqHighpass(float hz, float q, float rate);
BiquadCoef bqNotch(float hz, float q, float rate);
BiquadCoef bqPeaking(float hz, float q, float gainDb, float rate);
BiquadCoef bqLowShelf(float hz, float gainDb, float rate);
BiquadCoef bqHighShelf(float hz, float gainDb, float rate);
float bqMagnitudeDb(const BiquadCoef& c, float hz, float rate);   // the response at one frequency (tests, the EQ curve in the UI)

struct BiquadState { float z1 = 0, z2 = 0; };
inline float bqRun(const BiquadCoef& c, BiquadState& s, float x) {
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

// ---- EQ ---------------------------------------------------------------------------------------------------------------------------------
constexpr int kEqBands = 5;
extern const float kEqHz[kEqBands];   // 120, 400, 1000, 2500, 6000
enum class EqPreset { Flat = 0, Voice, Music, CW, Custom, Count };
const char* eqPresetName(EqPreset p);
void eqPresetGains(EqPreset p, float gainDb[kEqBands]);   // Custom leaves the array alone
// Band 0 is a low shelf, band 4 a high shelf, the three between are peaking (Q 1.0).
void eqDesign(const float gainDb[kEqBands], float rate, BiquadCoef out[kEqBands]);
float eqResponseDb(const float gainDb[kEqBands], float hz, float rate);

// ---- squelch ----------------------------------------------------------------------------------------------------------------------------
// The decision core: a measurement in dB goes in, open/closed comes out. Opens at >= openDb, starts the hang when the measure falls below
// closeDb (closeDb < openDb: the hysteresis), closes when the hang has run out without the measure rising above closeDb again.
struct SquelchCore {
    float openDb = -50, closeDb = -54;
    float hangMs = 300;
    bool open = false;
    float hangLeftMs = 0;
    bool update(float measureDb, float dtMs);
    void reset() { open = false; hangLeftMs = 0; }
};

// CTCSS: Goertzel at the wanted tone against the neighbours (200 ms blocks). `detected` after each block.
struct ToneDetector {
    void prepare(float rate, float toneHz);
    // feeds mono samples; true while the tone is present (updates once per block)
    bool process(const float* x, int n);
    bool present() const { return present_; }
    float toneHz() const { return toneHz_; }
private:
    float rate_ = 48000, toneHz_ = 0;
    int block_ = 9600, count_ = 0;
    float c_[3] = {0, 0, 0}, s1_[3] = {0, 0, 0}, s2_[3] = {0, 0, 0};
    bool present_ = false;
};
extern const float kCtcssHz[50];   // the standard sub-audible tones, 67.0 .. 254.1 Hz

// DCS: the 23-bit Golay word (134.4 bit/s, below 300 Hz) of a 3-digit octal code. Detection slices the low band, searches the stream for the
// word (any rotation, either polarity).
uint32_t dcsWord(int octalCode);   // 23 bits, the first bit sent is bit 0
struct DcsDetector {
    void prepare(float rate, int octalCode);
    bool process(const float* x, int n);
    bool present() const { return present_; }
private:
    float rate_ = 48000, lp1_ = 0, lp2_ = 0, mean_ = 0, lpA_ = 0.05f, meanA_ = 0.0005f;
    double phase_ = 0, inc_ = 0.0028;
    int quarter_ = 0, holdLeft_ = 0, holdMax_ = 24000;
    long bitCount_ = 0;
    uint32_t hist_[4] = {0, 0, 0, 0};
    long lastHit_[4] = {-1000, -1000, -1000, -1000};
    int run_[4] = {0, 0, 0, 0};
    uint32_t pat_[46] = {};
    bool present_ = false;
};

// ---- AGC / meters -----------------------------------------------------------------------------------------------------------------------
enum class AgcMode { Off = 0, Fast, Slow };
struct Agc {
    void prepare(float rate);
    void setMode(AgcMode m, float targetDb);
    // in place, one channel pair (mono envelope from max(|l|,|r|)); gain is limited to maxGainDb
    void process(float* st, int frames);
    float gainDb() const { return gain_ > 1e-6f ? 20.f * std::log10(gain_) : -120.f; }
    float rate_ = 48000, env_ = 0, gain_ = 1, target_ = 0.1f, att_ = 0.01f, rel_ = 0.0005f, floor_ = 0.002f, maxGain_ = 31.6f;
    AgcMode mode_ = AgcMode::Off;
};

struct Meter {   // peak and RMS of what is played, clip latch
    float peak = 0, rms2 = 0;
    uint32_t clipped = 0;       // samples at or beyond full scale since reset
    void process(const float* st, int frames);
    float peakDb() const;
    float rmsDb() const;
};

// FFT magnitude of mono samples (Hann window), n a power of two <= 4096. out gets n/2 values in dBFS.
void spectrumDb(const float* x, int n, float* outDb);

// ---- the settings -----------------------------------------------------------------------------------------------------------------------
enum class StereoMode { Stereo = 0, Mono, Swap, Left, Right };
enum class Deemph { Off = 0, Us50, Us75 };
enum class SquelchKind { Off = 0, Level, Snr, Ctcss, Dcs };

struct AudioControlSettings {
    bool on = false;                                  // the whole chain (off: bit-exact pass-through)
    StereoMode stereo = StereoMode::Stereo;
    bool hp = false;  float hpHz = 300.f;  int hpSlope = 12;     // dB/oct 12 or 24
    bool lp = false;  float lpHz = 3000.f; int lpSlope = 12;
    bool eq = false;  EqPreset eqPreset = EqPreset::Flat; float eqDb[kEqBands] = {0, 0, 0, 0, 0};
    bool notch = false; float notchHz = 1000.f; float notchQ = 10.f;
    bool autoNotch = false;
    Deemph deemph = Deemph::Off;
    AgcMode agc = AgcMode::Off; float agcTargetDb = -18.f;
    float shiftHz = 0.f;                              // clarifier (SSB): -1000..1000 Hz, 0 = off
    float cwToneHz = 0.f;                             // CW: wanted pitch, 0 = leave (the receivers produce kCwNominalHz)
    SquelchKind sqKind = SquelchKind::Off;
    float sqOpenDb = -50.f;                           // Level: dBFS. Snr: dB of speech band over high-band noise
    float sqHystDb = 4.f;                             // close = open - hysteresis
    float sqHangMs = 250.f;
    float sqTailMs = 0.f;                             // squelch tail elimination: the output runs this much behind so the noise burst is cut
    float ctcssHz = 0.f; int dcsCode = 23;            // tone squelch
    bool recordOnlyOpen = false;                      // the recorder gets nothing while the squelch is closed
    float volume = 1.f; bool muted = false;           // per mode, kept by the app (the value is applied by the output)
    std::string device;                               // output device by name, "" = system default
    int latencyMs = 300;                              // audio queued before playback starts
    int bufferMs = 0;                                 // 0: automatic
};
constexpr float kCwNominalHz = 700.f;
AudioControlSettings audioControlDefaults(int profile);   // AudioProfile value; the bandwidth that suits the kind of mode (still off)
std::string audioControlToString(const AudioControlSettings& s);                  // key=value;key=value, for the preferences
AudioControlSettings audioControlFromString(const std::string& str, const AudioControlSettings& base);
// A saved output device that is gone: "" (the system default); otherwise the name.
std::string audioPickDevice(const std::vector<std::string>& present, const std::string& wanted);

}  // namespace actl

// What the audio threads read (atomics, any thread writes); statistics go back the other way.
class AudioControlConfig {
public:
    static AudioControlConfig& global();
    void set(const actl::AudioControlSettings& s);
    actl::AudioControlSettings get() const;
    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }
    std::atomic<float> statPeakDb{-120.f}, statRmsDb{-120.f}, statAgcDb{0.f}, statNotchHz{0.f}, statSqMeasure{-120.f};
    std::atomic<bool> statSqOpen{true};
    std::atomic<uint32_t> statClips{0};
    static constexpr int kAfBins = 128;   // AF spectrum strip: 128 bins from 0 to rate/2, dBFS (written by the chain while afWanted)
    std::atomic<float> afDb[kAfBins];
    std::atomic<int> afRate{48000};
    std::atomic<bool> recordOnlyOpen{false};   // mirrors the settings: the recorder reads it without taking the lock
    std::atomic<bool> afWanted{false};   // the UI shows the strip: the chain computes it
private:
    mutable std::atomic<uint32_t> gen_{0};
    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
public:
    AudioControlConfig() { for (auto& v : afDb) v.store(-120.f); }
private:
    actl::AudioControlSettings s_;
};

class AudioControl {
public:
    AudioControl();
    ~AudioControl();
    AudioControl(const AudioControl&) = delete;
    AudioControl& operator=(const AudioControl&) = delete;
    void prepare(int sampleRate);
    void setConfig(AudioControlConfig* c) { cfg_ = c; }
    static constexpr int kMaxChunk = 1024;
    // Interleaved stereo. Returns `in` while the chain is off, else `scratch` (kMaxChunk*2 floats). *open: the squelch lets the sound through.
    const float* process(const float* in, int frames, float* scratch, bool* active = nullptr, bool* open = nullptr);
    bool squelchOpen() const { return open_.load(std::memory_order_relaxed); }
    void reset();
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
    AudioControlConfig* cfg_;
    std::atomic<bool> open_{true};
};

}  // namespace dect2
