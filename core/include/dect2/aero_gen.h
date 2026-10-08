// Inmarsat Aero test signal: P channels (ground to aircraft) carrying system tables, logons and ACARS uplinks to six aircraft
// over the Gulf and the Indian Ocean, with valid framing and coding (aero_phy.h) and SUs from the SU layer's builders (aero_su.h).
// The aircraft also send position reports (ADS-C basic reports in labels B6 and H1, one ARINC 702 text POS report), as downlinks on the
// same channels so the map has something to show: a real P channel carries only uplinks. They fly 20 times faster than real.
// The channels sit relative to the user's frequency (and are moved by -tuneOffsetHz for the radio, as the engine expects):
//   10500 bit/s at -50 kHz, 1200 bit/s at +150 kHz, 600 bit/s at +100 kHz; each with its own small carrier offset and a slow drift.
//
// SynthConfig:
//   modeOpt[0]  channels, a bit mask: 1 = 600, 2 = 1200, 4 = 10500 bit/s (0 = 10500 + 1200)
//   modeOpt[1]  seed of the noise and of the filler bytes (0 = 1)
//   modeOpt[2]  1 = no built-in carrier offsets and drift (cfoHz and modeVal[1] still apply)
//   modeVal[0]  Eb/N0 per channel bit in dB (0 = 12)
//   modeVal[1]  extra carrier drift of every channel in Hz per second (linear)
//   cfoHz       carrier offset added to every channel; sroPpm: bit clock offset of every channel
//   snrDb       not used (Eb/N0 is the measure for these channels)
#pragma once
#include "mode_synth.h"
#include "aero_su.h"
#include <memory>
#include <vector>

namespace dect2 {

std::unique_ptr<ModeSynth> makeAeroSynth(const SynthConfig& cfg, double sampleRate);

class AeroSynth : public ModeSynth {
public:
    AeroSynth(const SynthConfig& cfg, double sampleRate);
    ~AeroSynth() override;
    double sampleRate() const override;
    void generate(cf32* out, size_t n) override;

    // for tests: what went out
    struct ChannelInfo { int bitRate; double offsetHz; };      // offset from the user's frequency (before the tune offset)
    std::vector<ChannelInfo> channels() const;
    struct SentFrame { int channel; int bitRate; uint64_t frameNo; std::vector<uint8_t> bytes; };
    struct SentMessage { int channel; double time; AeroAcars msg; };
    struct SentLogon { int channel; double time; AeroLogon logon; };
    void record(bool on);                                      // keep what is sent (off by default)
    std::vector<SentFrame> takeFrames();
    std::vector<SentMessage> takeMessages();
    std::vector<SentLogon> takeLogons();
    double signalAmplitude(int channel) const;                 // rms of that carrier in the output
    double noiseSigma() const;                                 // per component
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
