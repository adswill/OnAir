// APCO Project 25 Phase 2 (TDMA, TIA-102.BBAB physical layer / BBAC MAC) protocol pieces shared by the receiver and the test signal.
// Receive only: nothing here is used to transmit.
//
// WHERE THE LAYOUT COMES FROM. The burst layer follows the decoder of XMC_SDR (P25_P2_Decode, heard working on the NSW PSN, WACN BEE00 system 2D1):
// 6000 symbols/s differential QPSK (+-pi/4, +-3pi/4) in 12.5 kHz through a 0.35 root raised cosine, 180-dibit bursts of 30 ms, 12 to a 360 ms
// superframe, each starting with a 40-bit ISCH: the sync word 0x575D57F7FF (S-ISCH) or a (40,9) code word (I-ISCH: channel, location, first,
// ultraframe count). The DUID is an (8,4) code spread over dibits 20, 57, 142 and 179 of the burst. The bursts are scrambled (TIA-102.BBAC 7.2.5)
// with a 44-bit LFSR seeded from the WACN, the system ID and the colour code (NAC) of the system, 4320 bits per superframe; the receiver gets the
// seed from the Phase 1 control channel (P25p2Receiver::setSystem). 4V bursts carry four 72-bit AMBE+2 half-rate frames and 12 dibits of the
// encryption sync (ESS-B), 2V bursts two frames and the ESS-A parity; the ESS is RS(44,16) over GF(64) and gives the algorithm ID, key ID and MI.
// NOT established: the MAC PDUs of the SACCH / FACCH bursts (XMC does not decode them). Their codewords (RS(46,26) SACCH, RS(52,30) FACCH, a CCITT
// CRC, no interleave) sit in the burst positions after the ISCH that the DUID leaves free: this program's own layout, used by its test signal. On a
// real channel they are expected not to decode (RS / CRC fail) and are only counted; calls are then found from the voice bursts and the ESS.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace p25p2 {

constexpr double kSymbolRate = 6000.0;
constexpr int kSlotDibits = 180;               // 30 ms
constexpr int kBurstDibits = kSlotDibits;
constexpr int kIschDibits = 20;                // 40 bits at the start of every burst
constexpr int kSlotsPerSuperframe = 12;        // 360 ms
constexpr uint64_t kSyncWord = 0x575D57F7FFULL;   // 40 bits, MSB first
constexpr int kVoiceBits = 72;                 // one AMBE+2 half-rate frame
constexpr int kScrambleDibits = 10 + kSlotsPerSuperframe * kSlotDibits;   // 10 leading zero dibits, then 4320 bits

// DUIDs of the bursts (4 bits, an (8,4) code word spread over four dibits)
enum Duid { kDuid4V = 0, kDuidSacch = 3, kDuid2V = 6, kDuidFacch = 9, kDuidSacchClear = 12, kDuidLcch = 13, kDuidFacchClear = 15 };
const char* duidName(int d);

// Dibit <-> differential phase step in units of pi/4: 00 +1, 01 +3, 10 -1, 11 -3 (the C4FM table of Phase 1)
inline int dibitStep(int d) { static const int t[4] = {1, 3, -1, -3}; return t[d & 3]; }
inline int stepDibit(int s) { return s == 3 ? 1 : s == 1 ? 0 : s == -1 ? 2 : 3; }

// Root raised cosine impulse response at time t (in symbols), roll-off alpha
constexpr double kRollOff = 0.35;
inline double rrcImpulse(double t, double a) {
    const double pi = 3.14159265358979323846;
    const double at = a * t;
    if (t > -1e-9 && t < 1e-9) return 1.0 - a + 4.0 * a / pi;
    const double q = 1.0 / (4.0 * a);
    if ((t > q - 1e-9 && t < q + 1e-9) || (t > -q - 1e-9 && t < -q + 1e-9)) {
        const double r = pi / (4.0 * a);
        return (a / 1.4142135623730951) * ((1.0 + 2.0 / pi) * __builtin_sin(r) + (1.0 - 2.0 / pi) * __builtin_cos(r));
    }
    return (__builtin_sin(pi * t * (1.0 - a)) + 4.0 * at * __builtin_cos(pi * t * (1.0 + a))) / (pi * t * (1.0 - 16.0 * at * at));
}

