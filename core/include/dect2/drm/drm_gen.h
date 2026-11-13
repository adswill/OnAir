// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM test signal: a DRM30 transmitter (robustness modes A to D) and a DRM+ one (mode E, 100 kHz) as complex baseband. The synthetic source plays it,
// drmtool writes it to a file.
// Nothing is transmitted: the samples only exist in memory or in a file.
//
// Options of the synthetic source (SynthConfig::modeOpt / modeVal, all 0 = the default shown):
//   modeOpt[0]  robustness mode: 0 B (default), 1 A, 2 B, 3 C, 4 D, 5 E (DRM+: 100 kHz at 192 kHz, label "OnAir DRM+")
//   modeOpt[1]  spectrum occupancy: 0 the usual one of the mode (10 kHz), 1 .. 6 = occupancy code 0 .. 5 (4.5, 5, 9, 10, 18, 20 kHz; modes C and D only have 10 and 20 kHz;
//               mode E only has 100 kHz)
//   modeOpt[2]  MSC constellation: 0 64-QAM (mode E 16-QAM), 1 16-QAM (mode E 4-QAM)
//   modeOpt[3]  MSC protection level of part B: 0 the second one (rate about 0.6 for 64-QAM; mode E level 2 for 16-QAM, 1 for 4-QAM), 1 .. 4 = level 0 .. 3
//   modeOpt[4]  time interleaving: 0 long (2 s), 1 short (400 ms)
//   modeOpt[5]  audio (AAC-LC, 24 kHz mono, coded by our own encoder, drm_aacenc.h): 0 a short melody, 1 a steady 1 kHz tone, 2 silence, 3 noise-like test sound (no two frames alike,
//               for tests of the channel coding), 4 frames of random bytes that are not AAC (the receiver counts them as damaged)
//   modeOpt[6]  channel: 0 none (only the noise), 1 .. 6 = the channels of Annex B.1 (1 AWGN, 2 Rice with delay, 3 US consortium, 4 CCIR poor, 5, 6 the worst of them;
//               short wave channels, not applied in mode E)
//   modeOpt[7]  text message: 0 on, 1 off
// SynthConfig::snrDb is the carrier to noise ratio in the nominal channel width (signal power including pilots and guard interval, as in Annex A of the
// standard); noise is limited to +-24 kHz around the signal like after a receiver's channel filter (mode E: +-96 kHz). cfoHz, sroPpm: carrier and sample clock offset. echoDb and
// echoDelay add one echo (delay in samples of 48 kHz) on top of the channel.
#pragma once
#include "drm_defs.h"
#include "drm_msg.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The content of a packet mode data service: MSC data groups (data units), one after the other, for ever
class DrmDataSource {
public:
    virtual ~DrmDataSource() = default;
    virtual std::vector<uint8_t> nextUnit() = 0;
};

// A MOT object to send (ETSI EN 301 234; slide show parameters of TS 101 499)
struct DrmMotObject {
    std::string name;
    int contentType = 2, subtype = 1;    // 2/1 image JPEG, 2/3 PNG, 1/2 HTML, 1/0 ASCII text
    std::vector<uint8_t> body;
    std::string caption, category, clickUrl;
    int categoryId = -1, slideId = -1;   // -1: not sent
    bool hasTrigger = false;             // TriggerTime: now (trigMjd 0) or the given date and time
    int trigMjd = 0, trigHour = 0, trigMinute = 0;
};
// The MOT carousel: every object in turn, as a header and body segments of `segSize` bytes each (header mode), or the directory first and then the bodies
// (directory mode). Transport ids count up from firstTid.
std::shared_ptr<DrmDataSource> makeDrmMotSource(const std::vector<DrmMotObject>& objects, int segSize, bool directoryMode, uint16_t firstTid = 1);
// A PNG picture of w x h pixels (RGB, stored deflate) and the pixels it holds, for tests
std::vector<uint8_t> drmGenPng(int w, int h, int seed);
void drmGenPngPixels(int w, int h, int seed, std::vector<uint8_t>& rgba);
// One MSC data group (EN 300 401 clause 5.3.3.1) of a MOT segment: type 3 header, 4 body, 6 directory
std::vector<uint8_t> drmGenMotGroup(int type, uint16_t tid, int segNo, bool last, int repetition, const uint8_t* seg, size_t n, int continuity);
// MOT header core and extension for an object (the same bytes in the header segments and in a directory entry)
std::vector<uint8_t> drmGenMotHeader(const DrmMotObject& o);

struct DrmTxConfig {
    int mode = drm::kModeB;      // 0 A, 1 B, 2 C, 3 D, 4 E (DRM+)
    int occupancy = 3;           // Table 48: 0 4.5 kHz, 1 5, 2 9, 3 10, 4 18, 5 20 kHz (mode E: 0, 100 kHz)
    int mscQam = 64;             // 16 or 64; mode E 4 or 16
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
    int audioRateHz = 24000;     // 12000 or 24000 (the core sampling rate of the AAC stream): 5 or 10 audio frames per super frame (mode E: 24000, 5 frames
                                 // in an audio super frame of 200 ms that two logical frames carry)
    bool textMessage = true;
    std::string text = "OnAir DRM test signal";
    int year = 2026, month = 10, day = 6, hour = 12, minute = 0;   // the clock that SDC type 8 reports (advances with the signal)
    bool sendTime = true, sendLanguage = true;
    bool sdcRotate = true;       // the optional SDC entities take turns; false: the SDC is the same block every time
    std::vector<std::pair<int, std::vector<uint8_t>>> extraEntities;   // more SDC entities in the rotation: type, body (the first nibble in the high half of the first byte)
    uint32_t seed = 1;
    // A packet mode data service next to the audio service (modes A to D only): short id 1, stream 1 with equal error protection, DAB domain user application
    // `dataApp` (0 = none; 0x002 slide show, 0x003 broadcast website, 0x004 TPEG, 0x007 EPG). Its units come from dataSource; they are cut into packets of
    // dataPacketLen data bytes (ES 201 980 clause 6.6.1) and dataPackets of them fill a logical frame (400 ms).
    int dataApp = 0;
    int dataPackets = 12;
    int dataPacketLen = 45;
    int dataPacketId = 1;
    bool dataUnitMode = true;            // false: the application information says "single packets"
    std::shared_ptr<DrmDataSource> dataSource;
    int dataLossEvery = 0;               // > 0: every n-th packet is not sent at all (the continuity index jumps)
    int dataBadCrcEvery = 0;             // > 0: every n-th packet is sent with a wrong CRC-16
    int dataOtherIdEvery = 0;            // > 0: every n-th packet slot carries a packet of another Packet Id (an unrelated application)
    std::string dataLabel = "OnAir Data";
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
    static constexpr int kRate = 48000;           // sample rate of the baseband that superFrame() delivers (modes A to D)
    int sampleRate() const;                       // kRate, mode E 192000
    int superFrameSamples() const;                // samples of one transmission super frame (3 frames = 1.2 s; mode E 4 frames = 400 ms)
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
std::unique_ptr<ModeSynth> makeDrmSynth(const DrmTxConfig& tc, const SynthConfig& cfg, double sampleRate);
// The transmitter configuration and audio source that the synthetic source builds from a SynthConfig (the receiver tests build the same to know what was sent)
DrmTxConfig drmTxConfigFromSynth(const SynthConfig& sc);
std::unique_ptr<DrmAudioSource> drmAudioFromSynth(const SynthConfig& sc, const DrmTxConfig& tc);

} // namespace dect2
