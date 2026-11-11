// TETRA Enhanced Data Service (TEDS, ETSI EN 300 392-2 V3.x): identity and signalling only, never user data.
//
// What OnAir reads of TEDS:
//   * the cell's TEDS capabilities from the clear SYSINFO broadcast of its pi/4-DQPSK main carrier: the "extended services broadcast"
//     optional field (tables 21.67 to 21.69): pi/8-D8PSK support (section 1) and 25 / 50 / 100 / 150 kHz QAM support (section 2);
//   * pi/8-D8PSK slots of a phase modulation carrier (table 9.8, training sequences 9.7 / 9.8): detected and their signalling blocks
//     (SCH-P8/HD, SCH-P8/F: CRC-16, RCPC 2/3, (324,19) / (648,23) interleaving, the cell's scrambling) decoded in tetra_rx.cpp;
//   * QAM carriers (clause 5.8 to 5.17): identified from the physical layer only: 8 sub-carriers per 25 kHz spaced 2.7 kHz, 2 400 symbols/s
//     on each, root raised cosine 0.2. classifyCarrier() gives the carrier type, its bandwidth (25 / 50 / 100 / 150 kHz), the sub-carrier
//     count and an estimate of the constellation (4-QAM, or 16 / 64-QAM). The QAM bursts (Annex P symbol maps), their PCCC turbo code and
//     the QAM signalling channels (BSCH-Q, BNCH-Q / SYSINFO-Q, AACH-Q) are NOT decoded: docs/TETRA-3.9.md.
// Receive only. Nothing is decrypted.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::tetra {

// ---------------------------------------------------------------- extended services broadcast (SYSINFO optional field 11)

struct ExtServices {
    int security = 0;          // 8 bits (EN 300 392-7)
    int sdsTl = 0;             // SDS-TL addressing method
    bool gck = false;
    int section = -1;          // 0..3: which section the last 7 bits are
    int bits = 0;              // the section's 7 bits, the first one in bit 6
    // section 1 (table 21.68)
    bool dataPriority = false, extAdvLinks = false, qos = false, d8psk = false, sec2 = false, sec3 = false, sec4 = false;
    // section 2 (table 21.69): bit 0 25 kHz, bit 1 50 kHz, bit 2 100 kHz, bit 3 150 kHz QAM
    int qamMask = 0;
};
// optFlag / optValue as Sysinfo holds them (2 bits, 20 bits); false when the field is not the extended services broadcast
bool parseExtServices(int optFlag, uint32_t optValue, ExtServices& e);
uint32_t encodeExtServices(const ExtServices& e);   // the 20 bit value for optFlag 3 (generator)

// What a cell said about TEDS over time (sections 1 and 2 come in different SYSINFO PDUs): -1 not heard yet
struct TedsCaps {
    int d8psk = -1;
    int qamMask = -1;
    void add(const ExtServices& e);
};

// ---------------------------------------------------------------- carrier identification

enum class CarrierKind : int { Unknown = 0, Dqpsk = 1, D8psk = 2, Qam = 3 };
const char* carrierKindText(CarrierKind k);   // "pi/4-DQPSK", "pi/8-D8PSK", "QAM", "unknown" (modulation names, not translated)

constexpr double kQamSymRate = 2400.0;
constexpr double kQamSpacingHz = 2700.0;
constexpr double kQamRolloff = 0.2;

struct TedsCarrierClass {
    CarrierKind kind = CarrierKind::Unknown;
    double bwHz = 0;           // 25e3 .. 150e3 for QAM and phase modulation; 0 unknown
    int subcarriers = 0;       // QAM: 8, 16, 32 or 48
    int qamOrder = 0;          // 4, 16, 64 (16 when 16 and 64 cannot be told apart: see orderSure), 0 unknown
    bool orderSure = false;
    float score = 0;           // the strength of the feature that decided (0..1)
    float snrDb = -99;         // in the carrier's own bandwidth
};
// x: a capture at fs; the candidate centred at offHz from its centre, spread over about widthHz (a rise of the spectrum, findCarriers).
// Up to 0.35 s of the capture (at most 2^21 samples) is used. TedsIdentifier transforms the capture once for any number of candidates.
class TedsIdentifier {
public:
    TedsIdentifier(const cf32* x, size_t n, double fs);
    TedsCarrierClass classify(double offHz, double widthHz) const;
private:
    double fs_ = 0;
    int N_ = 0;
    std::vector<float> re_, im_;   // the spectrum, natural order
};
TedsCarrierClass classifyCarrier(const cf32* x, size_t n, double fs, double offHz, double widthHz);

// ---------------------------------------------------------------- test signal

// A QAM carrier as clause 5 describes it: M = bw / 25 kHz * 8 sub-carriers at fm = (0.5625 - (M/2 - m) * 1.125) * 2400 Hz, 2 400 symbols/s each,
// root raised cosine 0.2, the data as 4/16/64-QAM (tables 5.4 to 5.6) and every 8th symbol of each sub-carrier a pilot on the unit circle.
// The pilot positions are this generator's own, not Annex P's: the identifier does not use them. Samples at kQamGenRate, continuous across calls.
constexpr double kQamGenRate = kQamSymRate * 96;   // 230.4 kHz
class QamGen {
public:
    QamGen(int bwKHz, int order, uint32_t seed);
    void push(int symbols, std::vector<cf32>& out);   // `symbols` symbol periods (96 samples each), unit mean power
    int subcarriers() const { return M_; }
private:
    int M_, order_;
    uint32_t rng_;
    long sym_ = 0;
    std::vector<float> h_;                   // the pulse at 96 samples per symbol, +-kSpan symbols
    std::vector<std::vector<cf32>> hist_;    // per sub-carrier, the last 2 * kSpan + 1 symbols
    std::vector<double> ph_;                 // sub-carrier oscillator phases
    float norm_ = 1;
    cf32 dataSymbol();
};

} // namespace dect2::tetra
