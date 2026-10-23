// Mercury frames above the modem (see mercury_phy.h for the waveform). Written for OnAir from Mercury's source and documents (branch
// mercuryv2, commit c72997be9d3c: datalink_arq/arq_protocol.[ch], arith.c, carousel.c, rs_erasure.c, modem/framer.h,
// data_interfaces/tcp_interfaces.c, docs/CAROUSEL-ARQ.md, docs/BROADCAST-WIRE-FORMAT.md). Receive side, plus the builders the test
// signal needs. Nothing here keys a radio.
//
// A frame is the payload of one modem frame (the CRC-16 is checked by the receiver). A frame with a plain CRC starts with the framer
// byte: packet type in bits 7..5, an extension in bits 4..0. Types: 0 ARQ control (the DISCONNECT), 1 ARQ data of 1.9.x, 2 CALL /
// ACCEPT of 1.9.x, 3 broadcast control (the RaptorQ file carousel), 4 broadcast data (messages), 5 CQ, 6 CALL / ACCEPT of Mercury 2.0.
// A frame inside a connected session has its CRC XORed with the session's seed and no framer byte: the carousel's DATA rounds on the
// payload modes and its POLL / REQ / STATUS frames on the control mode (DATAC16).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace mercury {

enum PacketType : int {
    kPtArqControl = 0, kPtArqData19 = 1, kPtCall19 = 2, kPtBcastControl = 3, kPtBcastData = 4, kPtCq = 5, kPtCall2 = 6
};
inline int packetType(uint8_t framer) { return (framer >> 5) & 7; }
inline int packetExt(uint8_t framer) { return framer & 0x1F; }
inline uint8_t framerByte(int type, int ext) { return (uint8_t)(((type & 7) << 5) | (ext & 0x1F)); }
const char* packetTypeName(int type);

// The bandwidth token of CALL / CQ (2 bits): 1 = 500 Hz, 2 = 2300 Hz, 3 = 2750 Hz (VARA's BW500 / BW2300 / BW2750)
int bwHzFromToken(int token);
int bwTokenFromHz(int hz);

// Callsigns: CRC-16/CCITT-FALSE of the upper-case callsign (the CALL names the called station only by it), and the arithmetic code of
// the caller's callsign (uniform model over A-Z, 0-9, '-' and an end symbol, 32-bit coder; arith.c). decode returns "" when the
// bytes do not decode to a callsign.
uint16_t callsignCrc(const std::string& call);
std::vector<uint8_t> encodeCallsign(const std::string& call);
std::string decodeCallsign(const uint8_t* p, size_t n);

// ------------------------------------------------------------------------------------------------ connect, CQ, disconnect

struct CallFrame {
    int sessionId = 0;           // 7 bits
    int bwHz = 0;
    int minor = 0;               // 2.x minor revision (the framer extension's bits 4..2)
    uint16_t calledCrc = 0;      // CRC-16 of the called callsign
    std::string caller;
};
struct AcceptFrame {
    int sessionId = 0;
    int bwHz = 0;
    int minor = 0;
    uint16_t callerCrc = 0, calledCrc = 0;
    uint16_t nonce = 0;
    int startLevel = 0;          // the rung the caller's first round goes out on (0 MFSK .. 6 QAM16C2)
    bool ctlDeaf = false;        // the callee hears the caller below the control mode
    float callSnrDb = -1000;     // what the callee read the CALL at (-1000: not given)
    bool reservedSet = false;    // a reserved bit or byte is not zero (plain 2.0 drops it; a later revision, or encryption, may use them)
};
struct CqFrame { int bwHz = 0; std::string caller; };
struct DisconnectFrame { int sessionId = 0; float snrDb = -1000; };

// 14 bytes; false when the frame is not that kind
bool parseCall(const uint8_t* p, size_t n, CallFrame& out);
bool parseAccept(const uint8_t* p, size_t n, AcceptFrame& out);
bool parseCq(const uint8_t* p, size_t n, CqFrame& out);
bool parseDisconnect(const uint8_t* p, size_t n, DisconnectFrame& out);
std::vector<uint8_t> buildCall(int sessionId, const std::string& caller, const std::string& called, int bwHz);
std::vector<uint8_t> buildAccept(int sessionId, const std::string& called, const std::string& caller, int bwHz, int startLevel,
                                 bool ctlDeaf, float callSnrDb, uint16_t nonce);
std::vector<uint8_t> buildCq(const std::string& caller, int bwHz);
std::vector<uint8_t> buildDisconnect(int sessionId, float snrDb);

// The session's CRC seed (arq_fsm.c car_seed): CRC-16 of the session id, the nonce (low byte first), the piece size, the caller's
// callsign, '/', the callee's (as the caller called it), upper case; never 0 (0x5A5A instead)
uint16_t sessionSeed(int sessionId, uint16_t nonce, int startLevel, const std::string& caller, const std::string& callee);
// The piece size of a session whose caller's rounds start on rung startLevel: 23 at the floor (rung 0), 24 otherwise
inline int pieceSize(int startLevel, bool ctlDeaf = false) { return (startLevel == 0 || ctlDeaf) ? 23 : 24; }