// ---- ISCH (the first 20 dibits of every burst)
void ischSync(uint8_t dibits[kIschDibits]);
uint64_t ischValue(const uint8_t dibits[kIschDibits]);        // the 40 bits, the first dibit in the top bits
int syncDistance(const uint8_t dibits[kIschDibits]);          // bits that differ from the sync word
// I-ISCH: 7 information bits, channel (bits 6..5), location (4..3), first (2), ultraframe count (1..0)
inline int ischInfoOf(int channel, int loc, int first, int uf) { return ((channel & 3) << 5) | ((loc & 3) << 3) | ((first & 1) << 2) | (uf & 3); }
uint64_t ischWord(int info);
void ischInfo(int info, uint8_t dibits[kIschDibits]);
// The nearest I-ISCH code word within 7 bit errors: its information bits, else -1. errs: the distance.
int ischDecode(uint64_t v, int* errs = nullptr);
// The logical channel (0 or 1) of the burst in slot s (0..11) of the superframe: the slot parity, the other way round in slots 10 and 11
inline int slotChannel(int s) { return (s & 1) ^ (s > 9 ? 1 : 0); }

// ---- scrambling: dibit i of the burst in slot s of the superframe is XORed with seq[s * 180 + i]. All-zero seed: an all-zero sequence.
void scrambleSeq(int wacn, int sysId, int nac, uint8_t seq[kScrambleDibits]);

// ---- DUID
constexpr int kDuidPos[4] = {20, 57, 142, 179};
int duidWord(int duid);                                        // the 8-bit code word
void duidPut(int duid, uint8_t burst[kBurstDibits]);
int duidGet(const uint8_t burst[kBurstDibits], int* raw = nullptr);   // the DUID, -1 when it cannot be corrected

// ---- voice: 72-bit frames at bit offsets 2, 76, 172, 246 (4V) or 2, 76 (2V), counted from dibit 20 of the burst (a dibit's high bit first)
constexpr int kVoiceOff[4] = {2, 76, 172, 246};
// the AMBE+2 frame as mbelib's code words (C0 24 bits, C1 23, C2 11, C3 14) <-> its place in the burst
void ambeGet(const uint8_t burst[kBurstDibits], int bitOff, char fr[4][24]);
void ambePut(const char fr[4][24], int bitOff, uint8_t burst[kBurstDibits]);
// the 72 bits of a frame in code word order (C0, C1, C2, C3) <-> the code words
void ambeFlatten(const char fr[4][24], uint8_t bits[kVoiceBits]);
void ambeUnflatten(const uint8_t bits[kVoiceBits], char fr[4][24]);

// ---- encryption sync: ESS-B (16 hexbits, 4 in each 4V burst at dibit 94), ESS-A (28 parity hexbits: 16 at dibit 94 and 12 at dibit 143 of the 2V burst)
constexpr int kEssBPos = 94, kEssBDibits = 12, kEssA1Pos = 94, kEssA1Dibits = 48, kEssA2Pos = 143, kEssA2Dibits = 36;
void essEncode(int algId, int keyId, const uint8_t mi[9], uint8_t essB[16], uint8_t essA[28]);
// RS(44,16): corrects essB in place; an algorithm ID of 0 is reported as 0x80 (clear). fixed: hexbits corrected.
bool essDecode(uint8_t essB[16], const uint8_t essA[28], int& algId, int& keyId, uint8_t mi[9], int* fixed = nullptr);
void hexbitsPut(const uint8_t* hex, int n, uint8_t* dibits);   // n hexbits -> 3n dibits
void hexbitsGet(const uint8_t* dibits, int n, uint8_t* hex);

// ---- MAC PDU codewords (this program's layout, see above): the 156 burst dibits after the ISCH that are not DUID dibits, in order
constexpr int kPayloadDibits = kBurstDibits - kIschDibits - 4;
void payloadPut(const uint8_t* dibits, int n, uint8_t burst[kBurstDibits]);   // n <= kPayloadDibits; the rest is left as it is
void payloadGet(const uint8_t burst[kBurstDibits], uint8_t dibits[kPayloadDibits]);

