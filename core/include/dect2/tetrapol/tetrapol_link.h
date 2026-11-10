// TETRAPOL data link and signalling, written from the public PAS 0001-3-3 v2.0.6 (transport and link), 0001-3-2 v2.3.9 (messages) and
// 0001-3-4 v1.3.0 (circuit mode LSDUs), on top of the data frames of tetrapol_codec.h:
//   data block:  FN 00 single block, 01 11 bi-block, 01 10 11 ... 11 10 then a closing 01 block that is the XOR of the others (3-3 6.2);
//                64 bits per block, packed into bytes LSB first in the order the bits are sent
//   control channel superframe of 200 blocks: BCH 0..3 and 100..103, PCH 98, 99, 198, 199 (and 48, 49, 148, 149 with MUX_TYPE 1),
//                RCH 14 + 25 i, SDCH the rest (3-3 5.1.3); traffic channel: ASB bit X marks the VCH blocks (3-3 5.2, 6.1.1 table 1)
//   HDLC frame:  address Z 1, Y 3, X 12 bits (3-3 7.4.1.4), command octet (7.5.4, table 9), information, FCS-16 x^16 + x^12 + x^5 + 1,
//                one's complement, the first 16 bits inverted (7.4.1.1); fill frames 7.4.1.9, RCH 7.4.2, PCH 7.4.3
//   TPDU:        DU without segmentation (EXT, SEG, PRIO, ID_TSAP; a length octet when the information field is longer than 3 octets,
//                3-3 9.5.1) or a connection-oriented DT / DTE (8.3.1)
//   TSDU:        CODOP values 3-2 4.5, layouts 3-2 clause 4.4, information elements 3-2 clause 5.3
// KEY_REFERENCE (3-2 5.3.41): a clear call is key type 0 or 15 with key index 0. Nothing else about keys is read: an encrypted call is never
// decoded and no key is ever used.
#pragma once
#include "dect2/tetrapol/tetrapol_codec.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace tetrapol {

constexpr int kBlockFramesMax = 16;

// Gathers data frames into data blocks. push() returns true with the block's bytes when a block is complete.
class BlockAssembler {
public:
    bool push(const DataFrame& f, bool ok, std::vector<uint8_t>& bytes);
    void reset() { n_ = 0; bad_ = 0; }
    uint64_t repaired() const { return repaired_; }
private:
    bool finish(std::vector<uint8_t>& bytes, int dataFrames);
    DataFrame fr_[kBlockFramesMax + 1];
    bool ok_[kBlockFramesMax + 1] = {};
    int fn_[kBlockFramesMax + 1] = {};
    int n_ = 0, bad_ = 0;
    uint64_t repaired_ = 0;
};
// A data block of bytes (a multiple of 8) as data frames with their frame numbers (and the parity frame of a multi-frame block).
std::vector<DataFrame> blockFrames(const std::vector<uint8_t>& bytes, int scramble, int asb0 = 0);

struct Hdlc {
    int z = 0, y = 0, x = 0;
    uint8_t cmd = 0;
    std::vector<uint8_t> info;   // between the command byte and the FCS
    bool fcsOk = false;
};
bool parseHdlc(const std::vector<uint8_t>& block, Hdlc& h);
bool fcsOk(const uint8_t* bytes, int nbytes);
// Appends the FCS-16 of b (test signals: RCH blocks)
void appendFcs(std::vector<uint8_t>& b);
// An HDLC frame of exactly nbytes (a multiple of 8) with the FCS at the end; info is padded with zeros.
std::vector<uint8_t> buildHdlc(int z, int y, int x, uint8_t cmd, const std::vector<uint8_t>& info, int nbytes);

constexpr uint8_t kCmdUi = 0x03;
// The TSDU inside an HDLC frame (UI or an unsegmented connection-oriented DT). False when there is none in this frame.
bool tsduOf(const Hdlc& h, std::vector<uint8_t>& tsdu);
// UI information field carrying one TSDU for a block of nbytes (single frame: no length byte)
std::vector<uint8_t> uiInfo(const std::vector<uint8_t>& tsdu, int nbytes);
// The smallest data block (in bytes) that holds this TSDU as a UI frame
int uiBlockBytes(size_t tsduLen);

enum Codop : uint8_t {
    kDSystemInfo = 0x90, kDGroupActivation = 0x55, kDCallConnect = 0x34, kDCallStart = 0x3e,
    kDCallEnd = 0xe2, kDGroupEnd = 0x57, kDRelease = 0x0c, kDBackCch = 0x0b, kDGroupIdle = 0x58,
};
constexpr uint8_t kIeiKeyReference = 0x83, kIeiKeyOfCall = 0x03;

