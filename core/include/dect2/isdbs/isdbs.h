// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISDB-S (Japanese BS satellite television, ARIB STD-B20): the frame, the TMCC control information and a test signal generator.
//
// Written from ITU-R BO.1408-1 (the free ITU description of ISDB-S, Annex 1 and its Appendix 1 with the BS parameters), ARIB TR-B15
// (operational guidelines) and ARIB STD-B21 (receiver); not from the text of ARIB STD-B20 (Japanese, paid) and not from any other
// implementation. docs/ISDBS.md lists, item by item, what comes from which document and what is still this code's own reading. In short,
// from the documents: 28.86 Msymbol/s, roll-off 0.35; a frame of 39936 symbols = a 192 symbol header (TMCC with its sync words, BPSK
// rate 1/2) and 38976 data symbols with a 4 symbol BPSK burst after every 203; 48 slots of one 204 byte packet each (203 sent, the sync
// byte is not); 8 frames a super-frame; the slot groups in falling order of efficiency with dummy slots for the less efficient ones;
// sync words 1B95 / A340 / 5CBF; TMCC of 384 bits with RS(64,48); energy dispersal 1 + x^14 + x^15 started at the second byte of each
// super-frame; inter-frame byte interleaving of depth 8; one K = 7 (171,133) code over the whole frame with the DVB-S style puncturing
// restarted at each group; TC8PSK as the rate 1/2 code on one bit of each pair plus one uncoded bit, and the 8PSK / QPSK / BPSK maps.
// Inferred here (not stated by those documents): the bit order inside some TMCC fields, where the dummy slots sit inside a group, whether
// the PRBS clocks over the bytes it skips, the burst PN sequence (only "9th order PN" is known), the 2 super-frame TMCC lead.
// Nothing in the receiver has been tried on a real BS transponder.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace isdbs {

constexpr double kSymbolRate = 28.860e6;   // symbols per second (BO.1408 Table 4: 28.9 MBd in 34.5 MHz)
constexpr double kRollOff = 0.35;
constexpr int kFrameSyms = 39936;          // per frame, header and bursts included (BO.1408 Table 3)
constexpr int kHeaderSyms = 192;           // 96 bits (W1, 64 TMCC bits, W2 / W3) through the rate 1/2 code, BPSK
constexpr int kHeaderBits = 96;
constexpr int kDataSyms = 38976;           // main signal symbols of a frame (Table 3: I_D)
constexpr int kBlocks = 192;               // a burst after every 203 data symbols
constexpr int kBlockData = 203;
constexpr int kBurstSyms = 4;
constexpr int kBlockSyms = kBlockData + kBurstSyms;
constexpr int kSlots = 48;                 // per frame (Table 3: N)
constexpr int kSlotBytes = 203;            // bytes sent of a 204 byte packet (the 0x47 is not sent)
constexpr int kSlotSyms = kDataSyms / kSlots;   // 812: the air time of one slot, whatever its modulation
constexpr int kFramesPerSuper = 8;         // Table 3: M, also the interleaving depth
constexpr int kMaxStreams = 8;             // relative transport stream numbers 0..7 (Table 3: TM)
constexpr int kMaxGroups = 4;              // modulation-code combinations at the same time (Table 3: CM)
constexpr int kTmccBits = 384;             // TMCC information of a super-frame
constexpr int kTmccCoded = 512;            // after RS(64,48): 64 bits in each frame
constexpr uint16_t kW1 = 0x1B95, kW2 = 0xA340, kW3 = 0x5CBF;   // frame sync; end of the first frame's block; end of the others (BO.1408 6.3)
constexpr double kFrameSeconds = kFrameSyms / kSymbolRate;
static_assert(kHeaderSyms + kBlocks * kBlockSyms == kFrameSyms, "frame layout");
static_assert(kBlocks * kBlockData == kDataSyms && kSlots * kSlotSyms == kDataSyms, "data layout");

// Modulation and code of a slot group: the 4 bit word of the TMCC (BO.1408 Table 1). 15 marks an unused group (count 0).
enum Mod { kUnused = 0, kBpsk12 = 1, kQpsk12 = 2, kQpsk23 = 3, kQpsk34 = 4, kQpsk56 = 5, kQpsk78 = 6, kTc8psk23 = 7, kDummyWord = 15 };
const char* modName(int mod);
bool isMod(int mod);                       // one of the seven
int efficiencyRank(int mod);               // 0 for TC8PSK (the most efficient) .. 6 for BPSK 1/2; the order the groups are sent in
// Slots come in units: of `slotUnit` assigned slots the first `slotActive` carry packets and the rest are dummy slots (not sent). The unit
// sizes follow from the efficiencies (a slot is 812 symbols; TC8PSK carries one packet in it); that the active ones come first is read off
// BO.1408 Fig. 4 and is not otherwise stated.
int slotUnit(int mod);                     // 1, 2 (QPSK 1/2), 3 (2/3), 4 (3/4, BPSK), 6 (5/6), 8 (7/8)
int slotActive(int mod);                   // 1, 1, 2, 3, 5, 7, 1
// trellis steps (input bits of the convolutional code) of a group of `count` slots (a whole number of units): 1624 a packet, but 812 a
// TC8PSK packet (only one bit of each pair is coded)
int groupSteps(int mod, int count);
// transport stream rate (bit/s, 188 byte packets) of one assigned slot
double slotTsBitrate(int mod);