// MAC PDU codewords. SACCH: 17 MAC bytes + CRC-16 + 4 spare bits = 26 hexbits -> RS(46,26) -> 138 dibits. FACCH: 20 bytes + CRC-16 + 4 spare bits = 30 hexbits
// -> RS(52,30) -> 156 dibits.
constexpr int kSacchMacBytes = 17, kFacchMacBytes = 20;
constexpr int kSacchDibits = 138, kFacchDibits = 156;
void macEncode(bool facch, const uint8_t* mac, uint8_t* dibits);        // mac: kSacchMacBytes or kFacchMacBytes bytes
bool macDecode(bool facch, const uint8_t* dibits, uint8_t* mac, int* corrected = nullptr);   // RS and CRC must pass

// ---- MAC PDU
enum MacOp { kMacSignal = 0, kMacPtt = 1, kMacEndPtt = 2, kMacIdle = 3, kMacActive = 4, kMacHangtime = 6 };
const char* macOpName(int op);
// Message opcodes inside a MAC PDU (the TSBK opcode plus 0x40 for the ones that exist in both)
enum MacMsg {
    kMsgNull = 0x00, kMsgGrpVUser = 0x01, kMsgUuVUser = 0x02,
    kMsgGrpVGrant = 0x40, kMsgGrpVGrantUpd = 0x42, kMsgUuVGrant = 0x44,
    kMsgIdenUpVu = 0x74, kMsgRfssSts = 0x7A, kMsgNetSts = 0x7B, kMsgAdjSts = 0x7C, kMsgIdenUp = 0x7D,
};
const char* macMsgName(int op);
int macMsgLen(int op);                 // bytes including the opcode; 0 when the opcode is not known here (parsing of the PDU stops)

struct MacHeader { int opcode = 0, offset = 0; };
inline uint8_t macHeaderByte(int opcode, int offset) { return (uint8_t)(((opcode & 7) << 5) | ((offset & 7) << 2)); }
inline MacHeader macHeader(uint8_t b) { return {(b >> 5) & 7, (b >> 2) & 7}; }

// PTT fields (after the header byte): message indicator 9 bytes (skipped, never read), algorithm ID 1, key ID 2 (skipped), source 3, group 2
constexpr int kPttMiOff = 1, kPttAlgOff = 10, kPttKeyOff = 11, kPttSrcOff = 13, kPttGrpOff = 16, kPttLen = 18;
// END_PTT fields: colour code (12 bits) + 4 reserved bits 2 bytes, source 3, group 2
constexpr int kEndColorOff = 1, kEndSrcOff = 3, kEndGrpOff = 6, kEndLen = 8;
constexpr int kAlgClear = 0x80;

inline uint32_t be24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
inline uint64_t be64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
inline void put24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
inline void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// Builders (the test signal and the unit tests); each returns a MAC PDU padded with zero bytes to n bytes
std::vector<uint8_t> macPtt(int n, int src, int group, int algId);
std::vector<uint8_t> macEndPtt(int n, int src, int group, int colorCode);
std::vector<uint8_t> macActive(int n, int svcOpts, int group, int src);               // MAC_ACTIVE carrying a group voice channel user message
std::vector<uint8_t> macActiveUu(int n, int svcOpts, int target, int src);            // ... a unit-to-unit voice channel user message
std::vector<uint8_t> macSignal(int n, const std::vector<std::vector<uint8_t>>& msgs); // MAC_SIGNAL with the given messages (each starts with its opcode byte)
std::vector<uint8_t> macIdle(int n);
std::vector<uint8_t> msgGrpVGrant(int svc, int channel, int group, int src);
std::vector<uint8_t> msgGrpVGrantUpd(int ch1, int g1, int ch2, int g2);
std::vector<uint8_t> msgUuVGrant(int channel, int target, int src);
std::vector<uint8_t> msgRfssSts(int lra, int sysId, int rfss, int site, int channel, int ssc);
std::vector<uint8_t> msgNetSts(int lra, int wacn, int sysId, int channel, int ssc);
std::vector<uint8_t> msgAdjSts(int lra, int sysId, int rfss, int site, int channel, int ssc);
// bandwidth in 125 Hz units (9 bits), transmit offset in 250 kHz units (8 bits + sign, sign set for positive), spacing in 125 Hz units, base in 5 Hz units
std::vector<uint8_t> msgIdenUp(int id, int bw125, int txOffset250k, int spacing125, uint32_t base5Hz);

} // namespace p25p2
} // namespace dect2