// ------------------------------------------------------------------------------------------------ the carousel (in session)

// The ARQ ladder: rung -> mode name ("MFSK", "DATAC15", "DATAC4", "DATAC3", "DATAC1", "DATAC17", "QAM16C2")
const char* rungName(int level);
int rungOfMode(int ofdmModeId);   // -1 when the mode is not a payload rung

struct CarCtl {                  // POLL / REQ / STATUS (14 bytes on DATAC16, or behind 0x07 on MFSK)
    int type = 0;                // 0 POLL, 1 REQ, 2 STATUS, 3 POLL saying the callee's application has hung up
    bool hasData = false, ctlDeaf = false;
    int pollId = 0;
    int level = 0, frames = 0;   // "send me up to `frames` frames on rung `level`"
    int loss16 = 0;              // the loss the receiver measured, in 16ths
    int base = 0;                // window base, mod 16
    int need[8] = {};            // pieces each open block still needs (-1: block not seen)
    int gap = -1;                // the first data piece missing from the base block
    float snrDb = -1000;         // how the sender of this frame hears its peer (-1000: no reading)
    int hLevel = 0, hFrames = 0, hId = 0;   // a handover: the round the sender announces in the same keydown
    int highBlock = 0;           // highest block opened, mod 16
    bool unopened = false, shortRung = false;
    int backlog = 0;
};
bool parseCarCtl(const uint8_t* p, size_t n, CarCtl& out);
std::vector<uint8_t> buildCarCtl(const CarCtl& c);

struct CarSegment {
    int block = 0;               // block id, mod 16
    int K = 0;                   // data pieces of the block
    int count = 0, first = 0;    // pieces carried here, the index of the first (consecutive, mod 256)
    int pad = 0;                 // pad bytes in the block's last data piece
    size_t offset = 0;           // where the pieces start in the frame
};
struct CarData {
    int left = 0;                // frames that follow in the round
    bool unopened = false;
    int slot = 0;
    int pollId = 0, highBlock = 0;
    bool ctlDeaf = false, patAnchor = false;
    bool hasInlineCtl = false;
    CarCtl inlineCtl;
    std::vector<CarSegment> segs;
};
bool parseCarData(const uint8_t* p, size_t n, int pieceSize, CarData& out);
// one DATA frame of `frameLen` bytes carrying `count` pieces of one block (generator)
std::vector<uint8_t> buildCarData(size_t frameLen, int left, int pollId, int highBlock, int block, int K, int first, int count,
                                  int pad, const uint8_t* pieces, int pieceSize);

// Reed-Solomon erasure code of the carousel (rs_erasure.c: systematic, Cauchy rows over GF(256), polynomial 0x11D)
bool rsDecode(int K, int P, const std::vector<int>& idx, const std::vector<std::vector<uint8_t>>& pieces, std::vector<std::vector<uint8_t>>& out);
void rsRepair(int K, int P, const std::vector<std::vector<uint8_t>>& data, int j, std::vector<uint8_t>& out);
// A block's check: CRC-32 of the key (4 bytes, low first), the block id and K, then the block's bytes
uint32_t blockCheck(uint32_t key, int blockId, int K, const uint8_t* bytes, size_t n);

// ------------------------------------------------------------------------------------------------ broadcast

// A message (type 4, extension bit 0 set): 2-byte big-endian length, then the bytes. Extension bit 1: the sender used standard KISS
// (e.g. Reticulum), bit 2: unformatted KISS data (a beacon); neither: VARA-style framing of the client (shown as it is, not parsed).
struct BcastMessage { int ext = 0; bool lenPrefix = false; std::vector<uint8_t> bytes; };
bool parseBcastMessage(const uint8_t* p, size_t n, BcastMessage& out);
std::vector<uint8_t> buildBcastMessage(const uint8_t* msg, size_t n, size_t frameLen, int kissExt = 4);

// RaptorQ file carousel frame (type 3, every frame self-describing; BROADCAST-WIRE-FORMAT.md section 3)
struct BcastFileFrame {
    int session = 0;             // 1 .. 31
    uint32_t F = 0;              // transfer length, bytes
    int T = 0;                   // symbol size
    int Z = 1, N = 1;            // source blocks, sub-blocks
    int sbn = 0;
    uint32_t esi = 0;            // of the first symbol in the frame
    int symbols = 0;             // whole symbols in the frame
    size_t offset = 12;
};
bool parseBcastFile(const uint8_t* p, size_t n, BcastFileFrame& out);
// The bundle inside the object: uint32 LE length, the base name ending in '\n', the file. Rejects paths, ".", ".." and bad lengths.
bool parseBundle(const std::vector<uint8_t>& obj, std::string& name, size_t& offset, size_t& len);

} // namespace mercury
} // namespace dect2