// ---- TMCC (BO.1408 6.1, Table 3; TR-B15 6.2). 384 bits: change 5, four times (mode 4, slot count 6), 48 relative TS numbers of 3 bits,
// eight TS ids of 16 bits, emergency flag 1, uplink 4, reserved 62. The field widths are from the documents; the order inside a
// combination (word first, then count) and of the last three fields is this code's reading.
struct Tmcc {
    bool valid = false;
    int change = 0;                        // 0..31, steps when the TMCC is renewed (TR-B15 6.2.1)
    int mode[kMaxGroups] = {0, 0, 0, 0};   // Mod of the groups, most efficient first; kUnused for an unused group
    int count[kMaxGroups] = {0, 0, 0, 0};  // slots of each group, dummy slots included (TR-B15 6.2.2); the groups follow each other from slot 0
    std::array<uint8_t, kSlots> slotTs{};  // relative TS number of each slot, dummy slots included (TR-B15 6.2.3)
    std::array<uint16_t, kMaxStreams> tsId{};   // TS id of each relative number, 0xFFFF when not used (TR-B15 6.2.4)
    bool ews = false;                      // start control for emergency warning broadcasting
    int uplink = 0;                        // 4 bits (site diversity, TR-B15 Table 6.4.1)
    Tmcc() { tsId.fill(0xFFFF); }
    bool operator==(const Tmcc& o) const;
    bool operator!=(const Tmcc& o) const { return !(*this == o); }
    int slotMod(int slot) const;           // Mod of a slot, kUnused when outside every group
    bool slotCarries(int slot) const;      // a packet is sent in it (not a dummy slot)
    int slotsOf(int rel) const;            // slots (dummies included) of relative stream `rel`
    int packetsOf(int rel) const;          // packets of `rel` in a frame
    int modOf(int rel) const;              // modulation of the first slot of the stream (-1 none)
};
std::array<uint8_t, kTmccBits> tmccPack(const Tmcc& t);
// Reads the 384 bits back. False when the fields are not consistent (a word that is not in Table 1, a count that is not a whole number of
// units, more than 48 slots).
bool tmccUnpack(const uint8_t* bits384, Tmcc& t);
// RS(64,48) (the RS(255,239) code of the main signal, shortened) over the 48 bytes, MSB first: 512 bits, 64 of them in each frame
void tmccEncode(const uint8_t* bits384, uint8_t* out512);
// Corrects and checks; returns the corrected bytes (>= 0) or -1
int tmccDecode(const uint8_t* bits512, uint8_t* out384);

// ---- energy dispersal: 1 + x^14 + x^15, registers loaded with 100101010000000 (BO.1408 4, Fig. 5; B21 4.4.5 "15th M-sequence")
void prbs15(uint8_t* bits, size_t n);      // the first n output bits after loading
// ---- the burst: BPSK of a PN sequence of the 9th order (TR-B15 6.5). Which one and where it starts is not known: this generator uses
// 1 + x^5 + x^9 started with all ones at every frame, and the receiver does not rely on it (TR-B15 6.5 (2): it may carry data one day).
const std::vector<float>& burstSequence();  // 768 values +-1 of one frame
// ---- the header: W1, the 64 TMCC bits of this frame, W2 (first frame of a super-frame) or W3, through the rate 1/2 code, BPSK
// The 20 coded symbols of W1's last 10 bits (header symbols 12..31) and of W2's (172..191; W3 gives their negatives) do not depend on
// anything else (BO.1408 6.3: the first 6 bits of each word end the code's memory of what came before).
const std::array<float, 20>& w1Tail();
const std::array<float, 20>& w2Tail();
constexpr int kW1TailPos = 12, kW2TailPos = 172;
// 8PSK point of the label (W, Y, X) = bits 2, 1, 0 (BO.1408 Fig. 15 a)
cf32 psk8Point(int label);

// ---- test signal
struct StreamCfg {
    int mod = kTc8psk23;
    int slots = 8;                         // assigned slots, dummies included: a whole number of slotUnit(mod)
    uint16_t tsId = 0x4010;
    std::function<void(uint8_t*)> src;     // fills one 188 byte packet; null: null packets
};
struct GenConfig {
    double sampleRate = 57.72e6;
    double symbolRate = kSymbolRate;
    double snrDb = 200;                    // Es/N0, 200 = none
    double cfoHz = 0;
    double clockPpm = 0;                   // the symbol clock runs this much fast
    double level = 0.2;                    // rms of the clean signal
    uint32_t seed = 1;
    // up to 8 streams in at most 4 modulations, together exactly 48 slots (all slots are always allocated on the air: TR-B15 6.1.2);
    // they are sent most efficient first, the relative numbers follow that order
    std::vector<StreamCfg> streams;
    int changeAfterSuper = -1;             // >= 0: `streams2` from this super-frame on; the TMCC announces it two super-frames before
    std::vector<StreamCfg> streams2;
    bool burstData = false;                // random BPSK on the bursts instead of the PN sequence (TR-B15 6.5 allows data there one day)
};
class Generator {
public:
    explicit Generator(const GenConfig& cfg);
    ~Generator();
    bool ok() const;                       // false: the configuration does not fit (see error())
    const std::string& error() const;
    void generate(cf32* out, size_t n);    // the next n samples
    const Tmcc& tmcc() const;              // the layout of the frames being sent
    uint64_t packetsSent() const;
    uint64_t framesSent() const;
    double sampleRate() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
// The Tmcc for a list of streams (false when it does not fit); the streams are sorted most efficient first (stable), see GenConfig
bool tmccFromStreams(const std::vector<StreamCfg>& s, int change, Tmcc& t, std::string* err = nullptr, std::vector<int>* order = nullptr);

}  // namespace isdbs
}  // namespace dect2
