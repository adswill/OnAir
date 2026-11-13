// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NFC / RFID physical layer at 13.56 MHz: recover byte frames from complex baseband (carrier mixed to 0 Hz).
// Receive only; nothing is transmitted here.
//
// What it decodes, as far as the public specs allow:
//   * ISO/IEC 14443 Type A reader frames: 100% ASK, modified Miller, 106 kbit/s (fc/128). Short frames (7 bits) and standard
//     frames (8-bit bytes, least significant bit first, odd parity after each byte).
//   * ISO/IEC 14443 Type A tag frames: load modulation on the 847.5 kHz subcarrier (fc/16), OOK, Manchester, 106 kbit/s.
//   * ISO/IEC 14443 Type B tag frames: BPSK on the 847.5 kHz subcarrier, NRZ-L, 106 kbit/s (structure only).
//   * ISO/IEC 15693 reader frames: 100% / 10% ASK with 1-of-4 and 1-of-256 pulse-position coding.
//   * ISO/IEC 15693 tag frames: load modulation on the 423.75 kHz subcarrier (fc/32), Manchester, high data rate.
//
// The chain runs on complex baseband at fs (the subcarrier at +-847.5 kHz must fit, so fs is a few Msps). It is clean-room from the
// public timing and coding rules; the exact envelope shapes, filter bandwidths and thresholds are the author's own choices.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::nfc {

// Carrier and derived frequencies (Hz).
constexpr double kFc = 13.56e6;
constexpr double kSubA = kFc / 16.0;       // 847.5 kHz, Type A / B load-modulation subcarrier
constexpr double kSubV = kFc / 32.0;       // 423.75 kHz, ISO 15693 single subcarrier
constexpr double kBitRate = kFc / 128.0;   // 105.9375 kbit/s (the "106 kbit/s" of the spec)
constexpr double kEtu = 128.0 / kFc;       // one bit, seconds (elementary time unit)
constexpr double kPauseUs = 128.0 / kFc * 1e6 / 2.0;   // a Type A reader pause is about half an etu wide here

enum class Proto { TypeA, TypeB, V15693 };

// One recovered frame as bytes, before the protocol layer names it.
struct RawFrame {
    Proto proto = Proto::TypeA;
    bool fromReader = true;      // a reader (PCD) frame or a tag (PICC) answer
    int bits = 0;                // exact bit count (7 for Type A short frames, else 8 * bytes)
    bool parityOk = true;        // Type A: every byte's odd parity checked out
    std::vector<uint8_t> bytes;  // payload bytes (CRC bytes included; parity bits removed)
    double timeSec = 0;          // start time of the frame within the stream
    float levelDb = -200;        // field / subcarrier level, dBFS
};

// The demodulator. feed() is called with baseband blocks; it calls onFrame() for each recovered frame.
class Demod {
public:
    explicit Demod(double fs);
    void setFs(double fs);
    double fs() const { return fs_; }
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    void setFrameCallback(std::function<void(const RawFrame&)> cb) { onFrame_ = std::move(cb); }
    // Level of the strongest subcarrier seen since the last reset (dBFS), for the "is anything there" meter.
    float peakSubcarrierDb() const { return peakSubDb_; }
    float carrierDb() const { return carrierDb_; }

private:
    void processReaderEnvelope(const float* env, size_t n, double t0);
    void processTagSubcarrier(const std::complex<float>* sub, size_t n, double t0);

    double fs_ = 0;
    std::function<void(const RawFrame&)> onFrame_;
    // running state is kept in the .cpp implementation struct
    struct State;
    std::shared_ptr<State> st_;
    float peakSubDb_ = -200, carrierDb_ = -200;
};

// Decode the modified-Miller reader bit sequence X/Y/Z into bytes (least significant bit first, odd parity after each byte).
// `seq` is the per-etu symbol: 0 = Y (no pause), 1 = X (pause in 2nd half), 2 = Z (pause in 1st half). Returns the bytes and the
// exact bit count; sets parityOk. Short frames (7 bits, no parity) are returned when the sequence is 7 data symbols with no EOF byte.
std::vector<uint8_t> millerSeqToBytes(const std::vector<uint8_t>& seq, int& bits, bool& parityOk);

// Encode bytes to the X/Y/Z symbol sequence a reader would send (used by the generator and the round-trip test).
std::vector<uint8_t> bytesToMillerSeq(const std::vector<uint8_t>& bytes, int bits);

// Byte <-> bit helpers with the ISO/IEC 14443 framing (least significant bit first, odd parity after each whole byte).
// Short frames (bits == 7) carry no parity. Used by the tag (Manchester) path and the generator.
std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes, int bits);   // returns one entry (0/1) per transmitted bit incl. parity
std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits, int& outBits, bool& parityOk);

}  // namespace dect2::nfc
