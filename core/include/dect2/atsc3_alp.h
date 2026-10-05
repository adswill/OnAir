// ATSC 3.0 link layer, ALP (A/330 section 5): packet headers (single, segmented, concatenated), signaling and MPEG-2 TS encapsulation, and
// the reassembly of ALP packets from the payloads of baseband packets (a packet can start in one baseband packet and end in the next).
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc3 {

enum AlpType { AlpIpv4 = 0, AlpCompressedIp = 2, AlpSignaling = 4, AlpTypeExt = 6, AlpTs = 7 };

// One input packet as it enters or leaves the link layer.
struct AlpPacket {
    int type = AlpIpv4;
    std::vector<uint8_t> data;       // IPv4 packet, compressed IP packet, signaling table, or (TS) the 188 byte packets
    int sid = -1;                    // sub-stream identifier, -1 if none
    // signaling_information_hdr (type 4)
    int signalingType = 0, signalingExt = 0, signalingVersion = 0, signalingFormat = 0, signalingEncoding = 0;
    int extendedType = 0;            // type 6
};

// ---- encapsulation (used by tests and for generating streams)
// A whole input packet in one ALP packet (base header only when it fits and nothing optional is needed).
std::vector<uint8_t> alpSingle(const AlpPacket& p);
// An input packet in several segments of at most `segmentBytes` payload bytes each.
std::vector<std::vector<uint8_t>> alpSegments(const AlpPacket& p, int segmentBytes, int firstSequence = 0);
// Several input packets (2 to 9, IPv4 or compressed IP of the same type) in one ALP packet.
std::vector<uint8_t> alpConcatenate(const std::vector<AlpPacket>& packets);
// MPEG-2 TS packets (188 bytes each, 1 to 16) in one ALP packet; sync bytes are removed, `deletedNull` null packets before them are recorded.
std::vector<uint8_t> alpTs(const std::vector<uint8_t>& ts188, int deletedNull = 0);

// ---- reassembly
class AlpReassembler {
public:
    // Feeds the payload of one baseband packet (the bytes after its header) and `pointer`: the offset of the first ALP packet that starts in
    // it, or 8191 when none starts. Complete input packets are appended to `out`.
    void push(const uint8_t* payload, int size, int pointer, std::vector<AlpPacket>& out);
    void reset();   // after a lost baseband packet
    long packetsOut() const { return packets_; }
    long errors() const { return errors_; }
private:
    // returns the total length of the ALP packet at the start of buf, 0 when more bytes are needed, -1 when it is invalid
    int packetLength(const std::vector<uint8_t>& buf) const;
    void emit(const std::vector<uint8_t>& packet, std::vector<AlpPacket>& out);
    std::vector<uint8_t> buf_;
    bool synced_ = false;          // buf_ starts at an ALP packet boundary
    // segmentation state
    bool inSegments_ = false;
    int segNext_ = 0;
    AlpPacket segPacket_;
    long packets_ = 0, errors_ = 0;
};

// Parses one complete ALP packet (as cut by the reassembler); appends the input packets it carries.
bool alpParse(const std::vector<uint8_t>& alp, std::vector<AlpPacket>& out, bool* isSegment = nullptr, int* segSeq = nullptr, bool* lastSeg = nullptr);

} // namespace atsc3
} // namespace dect2
