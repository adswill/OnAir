// SSDV (Slow Scan Digital Video, Philip Heron M0RPH / fsphil): the packet format of amateur satellites and high-altitude balloons.
// A picture is a baseline JPEG cut into 256-byte packets (sync 0x55, type 0x66 with Reed-Solomon (255,223) or 0x67 without, a base-40 callsign,
// image id, packet number, size, the MCU where the packet starts, the entropy-coded payload, CRC-32, 32 parity bytes). The check, the
// Reed-Solomon correction and the JPEG rebuild are the code of fsphil/ssdv (GPL-3.0-or-later, third_party/ssdv, THIRD_PARTY.md); this header is
// the C++ face of it. Only packets of 256 bytes are handled (the usual size).
#pragma once
#include <utility>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace ssdv {

constexpr int kPktSize = 256;

struct Info {
    int type = 0;              // 0 normal (Reed-Solomon), 1 no FEC
    std::string call;
    int imageId = 0;
    int packetId = 0;
    int width = 0, height = 0; // pixels
    bool eoi = false;          // the last packet of the picture
    int quality = 0;           // 0 .. 7
    int mcuMode = 0;
    int mcuOffset = 0, mcuId = 0, mcuCount = 0;
};

// pkt: 256 bytes as received (byte 0 is the sync, any value is replaced by 0x55). A good CRC-32 is required; errors are corrected in place
// when the packet is of the normal type. Returns false for anything else. rsErrors gets the number of corrected bytes.
bool check(uint8_t* pkt, int* rsErrors, Info* info);
Info header(const uint8_t* pkt);

// The packets of one picture (one callsign and image id). add() takes a checked packet; jpeg() rebuilds the JPEG from every packet held
// (missing parts are filled by the decoder), so packets can come in any order and repeat.
class Assembler {
public:
    bool add(const uint8_t* pkt);             // false: a copy of a packet already held
    size_t packets() const { return pk_.size(); }
    int width() const { return w_; }
    int height() const { return h_; }
    int quality() const { return q_; }
    int mcuCount() const { return mcuCount_; }
    int lastPacketId() const { return eoiId_; }   // -1 until the last packet has been seen
    bool complete() const;                    // packets 0 .. last are all held
    int missing() const;                      // packet numbers missing before the last one (or before the highest held when the end is unknown)
    std::vector<uint8_t> jpeg() const;        // empty before the first packet
    std::vector<std::pair<int, int>> gapMcus() const;   // MCU ranges [first, last] that lost packets may have left grey
private:
    std::map<int, std::array<uint8_t, kPktSize>> pk_;
    int w_ = 0, h_ = 0, q_ = 0, mcuCount_ = 0, eoiId_ = -1;
};

struct Rgb { int w = 0, h = 0; std::vector<uint8_t> rgb; };
// Paints the MCUs of the gaps that really are flat grey from the picture above and below them, so a lost packet is a soft patch instead of a grey band. Returns the MCUs filled.
int fillGaps(Rgb& im, const std::vector<std::pair<int, int>>& gapMcus, int mcuSize = 16);
bool decodeJpeg(const uint8_t* data, size_t n, Rgb& out);   // baseline JPEG to RGB (stb_image)

// Test signal side: a baseline JPEG (dimensions multiples of 16, 4:2:0, standard tables) cut into packets by the SSDV encoder
std::vector<std::array<uint8_t, kPktSize>> encodePackets(const std::vector<uint8_t>& jpeg, const std::string& call, int imageId, int quality, bool noFec);
// A small test picture (gradient and shapes), as an SSDV-ready baseline JPEG
std::vector<uint8_t> testJpeg(int w, int h);

} // namespace ssdv
} // namespace dect2
