// LoRaWAN FSK (data rate DR7: 50 kbit/s GFSK) as LoRaWAN radios send it, both ways: frame coding (length byte, CRC-16, whitening), the airtime of
// a frame, a modulator for the test signal, and a receiver that finds the frames on the FSK channel(s) of a plan in a wide capture. The PHY
// payload inside is an ordinary LoRaWAN frame (lorawan::parse). RECEIVE ONLY and keyless, as the LoRaWAN monitor: nothing is decrypted, no MIC
// is checked, nothing is sent (the modulator only writes samples to memory).
//
// Synthetic tests only; not checked against a real transmitter.
//
// Sources (all public; NOTES of the work item list the versions):
//   LoRa Alliance RP002-1.0.2 Regional Parameters, 4.2 "FSK description": preamble 5 bytes, sync word 0xC194C1, 50000 bit/s, deviation 25 kHz,
//     Gaussian BT 1.0, whitening, CRC-16-CCITT over the length byte and the PHY payload; the DR7 rows of the EU863-870, EU433 and AS923 tables,
//     maximum MACPayload M = 250 bytes at DR7 (PHY payload = M + 5 = 255, which the length byte can still hold).
//   Semtech LoRaMac-node 4.7.0+ (master dcbcfb32, 2024-02-06): RegionEU868 / RegionAS923 / RegionEU433 (DR7 = 50 kbit/s FSK, Radio.SetTxConfig(MODEM_FSK,
//     power, 25000, 0, 50000, 0, 5, false, true, ...)); radio/sx126x/radio.c (GFSK BT 1, sync C1 94 C1, CRC "2 bytes CCITT" = polynomial 0x1021, seed
//     0x1D0F, inverted; whitening seed 0x01FF); radio/sx1276 + boards/sx1276-board.h (RegPacketConfig1 0xD8: variable length, whitening, CRC on,
//     CCITT CRC; RegSyncConfig 0x12: 3 sync bytes C1 94 C1, preamble polarity 0xAA; RegPaRamp left at its reset value: no Gaussian shaping).
//     LR1110 (radio/lr1110/radio.c): the same CRC (0x1D0F, 0x1021, inverted) and whitening seed 0x01FF.
//   SX1276/77/78/79 datasheet rev. 5 (2016): 4.2.13 packet handler (CRC over length byte + payload, then the payload and the CRC whitened; CCITT CRC
//     x^16 + x^12 + x^5 + 1, seed 0x1D0F, complemented; the example CRC code of 7.5; bytes are sent MSB first); figure 37 whitening LFSR x^9 + x^5 + 1.
//   SX1261/2 datasheet rev. 1.2 (2019): 6.2.3.4 whitening (9-bit LFSR x^9 + x^5 + 1, the LSB at the LFSR output is XORed with the MSB of the data,
//     over the length byte, the payload and the CRC), 6.2.3.5 CRC (the CCITT setting 0x1021 / 0x1D0F / inverted); the preamble is sent as 0x55 bytes.
//   RadioLib 7.8.1: SX126x::setWhitening default 0x01FF "for compatibility with SX127x and LoRaWAN"; its LoRaWAN FSK settings (BT 1.0, whitening,
//     5 preamble bytes, sync 0xC194C1).
// Interoperation: the SX1276 has a fixed CCITT CRC (seed 0x1D0F, complemented) and whitening; LoRaMac-node and RadioLib program the SX126x and
// LR11xx (whose reset values differ: whitening seed 0x0100) to the same CRC and whitening seed 0x1FF. The two differ in the preamble (SX1276 0xAA
// after LoRaMac-node's set-up, SX126x 0x55) and in the shaping (LoRaMac-node leaves the SX1276 unshaped): the receiver needs neither.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace lorawan {
namespace fsk {

constexpr int kDataRate = 7;                     // DR7 in every plan that has FSK
constexpr double kBitRate = 50000;               // bit/s
constexpr double kDeviationHz = 25000;           // frequency deviation (single sided)
constexpr double kGaussianBt = 1.0;
constexpr int kPreambleBytes = 5;
constexpr uint32_t kSyncWord = 0xC194C1;         // 3 bytes, sent C1 94 C1
constexpr uint16_t kCrcPoly = 0x1021, kCrcSeed = 0x1D0F;
constexpr uint16_t kWhiteningSeed = 0x01FF;
constexpr size_t kMaxPhyPayload = 255;           // RP002 DR7: M = 250 MACPayload bytes + MHDR + MIC

// CRC-16 CCITT as the radios compute it: polynomial 0x1021, seed 0x1D0F, MSB first, complemented at the end
uint16_t crc16(const uint8_t* p, size_t n);
// XORs n bytes with the PN9 whitening sequence (x^9 + x^5 + 1, the given seed); the same call de-whitens
void whiten(uint8_t* p, size_t n, uint16_t seed = kWhiteningSeed);
// The bytes after the sync word as they go on the air: length byte, PHY payload, CRC (high byte first), all whitened. Empty if n > 255.
std::vector<uint8_t> encode(const uint8_t* phy, size_t n);
// The other way: de-whitens, reads the length, checks the CRC. The PHY payload is returned even when the CRC is wrong.
struct Unpacked {
    bool lengthOk = false;                       // enough bytes for the length the frame claims
    bool crcOk = false;
    uint16_t crcRx = 0, crcCalc = 0;
    std::vector<uint8_t> payload;
};
Unpacked decode(const uint8_t* onAir, size_t n);
// The whole frame as bits (one per byte, MSB first): preamble bytes (0x55 SX126x, 0xAA SX1276), sync word, encode(phy)
std::vector<uint8_t> frameBits(const uint8_t* phy, size_t n, uint8_t preambleByte = 0x55, int preambleBytes = kPreambleBytes);
// Airtime: (preamble + sync + length + payload + CRC) bits / bit rate
size_t frameBitCount(size_t phyLen, int preambleBytes = kPreambleBytes);
double airSeconds(size_t phyLen, int preambleBytes = kPreambleBytes);

// GFSK modulator (test signal): the complex baseband of bits at `rate`, carrier at freqHz, Gaussian BT (<= 0: unshaped FSK), the bit clock off
// by clockPpm, a 1 sent as +deviation. The amplitude ramps up during one bit before the first bit (which starts one bit plus startFrac samples
// after sample 0) and down during one bit after the last, on the first / last bit's tone.
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double rate, double freqHz, double bt, double clockPpm, double startFrac, float amp, double phase0);

