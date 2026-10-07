// DRM test signal: a DRM30 transmitter (robustness modes A to D) as complex baseband. The synthetic source plays it, drmtool writes it to a file.
// Nothing is transmitted: the samples only exist in memory or in a file.
//
// Options of the synthetic source (SynthConfig::modeOpt / modeVal, all 0 = the default shown):
//   modeOpt[0]  robustness mode: 0 B (default), 1 A, 2 B, 3 C, 4 D
//   modeOpt[1]  spectrum occupancy: 0 the usual one of the mode (10 kHz), 1 .. 6 = occupancy code 0 .. 5 (4.5, 5, 9, 10, 18, 20 kHz; modes C and D only have 10 and 20 kHz)
//   modeOpt[2]  MSC constellation: 0 64-QAM, 1 16-QAM
//   modeOpt[3]  MSC protection level of part B: 0 the second one (rate about 0.6 for 64-QAM), 1 .. 4 = level 0 .. 3
//   modeOpt[4]  time interleaving: 0 long (2 s), 1 short (400 ms)
//   modeOpt[5]  audio (AAC-LC, 24 kHz mono, coded by our own encoder, drm_aacenc.h): 0 a short melody, 1 a steady 1 kHz tone, 2 silence, 3 noise-like test sound (no two frames alike,
//               for tests of the channel coding), 4 frames of random bytes that are not AAC (the receiver counts them as damaged)
//   modeOpt[6]  channel: 0 none (only the noise), 1 .. 6 = the channels of Annex B.1 (1 AWGN, 2 Rice with delay, 3 US consortium, 4 CCIR poor, 5, 6 the worst of them)
//   modeOpt[7]  text message: 0 on, 1 off
// SynthConfig::snrDb is the carrier to noise ratio in the nominal channel width (signal power including pilots and guard interval, as in Annex A of the
// standard); noise is limited to +-24 kHz around the signal like after a receiver's channel filter. cfoHz, sroPpm: carrier and sample clock offset. echoDb and
// echoDelay add one echo (delay in samples of 48 kHz) on top of the channel.
#pragma once
#include "dect2/drm_defs.h"
#include "dect2/drm_msg.h"
#include "mode_synth.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DrmTxConfig {
    int mode = drm::kModeB;      // 0 A, 1 B, 2 C, 3 D (mode E is not generated)
    int occupancy = 3;           // Table 48: 0 4.5 kHz, 1 5, 2 9, 3 10, 4 18, 5 20 kHz
    int mscQam = 64;             // 16 or 64
    int protA = 0;               // protection level of the higher protected part (only used with partABytes > 0)
    int protB = 1;               // protection level of the lower protected part
    int partABytes = 0;          // bytes of the audio stream in part A (0: equal error protection)
    bool longInterleave = true;  // 2 s (otherwise 400 ms)
    int sdcMode = 0;             // 0 16-QAM, 1 4-QAM
    std::string label = "OnAir DRM";
    uint32_t serviceId = 0xE12345;
    int language = 5;            // Table 18: English
    std::string language3 = "eng", country2 = "ae";
    int programmeType = 10;      // Table 19: pop music
    int audioCoding = 0;         // 0 AAC (the only coding that is generated)
    int audioSbr = 0;
    int audioMode = 0;           // 0 mono
    int audioRateHz = 24000;     // 12000 or 24000 (the core sampling rate of the AAC stream): 5 or 10 audio frames per super frame
    bool textMessage = true;
    std::string text = "OnAir DRM test signal";
    int year = 2026, month = 10, day = 6, hour = 12, minute = 0;   // the clock that SDC type 8 reports (advances with the signal)
    bool sendTime = true, sendLanguage = true;
    uint32_t seed = 1;
};

// The content of the audio stream: the frames of each audio super frame
class DrmAudioSource {
public:
    virtual ~DrmAudioSource() = default;
    // numFrames frames whose sizes add up to exactly `payload` bytes, and one CRC byte per frame (the 8 bit CRC of the first 4 bytes of the frame, see drm_gen.cpp)
    virtual void nextSuperFrame(int numFrames, int payload, std::vector<std::vector<uint8_t>>& frames, std::vector<uint8_t>& crc) = 0;
    virtual std::string describe() const { return "test pattern"; }
    // bytes of every audio frame that sit in the higher protected part (unequal error protection); 0 for equal protection. Called once, before the first frame.
    virtual void setHigherProtectedBytes(int) {}
};

// Frames made of a known pseudo random pattern, for tests of the channel coding: frame content = pattern(seed, super frame index)
class DrmPatternSource : public DrmAudioSource {
public:
    explicit DrmPatternSource(uint32_t seed = 1) : seed_(seed) {}
    void nextSuperFrame(int numFrames, int payload, std::vector<std::vector<uint8_t>>& frames, std::vector<uint8_t>& crc) override;
    static void pattern(uint32_t seed, uint64_t index, int numFrames, int payload, std::vector<std::vector<uint8_t>>& frames, std::vector<uint8_t>& crc);
    uint64_t index() const { return index_; }
private:
    uint32_t seed_;
    uint64_t index_ = 0;
};

// Audio frames from our own AAC-LC encoder (drm_aacenc.h): kind 0 a melody, 1 a 1 kHz tone, 2 silence, 3 noise-like test sound (no two frames alike). nullptr for a
// configuration that is not mono AAC at 12 or 24 kHz.
std::unique_ptr<DrmAudioSource> makeDrmAacSource(const DrmTxConfig& cfg, int kind);

class DrmTransmitter {
public:
    DrmTransmitter(const DrmTxConfig& cfg, std::unique_ptr<DrmAudioSource> audio);
    ~DrmTransmitter();
    bool ok() const;                              // the configuration is valid
    const DrmTxConfig& config() const;
    static constexpr int kRate = 48000;           // sample rate of the baseband that superFrame() delivers
    int superFrameSamples() const;                // samples of one transmission super frame (3 frames = 1.2 s)
    int frameSamples() const;
    void superFrame(std::vector<cf32>& out);      // appends one transmission super frame
    // the layout, for tests
    int streamBytes() const;                      // bytes of the logical frame of the audio stream (parts A and B, text message included)
    int streamBytesA() const;
    int audioPayload() const;                     // bytes of the audio super frame (the logical frame without the text message)
    int audioFrames() const;
    int muxCells() const;
    int nSuperFrames() const;                     // super frames generated so far
    int firstDecodableFrame() const;              // the number of the first audio super frame a receiver that starts at the beginning can decode (the pre-roll of the interleaver)
    double signalPower() const;                   // mean power of the samples (1.0: the output is normalised)
    double noiseBandwidthHz() const;              // nominal channel width
    // the carrier offset of the centre of the occupied carriers from the reference frequency, in Hz (positive: the signal sits above the reference)
    double centreOffsetHz() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Fading channel of Annex B.1 (profiles 1 to 6) on a 48 kHz baseband stream
class DrmChannelSim {
public:
    DrmChannelSim(int profile, uint32_t seed);
    ~DrmChannelSim();
    void process(cf32* x, size_t n);              // in place; the gain is normalised so that the mean power stays 1
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeDrmSynth(const SynthConfig& cfg, double sampleRate);
// The transmitter configuration and audio source that the synthetic source builds from a SynthConfig (the receiver tests build the same to know what was sent)
DrmTxConfig drmTxConfigFromSynth(const SynthConfig& sc);
std::unique_ptr<DrmAudioSource> drmAudioFromSynth(const SynthConfig& sc, const DrmTxConfig& tc);

} // namespace dect2
