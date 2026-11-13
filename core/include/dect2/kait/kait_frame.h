// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT, the amateur image burst (spec: KAIT-SPEC-v0.1): framing and the transmit side.
//
// One burst of about 10 s: [sweep pair] [header] [layer 0 .. layer N-1] [sweep pair]. Single carrier, 158 ksym/s, root-raised-cosine 0.2 (occupied 189.6 kHz),
// 4 samples per symbol (632 ksps internal rate; the Resampler of KAPP converts to the radio rate). A pilot (+-1) follows every 31 data symbols over the whole burst
// (header and layers) for phase, gain and noise tracking. Sweeps: a linear chirp up then down (40 ms each, +-85 kHz), at the start and at the end: they give burst
// timing, carrier offset (up/down pair) and the sample clock error (start versus end), and are the station signature on a waterfall.
//
// The image stream (kait_codec.h) is embedded, so it is cut into layers: layer i has its own modulation, LDPC rate (n = 7680) and repetition (BPSK only) and
// carries the next bytes of the stream. Robust layers come first. A receiver decodes layers in order until a block fails, and shows the picture the bytes so far give.
// Header: 4 bit version, callsign (9 x 6 bit), width, height, wavelet levels, top bitplane, profile, layer table, codec decision count, CRC-16; K = 7 rate 1/4
// convolutional code, BPSK, sent 16 times (soft combined).
#pragma once
#include "dect2/kait/kait_codec.h"
#include "dect2/kris/kris_common.h"
#include "dect2/kris/kris_fec.h"
#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::kait {

using cf = std::complex<float>;

constexpr double kSymRate = 158000.0;
constexpr double kBandwidthHz = 190000.0;         // the bandwidth the SNR figures refer to
constexpr int kSps = 4;
constexpr double kInternalRate = kSymRate * kSps; // 632 ksps
constexpr float kRrcBeta = 0.2f;
constexpr int kRrcSpan = 12;
constexpr int kLdpcN = 7680;
constexpr int kBlockCrcBits = 16;
constexpr int kPilotGroup = 31;                   // data symbols between pilots
constexpr int kPilotPeriod = kPilotGroup + 1;
constexpr int kSweepSamples = 25280;              // 40 ms
constexpr double kSweepHalfBandHz = 85000.0;
constexpr int kGuardSamples = 48;                 // between a sweep pair and the symbols
constexpr int kMaxLayers = 8;
constexpr int kHeaderInfoBits = 290;
constexpr int kHeaderCodedBits = (kHeaderInfoBits + 6) * 4;   // 1184
constexpr int kHeaderRepeat = 16;
constexpr int kHeaderDataSyms = kHeaderCodedBits * kHeaderRepeat;   // 18944
constexpr int kMaxCallsign = 9;
constexpr double kBurstSeconds = 10.0;

struct LayerSpec {
    kris::Mod mod = kris::Mod::Bpsk;
    kris::LdpcRate rate = kris::LdpcRate::R1_4;
    int rep = 1;        // BPSK repetitions of the whole coded block
    int blocks = 0;
};

// Per-layer shape of a profile; shares are percent of the data symbols.
struct LayerShape {
    kris::Mod mod;
    kris::LdpcRate rate;
    int rep;
    int sharePercent;
    float needEsN0Db;   // Es/N0 for the block error rate ~1e-2, measured with this PHY (KAIT-MEASURED-v0.1.md); an estimate where not measured
};
struct Profile {
    int id;
    const char* name;
    int nLayers;
    std::array<LayerShape, kMaxLayers> layers;
};
constexpr int kNumProfiles = 4;
extern const std::array<Profile, kNumProfiles> kProfiles;
const Profile* profileById(int id);

int infoBitsPerBlock(kris::LdpcRate r);               // K of the n = 7680 code
int payloadBytesPerBlock(kris::LdpcRate r);           // (K - 16) / 8
int symbolsPerBlock(const LayerSpec& l);              // 7680 / bits per symbol x rep
long layerDataSymbols(const LayerSpec& l);

struct Header {
    std::string callsign;
    int width = 0, height = 0;
    int levels = 5;
    int topPlane = -1;
    int profileId = 0;
    int nLayers = 0;
    std::array<LayerSpec, kMaxLayers> layers{};
    uint32_t decisions = 0;
    long dataSymbols() const;                         // header + all layers, without pilots
    size_t streamBytes() const;                       // payload bytes of all layers
    long slots() const;                               // data + pilots
    long burstSamples() const;                        // whole burst at the internal rate
    StreamInfo streamInfo() const { return StreamInfo{width, height, levels, topPlane, decisions}; }
};

// callsign: 9 characters, A-Z 0-9 / - . (others become '/'), shorter ones padded
std::vector<uint8_t> headerInfoBits(const Header& h);        // 290 bits, one per byte, CRC-16 included
bool parseHeaderBits(const uint8_t* bits, Header& h);        // false when the CRC or a field is invalid
std::vector<cf> headerSymbols(const Header& h);              // 18944 BPSK data symbols (no pilots)

// The layers for a profile that fill `seconds` of burst. Empty for an unknown profile.
std::vector<LayerSpec> planLayers(int profileId, double seconds = kBurstSeconds);
long slotsForSeconds(double seconds);
long maxDataSymbols(double seconds);

struct BurstPlan {
    Header hdr;
    std::vector<uint8_t> stream;   // hdr.streamBytes() bytes: the codec stream, zero padded
    bool ok = false;
};
// Encodes the picture to the byte capacity of the layers.
BurstPlan planBurst(const Image& img, const std::string& callsign, int profileId, double seconds = kBurstSeconds);
// Takes an already coded stream (shorter than the capacity is padded).
BurstPlan planBurstFromStream(const Encoded& enc, const std::string& callsign, int profileId, double seconds = kBurstSeconds);

// data-symbol index -> slot index (pilots interleaved), and the pilot sign
inline long slotOfData(long d) { return d + d / kPilotGroup; }
inline bool isPilotSlot(long s) { return s % kPilotPeriod == kPilotGroup; }
float pilotSign(long pilotIndex);
std::vector<uint8_t> scramble(uint32_t seed, int nBits);

const std::vector<float>& rrcShape();                        // transmit taps, sum of squares = kSps
std::vector<cf> sweepUp();                                   // kSweepSamples, constant envelope with raised-cosine ends
std::vector<cf> sweepDown();

// Slot symbols of the burst (header, layers, pilots) / the whole burst at 632 ksps, unit average power.
std::vector<cf> buildSlots(const BurstPlan& plan);
std::vector<cf> buildBurst(const BurstPlan& plan);
// where the end sweep starts, in samples from the burst start
long endSweepOffset(const Header& h);

}  // namespace dect2::kait
