// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4 (docs/KAAL-NEXT.md, docs/KAAL-SLOTS.md): 16 ksym/s single carrier, RRC 0.25 (20 kHz), 3 samples per symbol at
// 48 kHz. Superframe 40 ms = 640 symbols: 16-symbol up-chirp + 16-symbol down-chirp (sync, timing and carrier offset in one step; the
// down-chirp is negated in superframe 0 of every 10), 32 QPSK control symbols (one rate-1/2 LDPC word per 10 superframes), four slots of
// 144 symbols (8 pilots + 136 data) carrying a voice payload or data. Slot words: the KAAL (640,320) LDPC shortened to k and
// circular-buffer rate-matched to 544 (16-QAM) or 272 (QPSK) coded bits; the profile follows from k (kaal_slots.h).
#pragma once
#include "dect2/kaal/kaal_beacon.h"
#include "dect2/kaal/kaal_slots.h"
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::kaal::v4 {

using cf = std::complex<float>;

constexpr int kVersion = 4;
constexpr int kFs = 48000, kRs = 16000, kSps = 3;
constexpr int kSfSyms = 640, kSfSamples = kSfSyms * kSps;   // 1920
constexpr int kChirp = 16;                                 // symbols per chirp (up, then down)
constexpr int kCtrlStart = 32, kCtrlSyms = 32;             // QPSK control symbols per superframe
constexpr int kCtrlCycle = 10;                             // superframes per control word (400 ms)
constexpr int kCtrlMsgBytes = 33;                          // message bytes per control word
constexpr int kSlot0 = 64, kSlotSyms = 144, kSlotPilots = 8, kPilotStep = 18, kSlotData = 136;
constexpr int kSlotCoded16 = kSlotData * 4, kSlotCodedQ = kSlotData * 2;   // 544, 272
constexpr double kOccupiedHz = kRs * 1.25;                 // 20 kHz
constexpr float kTxRms = 0.25f;                            // output rms (16-QAM peaks stay below about 0.7)
constexpr int slotStart(int s) { return kSlot0 + kSlotSyms * s; }
constexpr bool isPilot(int i) { return i % kPilotStep == 0 && i / kPilotStep < kSlotPilots; }   // i = symbol index in the slot

enum CtrlMsg : uint8_t { kMsgBeacon = 0, kMsgSlotCall = 1, kMsgText = 2, kMsgEmpty = 15 };

struct ControlWord {
    int version = kVersion;
    Profile map[kSlots] = {kIdle, kIdle, kIdle, kIdle};   // what the slots carry (a hint for the receiver)
    uint8_t msgType = kMsgEmpty;
    uint8_t part = 0, parts = 1;                          // text: chunk index and count (up to 16 chunks)
    uint8_t msg[kCtrlMsgBytes] = {};
};

// ---- building blocks (also used by the tests) ----
std::vector<cf> upChirp();                                 // 16 unit-modulus symbols, frequency -Rs/2 .. +Rs/2
cf pilotAt(int slot, int i);                               // the known pilot of slot position i (isPilot(i))
std::vector<double> rrcTaps3();                            // unit energy, 3 samples per symbol, span 10 symbols (31 taps)
uint16_t crc16(const uint8_t* bits, size_t n);             // CRC-16-CCITT (0x1021, init 0xFFFF) over bits (one per byte)
// Slot word: payload (+ data header + CRC for data profiles) -> LDPC -> rate matching -> scrambler -> interleaver. Returns the coded bits.
std::vector<uint8_t> encodeSlot(int slot, const SlotPayload& p);
// llr: coded LLRs of the slot (544 or 272, positive = bit 0) in transmit order. Tries profile p only. True when the word decoded (and, for
// data, the CRC passed). post (optional, size of llr) receives the decoder's a-posteriori LLRs in transmit order (for iterative estimation).
bool decodeSlot(int slot, const float* llr, Profile p, SlotPayload& out, int maxIter = 40, std::vector<float>* post = nullptr);
std::vector<uint8_t> encodeControl(const ControlWord& w);  // 640 coded bits in transmit order (64 per superframe)
bool decodeControl(const float* llr, ControlWord& w, int maxIter = 50);
// ---- transmitter ----
class FrameTx {
public:
    FrameTx();
    ~FrameTx();
    FrameTx(const FrameTx&) = delete;
    FrameTx& operator=(const FrameTx&) = delete;
    void configure(double outRate);                        // 48000 normally; other rates are resampled
    void reset();
    // Voice: give a slot a voice profile and push one payload (payloadBits(p) bits packed MSB first, 8 per byte, as SlotPayload::bits) per 40 ms. A voice slot with no payload
    // queued for a superframe carries data or fill in that superframe. kIdle = the slot is a data slot.
    void setSlotProfile(int slot, Profile p);
    void pushVoice(int slot, const uint8_t* bits);
    size_t voiceQueued(int slot) const;
    void setDataRobust(bool on);                           // data slots as D26R (QPSK) instead of D68
    void queueText(const std::string& text);               // sent in free data slots and, when none is free, in the control channel
    size_t textQueuedBytes() const;
    void setBeacon(const BeaconFields& f);                 // repeated in the control channel (every 4th control word)
    void setSlotCallsign(int slot, const std::string& call);
    void setControlText(bool on);                          // also send queued text in control words (default on)
    // Appends one superframe (1920 samples at 48 kHz, or the resampled equivalent).
    void superframe(std::vector<cf>& out);
    uint64_t superframes() const;
    // Statistics of what was sent (tests): payloads per profile, text bytes sent in slots / control.
    uint64_t sentSlots(Profile p) const;
    uint64_t textBytesSent() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- receiver ----
struct RxSuperframe {
    uint64_t index = 0;            // superframes since lock
    SlotPayload slot[kSlots];
    float esn0Db = 0;
    double cfoHz = 0;
    int ctrlPos = -1;              // position in the control cycle (0 = marker), -1 not yet known
};
struct RxStatsV4 {
    bool locked = false;
    uint64_t superframes = 0, slotsOk = 0, slotsBad = 0, ctrlOk = 0, ctrlBad = 0, locks = 0;
    float esn0Db = 0;
    double cfoHz = 0;
    double clockPpm = 0;
    double lockTimeS = -1;         // seconds of input until the first decoded slot
    uint64_t samplesIn = 0;
    Profile lastProfile[kSlots] = {kIdle, kIdle, kIdle, kIdle};
    int iterativeSaves = 0;        // slot words that decoded only after the decision-directed re-estimation
    uint64_t equalised = 0;        // superframes that went through the multipath equaliser
};

class FrameRx {
public:
    FrameRx();
    ~FrameRx();
    FrameRx(const FrameRx&) = delete;
    FrameRx& operator=(const FrameRx&) = delete;
    void configure(double inRate);
    void reset();
    void setIterative(bool on);    // decision-directed re-estimation after a failed decode (default on)
    void setDdTracking(bool on);   // decision-directed phase smoothing over the data symbols (default on)
    void setEqualiser(bool on);    // MMSE equaliser from the chirp channel estimate when there is multipath (default on)
    void feed(const cf* x, size_t n);
    bool popSuperframe(RxSuperframe& out);
    bool pollText(std::string& text);
    bool pollBeacon(BeaconFields& f);
    bool pollSlotCall(int& slot, std::string& call);
    ControlWord lastControl() const;
    RxStatsV4 stats() const;
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::kaal::v4
