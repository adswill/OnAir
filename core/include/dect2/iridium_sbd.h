// Iridium short burst data (Phase B): IDA frames joined into packets, the SBD headers, ACARS messages inside them.
// Rules from iridium-toolkit (iridiumtk/reassembler/ida.py and sbd.py), read from its source; not checked against a real
// recording. ACARS parity and any ACARS checksum are not checked (the IDA frames carry their own CRC).
#pragma once
#include "iridium_frame.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct IridiumIdaPacket {
    std::vector<uint8_t> data;
    bool downlink = true;
    double timeSec = 0, freqHz = 0;
    int fragments = 0;
    bool complete = true;            // false: timed out before its last fragment
};

// Joins IDA fragments (ida.py): a packet starts with counter 0; the next fragment has counter + 1 (mod 8), the same direction,
// a frequency within 260 Hz and comes at most 280 ms later; the fragment without the "more" flag ends it. A packet that waits
// longer than 1 s is given up.
class IridiumIdaAssembler {
public:
    // f must be an IDA frame that checked out; returns the packets this fragment completed (or that timed out)
    std::vector<IridiumIdaPacket> feed(const IridiumFrame& f, bool downlink, double freqHz, double timeSec);
    void reset() { open_.clear(); }
private:
    struct Open { IridiumIdaPacket p; int ctr; double lastT, lastF; };
    std::vector<Open> open_;
};

struct IridiumAcars {
    bool valid = false;
    char mode = 0;
    std::string reg;                 // aircraft registration (7 characters, leading dots kept)
    char ack = 0;
    std::string label;               // 2 characters
    char blockId = 0;
    std::string seq, flight;         // from the aircraft only (uplink frames): message sequence number, flight number
    std::string text;
    bool more = false;               // ended with ETB: another block follows
};

struct IridiumSbd {
    bool valid = false;
    int type = -1;                   // the first two bytes (0x0600, 0x7608 ...)
    std::vector<uint8_t> payload;    // after the headers
    IridiumAcars acars;              // when the payload is an ACARS message (first byte 0x01)
};

// sbd.py: the type bytes, the header that follows (29 bytes after 0x0600; 7 or 5 after 0x76xx depending on its first byte
// 0x26 / 0x20), then an optional message header 0x10 len count; ACARS when the rest starts with 0x01.
IridiumSbd iridiumParseSbd(const std::vector<uint8_t>& packet, bool downlink);

// Builders for the test signal (the reverse of the above): an ACARS message to an aircraft as an SBD packet (type 0x7608),
// and the packet cut into IDA payloads of at most 20 bytes.
std::vector<uint8_t> iridiumBuildSbdAcars(char mode, const std::string& reg, char ack, const std::string& label, char blockId, const std::string& text);
std::vector<std::vector<uint8_t>> iridiumSplitIda(const std::vector<uint8_t>& packet);

} // namespace dect2
