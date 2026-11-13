// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL voice codec 4 ("KV4"): the owner's own low-rate speech codec for the KAAL voice slots (docs/KAAL-CODEC.md).
//
// 8 kHz core: LPC analysis-by-synthesis coder (LSF prediction, fractional pitch, pyramid-VQ pulse codebook, closed-loop gains,
// adaptive postfilter). At 16 kHz the band above ~3.7 kHz is split off and sent as a parametric noise envelope (high band).
// Speech frames have a fixed size per profile; silence (VAD/DTX) sends a small SID frame or nothing, so the slot can carry data.
// The payload bits are ordered by importance: class A (envelope, pitch, gains: protect most), class B, class C (pulses).
//
// Containers (the KAAL PHY) carry the codec id, profile and frame length out of band; a frame's type follows from its length:
// sidBytes -> SID, 0 -> no data (DTX), payloadBytes -> speech. A frame that did not arrive is an erasure (concealed).
//
// Typical use:
//   kaalvoice::Config c; c.profile = kaalvoice::Profile::K6_0;
//   kaalvoice::Encoder enc(c); kaalvoice::Decoder dec(c);
//   std::vector<uint8_t> f; kaalvoice::FrameType t = enc.encode(pcm, f);   // pcm: info().frameSamples samples
//   dec.decode(t, f.data(), f.size(), out);                                // or dec.decode(..., nullptr, 0, out) for a lost frame
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace kaalvoice {

enum class CodecId : uint8_t {
    LegacyV1 = 1,     // KaalCodec (spec v0.1): 16 kHz, 20 ms, 320 bits
    LegacyV3 = 3,     // KaalCodec3: 16 kHz, 20 ms, 240 / 320 / 480 / 640 bits (old KAAL signals)
    KV4 = 4,          // this codec
};

// Speech payload rates per slot (bits per 20 ms in brackets). The four-slot KAAL v4 PHY (docs/KAAL-SLOTS.md) carries 40 ms payloads:
// V80 = K8_0, V60 = K6_0, V48 = K4_8, V32R = K3_2. HQ12 / HQ16 are single-slot high-quality profiles.
enum class Profile : uint8_t { K4_8 = 0, K6_0 = 1, K8_0 = 2, HQ12 = 3, HQ16 = 4, K3_2 = 5, Count = 6 };
int profileBitsPer20ms(Profile p);     // 96, 120, 160, 240, 320, 64
const char* profileName(Profile p);    // "4.8", "6.0", "8.0", "12", "16", "3.2"

enum class FrameType : uint8_t { Speech = 0, Sid = 1, NoData = 2 };

// Damaged importance classes, from the container's per-class checks (decode()).
enum : uint32_t { kBadA = 1u, kBadB = 2u, kBadC = 4u };

// Stages of the decoder-side speech enhancer (docs/KAAL-ENHANCE.md), each switchable for A/B tests (Config::enhanceStages).
// Receiver only: the bit stream does not change.
enum : uint32_t {
    kEnhPitch = 1u << 0,         // adaptive long-term (pitch) post-filter
    kEnhFormant = 1u << 1,       // adaptive formant post-filter with spectral-tilt compensation
    kEnhBwe = 1u << 2,           // high band: harmonic extension from the low band for voiced speech (noise for unvoiced)
    kEnhHbSmooth = 1u << 3,      // high band: temporal smoothing of the envelope, low-fluctuation noise (no chirping)
    kEnhLoudness = 1u << 4,      // energy restoration of the sparse-pulse excitation (the codec's loudness loss)
    kEnhComfort = 1u << 5,       // comfort-noise shaping (smoothed spectrum and level)
    kEnhDeclick = 1u << 6,       // de-clicking at the first good frame after a loss
    kEnhNoiseReduce = 1u << 7,   // gentle Wiener noise reduction from the decoder's own noise estimate (adds 5 ms delay)
    kEnhDefault = kEnhPitch | kEnhFormant | kEnhBwe | kEnhHbSmooth | kEnhLoudness | kEnhComfort | kEnhDeclick,
};

struct Config {
    int sampleRate = 16000;         // 8000 or 16000 (encoder input and decoder output)
    int frameMs = 20;               // 20 or 40
    Profile profile = Profile::K6_0;
    int payloadBits = 0;            // 0: the profile's budget; otherwise an exact speech-frame budget (custom slot sizes), see valid()
    bool dtx = true;                // encoder: voice activity detection + discontinuous transmission
    bool noiseSuppress = false;     // encoder: input noise suppression (adds 8 ms of delay)
    bool postfilter = true;         // decoder: adaptive formant / pitch postfilter (also gates the enhancer's post-filter stages)
    // decoder: speech enhancer (docs/KAAL-ENHANCE.md). -1 auto: on below 7 kbit/s (3.2 / 4.8 / 6.0), off for 8.0 / HQ12 / HQ16;
    // 0 off (the plain KV4 decoder), 1 on. When it runs, its post-filter replaces the core postfilter above.
    int enhance = -1;
    uint32_t enhanceStages = kEnhDefault;
    // Fixed-size containers (the KAAL v4 slots): every Speech and Sid frame is payloadBytes long and its first bit tells them apart
    // (0 speech, 1 SID); speech then has payloadBits - 1 bits. NoData stays empty (the slot carries data instead).
    bool inBandType = false;
};

struct Info {
    int frameSamples = 0;           // samples per frame at sampleRate
    int payloadBits = 0;            // bits of a speech frame (the last byte is zero-padded)
    int payloadBytes = 0;
    int sidBits = 40, sidBytes = 5;
    int classBits[3] = {0, 0, 0};   // A, B, C; A + B + C + padBits = payloadBits; class A occupies the first classBits[0] bits
    int padBits = 0;
    int delaySamples = 0;           // decoder output lags the encoder input by this many samples (noise suppressor included)
};

// Checks a configuration; payloadBits (custom) must lie in [64 * frameMs / 20, 400 * frameMs / 20] (64 needs 40 ms frames).
bool valid(const Config& c, std::string* why = nullptr);
Info info(const Config& c);
// The frame type a received payload length stands for (out-of-band typing): 0 -> NoData, sidBytes -> Sid, payloadBytes -> Speech.
// Any other length returns false (treat as an erasure).
bool typeOfLength(const Config& c, size_t nbytes, FrameType* t);

class Encoder {
public:
    explicit Encoder(const Config& c);
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    const Info& info() const;
    // pcm: info().frameSamples samples. out receives payloadBytes (Speech), sidBytes (Sid) or nothing (NoData).
    FrameType encode(const int16_t* pcm, std::vector<uint8_t>& out);
    void reset();
    // Statistics for the caller (rate control, UI): fraction of frames sent as speech so far.
    double speechFraction() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Decoder side information of one frame, handed to an external post-filter (a future neural post-filter, docs/KAAL-ENHANCE.md
// section 6). Everything is what the decoder already has: no extra bits.
struct EnhanceSide {
    enum Kind : uint8_t { Speech = 0, Concealed = 1, ComfortNoise = 2 };
    int sampleRate = 16000, frameSamples = 0;
    Kind kind = Speech;
    bool firstAfterGap = false;      // first speech frame after a loss, comfort noise or reset
    int lostRun = 0;                 // consecutive lost frames up to this one (0 for a good frame)
    bool enhanced = false;           // the classical enhancer ran on this frame
    int subframes = 0, subLen8 = 0;  // core subframes, their length in 8 kHz samples
    float lpc[16][11] = {};          // quantised LPC per subframe (8 kHz, lpc[s][0] = 1)
    float lag[16] = {};              // pitch lag per subframe in 8 kHz samples
    float pitchGain[16] = {}, codeGain[16] = {};
    float voicing = 0;               // mean pitch gain of the frame, 0 .. ~1.2
    int hbBlocks = 0;                // 5 ms high-band blocks (16 kHz only)
    float hbLevelDb[8][6] = {};      // their band levels (dB per 100 Hz MDCT bin; -100 = none)
    float noiseDb = -100;            // the decoder's background noise estimate (low band, dB per sample)
};

// A post-filter stage behind the classical enhancer (for example a neural post-filter). It sees the frame as floats at the output
// rate (int16 scale), after the classical stages and before the conversion to int16, and must not add delay.
class ExternalPostFilter {
public:
    virtual ~ExternalPostFilter() = default;
    virtual void reset() {}
    virtual void process(const EnhanceSide& side, float* pcm) = 0;
};

class Decoder {
public:
    explicit Decoder(const Config& c);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    const Info& info() const;
    // data == nullptr (or kBadA in badClasses) is a lost frame: concealed from the past. kBadB / kBadC: those bits are replaced by
    // neutral values (B) or a noise excitation (C). pcm receives info().frameSamples samples.
    void decode(FrameType t, const uint8_t* data, size_t nbytes, int16_t* pcm, uint32_t badClasses = 0);
    void erasure(int16_t* pcm) { decode(FrameType::Speech, nullptr, 0, pcm); }
    void reset();
    // Optional stage after the classical enhancer (nullptr removes it). Runs whether or not the classical enhancer is on.
    void setPostFilter(std::shared_ptr<ExternalPostFilter> pf);
    // Side information of the last decoded frame.
    const EnhanceSide& lastSide() const;
    bool enhancing() const;   // the classical enhancer is on for this configuration
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// One interface for the current codec and the old KAAL codecs, so a receiver can follow whichever codec id the container names.
// Legacy codecs take 16 kHz / 20 ms only; their payloadBits is the frame size in bits (KaalCodec3: 240, 320, 480 or 640; v1: 320),
// packed MSB first into bytes; they have no SID / DTX (every frame is Speech).
class FrameCodec {
public:
    virtual ~FrameCodec() = default;
    virtual CodecId id() const = 0;
    virtual const Info& info() const = 0;
    virtual FrameType encode(const int16_t* pcm, std::vector<uint8_t>& out) = 0;
    virtual void decode(FrameType t, const uint8_t* data, size_t nbytes, int16_t* pcm, uint32_t badClasses = 0) = 0;
};
// nullptr when the combination is not supported.
std::unique_ptr<FrameCodec> makeFrameCodec(CodecId id, const Config& c);

}  // namespace kaalvoice
}  // namespace dect2
