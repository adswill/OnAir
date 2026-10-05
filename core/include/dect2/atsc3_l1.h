// ATSC 3.0 L1-Basic signalling (A/322 sections 6.5 and 9.2): the 200-bit structure with its CRC, and the protection chain of
// scrambling, BCH, LDPC, parity permutation, repetition, puncturing and constellation mapping that turns it into cells of the
// first Preamble symbol. Modes 1 to 5 (the modes the defined preamble_structure values use).
#pragma once
#include "ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct L1Basic {
    int version = 0;                   // L1B_version (3 bits)
    int mimoScatteredPilotEncoding = 0;
    int llsFlag = 0;
    int timeInfoFlag = 0;              // 2 bits
    int returnChannelFlag = 0;
    int paprReduction = 0;             // 2 bits
    int frameLengthMode = 0;           // 0 = time-aligned, 1 = symbol-aligned
    int frameLength = 0;               // mode 0: 10 bits (units of 5 ms)
    int excessSamplesPerSymbol = 0;    // mode 0: 13 bits
    int timeOffset = 0;                // mode 1: 16 bits
    int additionalSamples = 0;         // mode 1: 7 bits
    int numSubframes = 0;              // number of subframes minus one (8 bits)
    int preambleNumSymbols = 0;        // number of Preamble symbols minus one (3 bits)
    int preambleReducedCarriers = 0;   // 3 bits
    int l1DetailContentTag = 0;        // 2 bits
    int l1DetailSizeBytes = 0;         // 13 bits
    int l1DetailFecType = 0;           // 3 bits: modes 1 to 7 are values 0 to 6
    int l1DetailAdditionalParityMode = 0;   // 2 bits
    int l1DetailTotalCells = 0;        // 19 bits
    int firstSubMimo = 0;
    int firstSubMiso = 0;              // 2 bits
    int firstSubFftSize = 0;           // 2 bits: 0 = 8K, 1 = 16K, 2 = 32K
    int firstSubReducedCarriers = 0;   // 3 bits
    int firstSubGuardInterval = 0;     // 4 bits
    int firstSubNumOfdmSymbols = 0;    // number of symbols minus one (11 bits)
    int firstSubScatteredPilotPattern = 0;   // 5 bits
    int firstSubScatteredPilotBoost = 0;     // 3 bits
    int firstSubSbsFirst = 0, firstSubSbsLast = 0;
    bool crcOk = false;                // set by unpack
};

constexpr int kL1BasicBits = 200;

// The 200 bits (including the CRC computed here) and back; unpack checks the CRC.
std::vector<uint8_t> packL1Basic(const L1Basic& l1);
bool unpackL1Basic(const std::vector<uint8_t>& bits, L1Basic& l1);

uint32_t l1Crc32(const uint8_t* bits, int n);   // CRC of A/322 6.1.2.2 (register initialised to ones), the value for the bit string

// Number of cells L1-Basic occupies in the first Preamble symbol for FEC mode 1..5 (A/322 Table 6.17).
int l1BasicCells(int mode);

// Protection chain: the cells to put into the first Preamble symbol.
std::vector<cf32> encodeL1Basic(const L1Basic& l1, int mode);

// Reverse direction. `cells` are equalised cells, `noiseVar` the noise power per cell (both real dimensions together).
// Returns true when LDPC converged, BCH was satisfied and the CRC matched.
bool decodeL1Basic(const cf32* cells, int nCells, float noiseVar, int mode, L1Basic& out, int* ldpcIterations = nullptr);


// ---------------------------------------------------------------------------------------------------------------------------------
// L1-Detail (A/322 9.3): the description of the subframes and the physical layer pipes (PLPs) of the frame.

struct L1DetailPlp {
    int id = 0, llsFlag = 0, layer = 0, start = 0, size = 0, scramblerType = 0, fecType = 0, mod = 0, cod = 0, tiMode = 0;
    int fecBlockStart = 0, ctiFecBlockStart = 0;
    int numChannelBonded = 0, channelBondingFormat = 0;
    std::vector<int> bondedRfId;
    int mimoStreamCombining = 0, mimoIqInterleaving = 0, mimoPh = 0;
    int type = 0, numSubslices = 0, subsliceInterval = 0;
    int tiExtendedInterleaving = 0, ctiDepth = 0, ctiStartRow = 0;
    int htiInterSubframe = 0, htiNumTiBlocks = 0, htiNumFecBlocksMax = 0;
    std::vector<int> htiNumFecBlocks;
    int htiCellInterleaver = 0;
    int ldmInjectionLevel = 0;
};

struct L1DetailSubframe {
    // the first subframe takes these from L1-Basic; they are used from the second subframe on
    int mimo = 0, miso = 0, fftSize = 0, reducedCarriers = 0, guardInterval = 0, numOfdmSymbols = 0;
    int scatteredPilotPattern = 0, scatteredPilotBoost = 0, sbsFirst = 0, sbsLast = 0;
    int subframeMultiplex = 0;
    int frequencyInterleaver = 0;
    int sbsNullCells = 0;
    std::vector<L1DetailPlp> plps;   // L1D_num_plp + 1 entries
};

struct L1Detail {
    int version = 1;
    std::vector<int> bondedBsid;   // L1D_num_rf entries
    uint32_t timeSec = 0;
    int timeMsec = 0, timeUsec = 0, timeNsec = 0;
    std::vector<L1DetailSubframe> subframes;   // L1B_num_subframes + 1 entries
    int bsid = 0;
    bool crcOk = false;
};

// The bits of L1-Detail with zero padding up to `sizeBytes` bytes (0 = the smallest size) and the CRC at the end. L1-Basic supplies the
// presence of several fields (number of subframes, time information, MIMO and subframe boundary symbols of the first subframe).
std::vector<uint8_t> packL1Detail(const L1Basic& basic, const L1Detail& d, int sizeBytes = 0);
// Needed length in bytes for L1B_L1_Detail_size_bytes.
int l1DetailSizeBytes(const L1Basic& basic, const L1Detail& d);
bool unpackL1Detail(const L1Basic& basic, const std::vector<uint8_t>& bits, L1Detail& d);

// Protection of L1-Detail: segmentation, per segment scrambling, BCH, LDPC, permutation, repetition, puncturing and mapping, for FEC
// modes 1 to 7 (L1B_L1_Detail_fec_type + 1). Additional parity (L1B_L1_Detail_additional_parity_mode) is not supported yet.
int l1DetailCells(int sizeBytes, int mode);
std::vector<cf32> encodeL1Detail(const std::vector<uint8_t>& bits, int mode);   // bits.size() = 8 * sizeBytes
bool decodeL1Detail(const cf32* cells, int nCells, float noiseVar, int mode, int sizeBytes, std::vector<uint8_t>& bits, int* ldpcIterations = nullptr);

} // namespace atsc3
} // namespace dect2
