// UMTS / WCDMA downlink cell search (3GPP TS 25.213): primary and secondary synchronisation channels and the CPICH, at 3.84 Mchip/s. For every cell heard: its primary
// scrambling code number, the code group, level and Ec/Io, the frame timing and the frequency offset. The broadcast channel (P-CCPCH, MIB, SIB, TS 25.331) is not decoded.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace umts {
constexpr double kChipRate = 3.84e6;
constexpr int kChipsPerSlot = 2560, kSlotsPerFrame = 15, kChipsPerFrame = 38400;
}

struct UmtsCell {
    int scramblingCode = -1;       // primary scrambling code number, 0 .. 511
    int group = -1;                // code group 0 .. 63
    int frameOffsetChips = 0;      // where the frame starts in the block
    double cfoHz = 0;
    double ecIoDb = 0;             // CPICH power over total in-band power (dB)
    double rscpDbfs = 0;           // CPICH level, dB re full scale
    int slotPeak = 0;              // the slot timing of the sync peak (chips)
};

// Finds the cells in a block of samples at kChipRate * 2 (7.68 Msps). maxCfoHz: the carrier offset range to search.
std::vector<UmtsCell> umtsCellSearch(const cf32* x7p68, size_t n, double maxCfoHz, int maxCells, double* scanTimeMs = nullptr);

// A test transmitter: SCH + CPICH of several cells, plus a dummy code channel, as a complex baseband at 7.68 Msps. Nothing is transmitted.
struct UmtsTxCell { int code = 0; double cfoHz = 0; double amp = 1; int chipOffset = 0; double dataPower = 0; };
std::vector<cf32> umtsGenerate(const std::vector<UmtsTxCell>& cells, size_t samples, double snrDb, unsigned seed = 1);

}  // namespace dect2
