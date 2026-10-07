// DMR burst layout and message formats (ETSI TS 102 361-1 clauses 6, 7, 8, 9 and annex E; TS 102 361-2 clause 7 for the voice services).
// Used by the receiver to read bursts and by the test signal to build them. Nothing here keeps state.
#pragma once
#include "dmr_fec.h"
#include <string>
#include <vector>

namespace dect2 {
namespace dmr {

// ---- frame synchronisation patterns (table 9.2): 48 bits, 24 dibits that are all +3 or -3
enum Sync {
    kSyncBsVoice, kSyncBsData, kSyncMsVoice, kSyncMsData, kSyncRc, kSyncDm1Voice, kSyncDm1Data, kSyncDm2Voice, kSyncDm2Data, kSyncCount
};
extern const uint64_t kSyncWords[kSyncCount];
const char* syncName(int s);
inline bool syncIsVoice(int s) { return s == kSyncBsVoice || s == kSyncMsVoice || s == kSyncDm1Voice || s == kSyncDm2Voice; }
inline bool syncIsData(int s) { return s == kSyncBsData || s == kSyncMsData || s == kSyncDm1Data || s == kSyncDm2Data; }
inline bool syncIsBs(int s) { return s == kSyncBsVoice || s == kSyncBsData; }
inline bool syncIsMs(int s) { return s == kSyncMsVoice || s == kSyncMsData || s == kSyncRc; }
inline bool syncIsDirect(int s) { return s >= kSyncDm1Voice && s <= kSyncDm2Data; }
int syncDirectSlot(int s);                        // 1 or 2 for the direct mode patterns, 0 otherwise
int syncOf(int family, bool voice);               // family 0 BS, 1 MS, 2 direct slot 1, 3 direct slot 2
void syncSymbols(int s, int8_t out[24]);          // the 24 symbols (+3 / -3)
// How many of the 24 dibits of a received centre field differ from each pattern; best match returned (index, or -1 if no pattern is within maxDiff)
int matchSync(const Bits& centre48, int maxDiff, int* diff = nullptr);

// ---- burst: 264 bits = 132 symbols, symbol 0 first on the air, the centre field is symbols 54..77
constexpr int kBurstBits = 264, kBurstSymbols = 132, kCentreFirstSymbol = 54, kSlotSymbols = 144, kCachSymbols = 12;

enum DataType {
    kDtPiHeader = 0, kDtVoiceLcHeader = 1, kDtTerminatorLc = 2, kDtCsbk = 3, kDtMbcHeader = 4, kDtMbcContinuation = 5, kDtDataHeader = 6,
    kDtRate12 = 7, kDtRate34 = 8, kDtIdle = 9, kDtRate1 = 10, kDtUsbd = 11
};
const char* dataTypeName(int dt);
const char* dataTypeShort(int dt);

Bits makeDataBurst(int cc, int dt, const Bits& payload196, int sync);                    // BPTC / trellis / rate 1 payload already in transmit order; sync -1: embedded signalling in the centre
// A voice burst: vs216 is VS(215)..VS(0); with sync >= 0 the centre carries that pattern, otherwise EMB (cc, pi, lcss) and the 32 embedded bits.
Bits makeVoiceBurst(const Bits& vs216, int sync, int cc, int pi, int lcss, const Bits& emb32);

Bits burstCentre(const Bits& burst);                                                        // the 48 centre bits
void burstDataPayload(const Bits& burst, Bits& payload196);                                 // the 2 x 98 payload bits of a data burst, in transmit order
void burstVoicePayload(const Bits& burst, Bits& vs216);

struct SlotTypeInfo { bool ok = false; int cc = 0, dt = 0, errors = 0; };
SlotTypeInfo burstSlotType(const Bits& burst);
struct EmbInfo { bool ok = false; int cc = 0, pi = 0, lcss = 0, errors = 0; Bits emb32; };
EmbInfo burstEmb(const Bits& burst);

// 4FSK symbols of a burst (+3, +1, -1, -3)
void bitsToSymbols(const Bits& bits, std::vector<int8_t>& sym);

// ---- full link control (72 bits): voice call identity
struct FullLc {
    bool pf = false;
    int flco = 0, fid = 0;
    uint8_t svc = 0;                 // service options (group and unit to unit calls)
    uint32_t dst = 0, src = 0;
    uint8_t raw[9] = {};
};
enum Flco { kFlcoGroupVoice = 0, kFlcoUnitVoice = 3, kFlcoTalkerAliasHeader = 4, kFlcoTalkerAliasBlock1 = 5, kFlcoTalkerAliasBlock2 = 6,
            kFlcoTalkerAliasBlock3 = 7, kFlcoGpsInfo = 8 };
void packFullLc(const FullLc& lc, uint8_t out[9]);
void parseFullLc(const uint8_t in[9], FullLc& lc);
const char* flcoName(int flco);
std::string serviceOptionsText(uint8_t svc);

// header and terminator bursts: 72 bit LC + Reed-Solomon parity (masked per data type) = 96 bits for the BPTC
void lcBurstInfo(const uint8_t lc[9], bool terminator, Bits& info96);
int lcBurstDecode(const Bits& info96, bool terminator, uint8_t lc[9]);                     // 0 clean, 1 one octet corrected, -1 failed

// ---- blocks of 96 bits that end in a CRC-CCITT (CSBK, headers, PI header, MBC)
void crcBlockInfo(const uint8_t data10[10], uint16_t mask, Bits& info96);
bool crcBlockCheck(const Bits& info96, uint16_t mask, uint8_t data10[10]);

struct Csbk {
    bool lb = true, pf = false;
    int opcode = 0, fid = 0;
    uint8_t data[8] = {};
};
void csbkInfo(const Csbk& c, Bits& info96);
bool csbkParse(const Bits& info96, Csbk& c);       // checks the CRC
const char* csbkName(int opcode, int fid);
uint32_t be24(const uint8_t* p);
void putBe24(uint8_t* p, uint32_t v);

// ---- data packets
enum Dpf { kDpfUdt = 0, kDpfResponse = 1, kDpfUnconfirmed = 2, kDpfConfirmed = 3, kDpfShortDefined = 13, kDpfShortRaw = 14, kDpfProprietary = 15 };
struct DataHeader {
    int dpf = 0, sap = 0;
    bool group = false, resp = false;
    uint32_t dst = 0, src = 0;
    int blocks = 0;                  // blocks to follow (BF), or appended blocks (AB) of the short data headers
    int pad = 0;                     // pad octet count of a packet, pad bits of a short data message
    bool fmf = false;
    int fsn = 0, ns = 0, dd = 0, sp = 0, dp = 0;
    bool resync = false, sarq = false;
    int status = 0;                  // status / precoded value
};
const char* dpfName(int dpf);
const char* sapName(int sap);
void packDataHeader(const DataHeader& h, uint8_t out[10]);       // the first ten octets; the CRC is added by crcBlockInfo with kMaskDataHeader
bool parseDataHeader(const uint8_t in[10], DataHeader& h);
int dataBlockBytes(int dt, bool confirmed);                      // 12 / 18 / 24 unconfirmed, 10 / 16 / 22 confirmed

// a data block: rate 1/2 and rate 1 blocks are BPTC or direct bits, rate 3/4 blocks the trellis; `user` is the data of the block
void dataBlockEncode(int dt, const uint8_t* user, size_t n, bool confirmed, unsigned dbsn, Bits& payload196);
// Decode a block's bytes (96, 144 or 192 bits as octets). For confirmed blocks `dbsn` and the CRC-9 check are filled in.
struct DataBlock { std::vector<uint8_t> bytes; unsigned dbsn = 0; bool crcOk = true; bool confirmed = false; };
bool dataBlockParse(int dt, const uint8_t* octets, bool confirmed, DataBlock& out);

// Defined data format (DD) of short data, TS 102 361-1 V2.6.1 table 9.50: 3 to 17 are the 8 bit sets ISO 8859-1 to -11 and -13 to -16, then the Unicode
// forms. (The 2006 edition, V1.2.1, numbered the Unicode forms one higher.) A talker alias has its own two bit format
// code (0 7 bit, 1 ISO 8859-1, 2 UTF-8, 3 UTF-16BE).
enum DdFormat { kDdBinary = 0, kDdBcd = 1, kDd7Bit = 2, kDdIso8859_1 = 3, kDdIso8859_16 = 17, kDdUtf8 = 18, kDdUtf16 = 19, kDdUtf16Be = 20, kDdUtf16Le = 21,
                kDdUtf32 = 22, kDdUtf32Be = 23, kDdUtf32Le = 24 };

// message text of short data or a talker alias
std::string textFromBytes(const uint8_t* p, size_t n, int format, bool isTalkerAlias);
const char* ddName(int dd);

// ---- talker alias assembled from the embedded LC of a voice call (TS 102 361-2 7.1.1.4, 7.1.1.5)
struct TalkerAlias {
    int format = 0, length = 0;
    uint8_t have = 0;                // bit 0 header, bits 1..3 blocks
    uint8_t data[28] = {};   // the seven octets (2..8) of the header and of each block
    void clear() { *this = TalkerAlias(); }
    void add(const FullLc& lc);
    bool complete() const;
    std::string text() const;
};
// the LC PDUs that carry an alias, for the test signal: returns 1 + number of blocks
int talkerAliasPdus(const std::string& alias, int format, FullLc out[4]);

} // namespace dmr
} // namespace dect2