// ---- receiver
struct RxFrame {
    double sec = 0;                              // signal time of the frame's start (first preamble bit)
    size_t chan = 0;                             // index in the channel list given to configure()
    double cfoHz = 0;                            // carrier offset from the channel's centre
    double devHz = 0;                            // measured deviation
    bool inverted = false;                       // the tones came the other way round (a 1 below the carrier)
    float levelDb = -200, snrDb = 0;             // level in dB re full scale; SNR in a 100 kHz bandwidth (RP002: Rx bandwidth 50 kHz single sided)
    bool crcOk = false;
    float syncScore = 0;                         // normalised correlation of the preamble end and the sync word, 0..1
    std::vector<uint8_t> payload;                // the PHY payload (de-whitened, without length and CRC)
};

class Receiver {
public:
    Receiver();
    ~Receiver();
    // offsetsHz: the FSK channels' centres in the input (Hz from its 0 Hz); a channel that does not fit (fits()) gets no demodulator
    void configure(double rate, const std::vector<double>& offsetsHz);
    static bool fits(double rate, double offsetHz);
    void reset();
    // Frames with a good CRC are returned; CRC failures after a strong sync are only counted
    void feed(const cf32* x, size_t n, std::vector<RxFrame>& out);
    struct Stats { uint64_t syncs = 0, frames = 0, crcBad = 0; bool on = false; };
    const std::vector<Stats>& stats() const;
    double timeSec() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace fsk
} // namespace lorawan
} // namespace dect2
