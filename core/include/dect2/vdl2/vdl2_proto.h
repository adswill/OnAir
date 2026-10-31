// VDL Mode 2 layers above the modem: burst header, scrambler, Reed-Solomon code and interleaving, HDLC framing, AVLC frames, and the
// protocols the frames carry (ACARS over AVLC, ISO 8208 / X.25 packets with CLNP and COTP headers, XID link management). The same in
// reverse for the test signal. Nothing here touches a radio signal.
//
// Facts from ICAO Annex 10 Vol III (Part I ch. 6), ICAO Doc 9776 (VDL Mode 2 technical manual) as dumpvdl2 implements them (read for
// reference, not copied):
//   Burst: ramp-up, 16 training symbols, header (3 reserved bits, 17-bit transmission length in bits sent least significant bit first,
//     5 FEC bits: a (25,20) code with the parity rows below), then the data octets and the FEC octets.
//   Scrambler: x^15 + x + 1, register 0x6959 at the first header bit, running on over the data and the FEC (the same pass descrambles).
//   Reed-Solomon (255,249) over GF(2^8), field polynomial 0x187, first root alpha^120. Blocks of 249 octets; the last block is shorter and
//     carries 0 (under 3 octets), 2 (3 - 30), 4 (31 - 67) or 6 (68 and more) parity octets: the first ones of the full (255,249) parity,
//     the others are left out (erasures for the decoder). Data and FEC octets are sent column by column across the blocks.
//   Octets are sent least significant bit first; the header word most significant bit first.
//   AVLC: HDLC frames between 0x7E flags with zero-bit insertion; two 4-octet addresses (destination, source), control octet,
//     information, FCS (CRC-16/X.25, residue 0xF0B8).
#pragma once
#include "vdl2_tel.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr double kVdl2SymbolRate = 10500.0;
constexpr int kVdl2PreambleSyms = 16;
constexpr int kVdl2HeaderBits = 25;
constexpr int kVdl2RsN = 255, kVdl2RsK = 249;
constexpr uint16_t kVdl2LfsrSeed = 0x6959;
constexpr double kVdl2Csc = 136.975e6;          // the common signalling channel

extern const int kVdl2PreamblePhase[kVdl2PreambleSyms];   // phase of each training symbol relative to the first, in units of pi/4
extern const uint8_t kVdl2GrayBits[8];          // phase step (units of pi/4) -> its three bits (first bit sent = most significant)
int vdl2StepForBits(int bits3);                 // the reverse

// ---- bit layer ----
void vdl2Scramble(std::vector<uint8_t>& bits, size_t from = 0);   // XOR with the scrambler sequence, which starts at `from`
uint32_t vdl2HeaderWord(uint32_t lenBits);                         // the 25 header bits, first sent = bit 24
bool vdl2HeaderDecode(uint32_t word, uint32_t& lenBits, bool& corrected);

struct Vdl2Layout {
    int dataOct = 0, fecOct = 0, blocks = 0, lastLen = 0, lastFec = 0;
};
bool vdl2Layout(uint32_t lenBits, Vdl2Layout& l);                  // false when the length cannot be a burst
int vdl2LastFec(int lastLen);

// Reed-Solomon (255,249). decode: nParity octets of parity present (the rest erased); returns the octets corrected (erasures not
// counted), or -1 when the block cannot be corrected.
void vdl2RsEncode(const uint8_t data[kVdl2RsK], uint8_t parity[6]);
int vdl2RsDecode(uint8_t cw[kVdl2RsN], int nParity);

// The bits after the training sequence for a burst whose data bits (flags and frames, already stuffed) are `dataBits`: header, data
// and FEC octets, scrambled.
std::vector<uint8_t> vdl2BurstBits(const std::vector<uint8_t>& dataBits);
// The reverse, after the header: `bits` (descrambled) holds 8 * (dataOct + fecOct) bits. Fills the data bits (lenBits of them).
// Returns false when a block cannot be corrected.
bool vdl2BurstData(const uint8_t* bits, const Vdl2Layout& l, uint32_t lenBits, std::vector<uint8_t>& dataBits, int& rsFixed);

// ---- HDLC ----
uint16_t vdl2Fcs(const uint8_t* p, size_t n);                      // the FCS to send (low octet first)
bool vdl2FcsOk(const uint8_t* p, size_t n);                        // frame including its two FCS octets
std::vector<uint8_t> vdl2HdlcBits(const std::vector<std::vector<uint8_t>>& frames);   // frames (FCS already appended) -> flags and stuffed bits
std::vector<std::vector<uint8_t>> vdl2HdlcFrames(const std::vector<uint8_t>& bits);  // the octets between flags (whole octets only)

// ---- AVLC ----
struct Vdl2Addr { uint32_t addr = 0; int type = 1; bool status = false; };
void vdl2PutAddr(uint8_t* o4, const Vdl2Addr& a, bool last);       // 4 octets
Vdl2Addr vdl2GetAddr(const uint8_t* o4);
const char* vdl2AddrTypeName(int type);                            // "aircraft", "ground station" ...

// A complete frame: addresses, control, info, FCS.
std::vector<uint8_t> vdl2Frame(const Vdl2Addr& dst, const Vdl2Addr& src, uint8_t control, const std::vector<uint8_t>& info);
uint8_t vdl2ControlI(int ns, int nr, bool p);
constexpr uint8_t kVdl2CtrlUI = 0x03, kVdl2CtrlXID = 0xAF;          // U frames (P/F 0); add 0x10 for P/F

// Decodes a frame whose FCS was good (n without the FCS). False when it is too short to be AVLC.
bool vdl2ParseFrame(const uint8_t* f, size_t n, Vdl2Message& m);

// ---- information field builders (test signal, tests) ----
std::vector<uint8_t> vdl2AcarsInfo(const std::string& reg, const std::string& label, char blockId, const std::string& text, char ack = 0x15, char mode = '2', bool lastBlock = true);
// an X.25 data packet with a CLNP DT header and a COTP DT TPDU around `user` (the payload of an ATN application)
std::vector<uint8_t> vdl2X25ClnpData(int lcn, int ps, int pr, const std::vector<uint8_t>& srcNsap, const std::vector<uint8_t>& dstNsap, const std::vector<uint8_t>& user);
std::vector<uint8_t> vdl2X25CallRequest(int lcn, const std::string& calledBcd, const std::string& callingBcd);
// XID information: GSIF of a ground station (location, airports) or an aircraft's link establishment (location, altitude, destination)
std::vector<uint8_t> vdl2XidGsif(double lat, double lon, const std::string& airports);
std::vector<uint8_t> vdl2XidAircraftLe(double lat, double lon, int altFt, const std::string& destAirport);

} // namespace dect2
