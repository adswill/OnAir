// Audio cleanup for listening: noise reduction, automatic and manual notch, leveller with soft limiter, squelch-aware silence muting,
// voice emphasis and de-esser. One chain per AudioOut (audioout.cpp), all of them follow one shared AudioCleanupConfig, which the UI sets.
//
// Order: notch (auto + manual) -> noise reduction -> voice EQ + de-esser -> silence gate -> leveller + soft limiter.
// All stages are our own code (clean-room from the published algorithms: minimum statistics noise tracking, decision-directed Wiener
// gain, see docs/audio-cleanup.md). process() never allocates or locks: everything is sized in prepare() (called when the output starts).
// Latency: 256 samples (5.3 ms at 48 kHz) while the chain is on, none while it is off; every stage fades in and out, no clicks.
// Thread safety: the config is atomics (any thread writes, the audio thread reads); process() is called from one thread per chain.
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

namespace dect2 {

// A plain copy of the settings (presets, the prefs, the tests).
struct AudioCleanupSettings {
    bool master = false;           // the chain is on
    bool nr = false;  float nrStrength = 0.5f;                   // 0..1
    bool neural = false; float neuralStrength = 1.0f;            // RNNoise voice noise remover on every mode's audio (speech only; off by default)
    bool autoNotch = false;                                      // whistles, carriers, heterodynes
    bool notch = false; float notchHz = 1000.f; float notchWidth = 0.5f;   // manual notch; width 0 (narrowest) .. 1 (widest)
    bool level = false; float levelTargetDb = -20.f; float levelAmount = 0.7f; bool levelHold = true;   // RMS target, 0..1, keep noise from being boosted
    bool gate = false;  float gateOffsetDb = 9.f;                // mute below noise floor + offset
    bool voice = false; float voiceAmount = 0.6f;                // speech EQ
    bool deess = false; float deessAmount = 0.5f;
    bool recCleaned = false;       // "Record audio" saves the cleaned sound instead of the raw one
    bool skipSilence = false;      // with recCleaned: nothing is recorded while the gate is closed
};

enum class AudioProfile { Off = 0, Generic, AM, NFM, WFM, SSB, CW, DigitalVoice, Count };
const char* audioProfileName(AudioProfile p);
AudioCleanupSettings audioCleanupPreset(AudioProfile p);   // the defaults per kind of mode (master off: switched on by the user)

class AudioCleanupConfig {
public:
    static AudioCleanupConfig& global();
    void set(const AudioCleanupSettings& s);
    AudioCleanupSettings get() const;
    // fields read by the audio thread
    std::atomic<bool> master{false}, nr{false}, neural{false}, autoNotch{false}, notch{false}, level{false}, levelHold{true}, gate{false}, voice{false}, deess{false};
    std::atomic<bool> recCleaned{false}, skipSilence{false};
    std::atomic<float> nrStrength{0.5f}, neuralStrength{1.0f}, notchHz{1000.f}, notchWidth{0.5f}, levelTargetDb{-20.f}, levelAmount{0.7f}, gateOffsetDb{9.f},
        voiceAmount{0.6f}, deessAmount{0.5f};
    // what the running chains do, for the UI (written by the audio threads, any of them: the last one wins)
    mutable std::atomic<float> statNrDb{0.f}, statNotchHz{0.f};
    mutable std::atomic<bool> statGateOpen{true};
};

class AudioCleanup {
public:
    AudioCleanup();
    ~AudioCleanup();
    AudioCleanup(const AudioCleanup&) = delete;
    AudioCleanup& operator=(const AudioCleanup&) = delete;
    // Sizes everything for this rate (not on the audio thread). Rates from 8 kHz up.
    void prepare(int sampleRate);
    void setConfig(const AudioCleanupConfig* c) { cfg_ = c; }   // default: the global one
    // Interleaved stereo, up to kMaxChunk frames at a time. Returns the pointer to read the result from: `in` itself while the chain is off,
    // else `scratch` (kMaxChunk * 2 floats, caller's). True in *active when the chain ran.
    static constexpr int kMaxChunk = 1024;
    const float* process(const float* in, int frames, float* scratch, bool* active = nullptr);
    bool gateOpen() const { return gateOpen_.load(std::memory_order_relaxed); }   // false: the silence gate holds the sound off
    int latencySamples() const;                                                    // while on
    float autoNotchHz(int slot) const;                                             // for the UI: the frequency a slot is notching, 0 = idle
    float nrGainDb() const { return nrGainDb_.load(std::memory_order_relaxed); }   // mean noise-reduction gain of the last frame (UI meter)
    void reset();

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
    const AudioCleanupConfig* cfg_;
    std::atomic<bool> gateOpen_{true};
    std::atomic<float> nrGainDb_{0.f};
};

}  // namespace dect2