struct SysInfo {
    int country = -1, systemId = -1, locArea = -1, bnId = -1, bsId = -1, rswId = -1, uScramble = -1, channelId = -1, band = 0;
    bool disconnected = false;
    int mode = 0;              // cell_state mode: 0 normal, 1..4 disconnected (internal BN, main switch, radio switch, BSC)
    int bch = 0;               // cell_state bch: this D_SYSTEM_INFO went out in frames 0..3 (0) or 100..103 (1) of the superframe
    int cellConfig = -1;       // cell_config byte (mux type in bits 2..4)
    int cellBn = -1;           // cell_bn (12 bits)
    int systemTime = -1, cellAccess = -1, superframe = -1;
    int muxType() const { return cellConfig < 0 ? 0 : (cellConfig >> 2) & 7; }
};
bool parseSysInfo(const uint8_t* t, int len, SysInfo& s);
std::vector<uint8_t> buildSysInfo(const SysInfo& s);   // normal mode (cell_state 0)

// What a TSDU says about a call. keyRef -1: no key reference in the message.
struct CallSignal {
    enum Kind { None, GroupActivation, CallConnect, CallStart, CallEnd } kind = None;
    int groupId = -1, channelId = -1, keyRef = -1, dScramble = -1;
};
bool parseCallSignal(const uint8_t* t, int len, CallSignal& c);
// PAS 0001-3-2 5.3.41: a clear call is key type 0 (RNK) or 15 (ESC) with key index 0; every other value is a key
inline bool keyRefClear(int keyRef) { return keyRef == 0x00 || keyRef == 0xf0; }
std::vector<uint8_t> buildGroupActivation(int groupId, int channelId, int dScramble, int keyRef);
std::vector<uint8_t> buildCallStart(int keyRef);   // keyRef < 0: no key reference element
std::vector<uint8_t> buildCallEnd();

// ---- everything else that is sent in the clear, as text (PAS 0001-3-2 / 0001-3-3 / 0001-3-4)
const char* codopName(int codop);   // "D_SYSTEM_INFO", ... or nullptr for a code the public PAS does not list
// The RTI address of an HDLC frame, PCH or RCH entry (z 1, y 3, x 12 bits) as "z.y.0xXXX"
std::string rtiText(int z, int y, int x);
// The variable-length address of a TSDU (PAS 0001-3-2 5.3.1): RFSI digits, PABX digits, ... `used` is set to its length in bytes.
std::string addressText(const uint8_t* p, int len, int& used);
// One TSDU as a line of text: the message name and every field the public PAS describes. Message codes that the public PAS does not list
// are shown as their code and bytes. keyRef is set when the message carries a key reference (else -1).
std::string describeTsdu(const uint8_t* t, int len, int* keyRef = nullptr);

struct Neighbour { int bnNb = -1, channelId = -1, param = -1, bsId = -1, rswId = -1; };
// D_NEIGHBOURING_CELL (0x94): the adjacent cells (channel, BN) and the cell id list when the message carries it
bool parseNeighbours(const uint8_t* t, int len, std::vector<Neighbour>& out);

// Data frames on a traffic channel with asb[0] = 1 carry LSDUs instead of TSDUs (PAS 0001-3-4): UI_VCH (command 0xc3) the voice channel
// state, UI_CD (0xa3) the talking party (TP_ADDRESS). talker is set from a TP_ADDRESS.
constexpr uint8_t kCmdUiVch = 0xc3, kCmdUiCd = 0xa3;
std::string describeVch(const uint8_t* info, int n);
std::string describeCd(const uint8_t* info, int n, std::string* talker = nullptr);
// The fill frames of PAS 0001-3-3 7.4.1.9 (address 0.7.0x000, UI, a 40-bit stuffing sequence; their FCS does not match by design)
bool isStuffing(const std::vector<uint8_t>& block);
// PCH (frames 98, 99 of the control channel): 64-bit activation bitmap, then four paged RTI addresses (no FCS). Empty when nothing is paged.
std::string describePch(const std::vector<uint8_t>& block16);
// RCH (frames 14, 39, 64, 89): three RTI addresses acknowledging random access (or a NACK: noise / collision), FCS checked
bool describeRch(const std::vector<uint8_t>& block8, std::string& text);

} // namespace tetrapol
} // namespace dect2
