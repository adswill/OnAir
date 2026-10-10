// FT8, FT4, FT2 and WSPR inside the HF digital receiver: the pieces the decoder (hfdig_ftx.cpp), the WSPR codec (hfdig_wspr.cpp), the
// test audio (hfdig_ftx_gen.cpp) and the tests share. Not part of the public interface (see dect2/hfdig_ftx.h).
//
// Sources (read, re-implemented, credited):
//   WSJT-X (K1JT et al., GPL-3): lib/ft8/genft8.f90, lib/ft4/genft4.f90, lib/ft8/encode174_91.f90, lib/ft8/ldpc_174_91_c_generator.f90,
//     lib/crc14.cpp, lib/77bit/packjt77.f90, lib/wsprd/wsprd.c, wsprd_utils.c, wsprsim_utils.c, lib/ft4_decode.f90.
//   WSJT-X Improved 3.1.0 (DG2YCB, GPL-3): FT2 = the FT4 frame at 288 samples per symbol (12 kHz), 3.75 s periods
//     (lib/decoder.f90, widgets/mainwindow.cpp, read on sources.debian.org).
//   ft8_lib (K. Goba, MIT): ft8/constants.c, ft8/message.c: the same tables, an independent cross-check.
//   JTEncode (Etherkit, GPL-3): the WSPR sync vector, polynomials and interleaver, a cross-check.
//   S. Franke, B. Somerville, J. Taylor, "The FT4 and FT8 Communication Protocols", QEX July/August 2020.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace ftx {

// ---------------------------------------------------------------- tables (hfdig_ftx_tables.cpp)
constexpr int kN = 174, kK = 91, kM = 83;
extern const uint8_t kLdpcGen[kM][12];       // parity rows, 91 bits MSB first
extern const uint8_t kLdpcNm[kM][7];         // the codeword bits (1-origin, 0 = unused) of each parity check
extern const uint8_t kLdpcMn[kN][3];         // the three checks (1-origin) of each codeword bit
extern const uint8_t kLdpcNumRows[kM];
extern const uint8_t kFt8Costas[7];
extern const uint8_t kFt4Costas[4][4];
extern const uint8_t kFt8Gray[8];
extern const uint8_t kFt4Gray[4];
extern const uint8_t kFt4Rvec[77];           // FT4 / FT2 scrambling of the 77 message bits
extern const uint8_t kWsprSync[162];
extern const char* const kArrlSections[86];  // ARRL Field Day sections (i3 = 0, n3 = 3 / 4)
const char* rttyMult(int i);                 // ARRL RTTY Roundup multipliers (i3 = 3), 0 .. 170

// CRC-14 (polynomial 0x2757) of the 77 message bits, as WSJT-X: over 82 bits (the message and 5 zeros), augmented
uint16_t crc14(const uint8_t* bits77);
// 77 message bits -> the 174-bit codeword: message, CRC-14, 83 parity bits
void encode174(const uint8_t* bits77, uint8_t* cw174);
// Belief propagation (sum-product). llr > 0 means 1. Returns the number of parity checks still wrong (0 = a codeword).
int ldpcDecode(const float* llr, int iters, uint8_t* cw174);
bool checkCrc(const uint8_t* cw174);

// ---------------------------------------------------------------- 77-bit messages (hfdig_ftx_msg.cpp)
// The callsigns heard, by their 22-, 12- and 10-bit hashes, to show <hashed> calls by name.
class CallHash {
public:
    void add(const std::string& call);
    std::string find22(uint32_t h) const;
    std::string find12(uint32_t h) const;
    std::string find10(uint32_t h) const;
    void clear() { m_.clear(); }
    static uint32_t hash22(const std::string& call);
private:
    std::map<uint32_t, std::string> m_;   // by the 22-bit hash
};
// Text -> 77 bits (standard messages, /R /P, CQ with a modifier, <hashed> calls, nonstandard calls (type 4), free text, telemetry).
// Returns false when the text cannot be sent.
bool pack77(const std::string& text, uint8_t* bits77);
// 77 bits -> text; empty when the bits are not a valid message. Learns the calls it sees (when hash is given).
std::string unpack77(const uint8_t* bits77, CallHash* hash);
// The calling station and its grid of a decoded message ("CQ K1ABC FN42" -> K1ABC, FN42; "K1ABC W9XYZ EN37" -> W9XYZ, EN37)
void callAndGrid(const std::string& msg, std::string& call, std::string& grid);
bool gridToLatLon(const std::string& grid, double& lat, double& lon);

// ---------------------------------------------------------------- the waveforms
enum Mode { kFt8 = 0, kFt4 = 1, kFt2 = 2, kWspr = 3, kModes = 4 };
struct Spec {
    const char* name;
    double period;      // slot, s
    double t0;          // nominal start of the transmission in the slot, s
    double symSec;      // symbol length, s
    double spacing;     // tone spacing, Hz
    int nn;             // symbols sent (FT4 / FT2: with the two ramp symbols)
    int tones;
    double bt;          // GFSK bandwidth-time product (0 = plain FSK)
};
const Spec& spec(int mode);
// FT8 79 tones, FT4 / FT2 103 tones (without the ramp symbols) from 77 message bits
std::vector<int> ftxTones(int mode, const uint8_t* bits77);
// Adds a transmission to out (rate Hz, starting at sample `start`, may be negative or past the end): tone 0 at f0 Hz, amplitude amp.
// mirrored: the tones go down from f0 (a lower sideband transmission heard on the upper sideband).
void addWave(int mode, const std::vector<int>& tones, double f0, double amp, double rate, double start, bool mirrored, double phase,
             std::vector<float>& out);

// ---------------------------------------------------------------- WSPR (hfdig_wspr.cpp)
uint32_t nhash(const void* key, size_t length, uint32_t initval);   // Bob Jenkins' lookup3 hashlittle(), as WSJT-X's nhash.c
// "K1ABC FN42 37", "PJ4/K1ABC 37" (type 2), "<K1ABC> FN42AX 37" (type 3) -> 50 bits in 7 bytes (MSB first)
bool wsprPack(const std::string& msg, uint8_t* data7);
// 162 channel symbols (0..3)
bool wsprSymbols(const std::string& msg, std::vector<int>& sym);
// 50 bits -> text, empty when invalid; hashes of the type 1 / 2 calls go into table (15 bits), type 3 is shown with them
std::string wsprUnpack(const uint8_t* data7, std::map<uint32_t, std::string>& table, int* dbm);
// Fano sequential decoding of 162 soft bits (llr > 0 means 1, deinterleaved, in coded order) into 50 data bits
bool wsprFano(const float* llr162, uint8_t* data7, long maxCycles);
void wsprConvEncode(const uint8_t* data7, uint8_t* coded162);
void wsprInterleave(const uint8_t* in162, uint8_t* out162);     // coded order -> channel order
void wsprDeinterleave(const float* in162, float* out162);       // channel order -> coded order

} // namespace ftx
} // namespace dect2
