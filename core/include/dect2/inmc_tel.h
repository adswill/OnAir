// Inmarsat-C receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One EGC message (SafetyNET or FleetNET), assembled from its packets.
struct InmcMessage {
    uint16_t id = 0;             // message sequence number from the packet header
    uint8_t serviceCode = 0;     // service/address code byte (for example 0x31 NAVAREA/METAREA warning)
    int kind = 0;                // 0 other or system, 1 SafetyNET, 2 FleetNET
    int priority = 0;            // 0 routine, 1 safety, 2 urgency, 3 distress
    std::string serviceText;     // the service code in words
    std::string area;            // the address bytes after the first one, in hex (the area layout is not confirmed, so it is not decoded)
    uint8_t addr0 = 0;           // first address byte
    int sat = -1, lesId = -1;    // addr0 read as satellite (2 bits) and LES (6 bits), as in every other packet. TENTATIVE: not confirmed for EGC packets
    std::string lesName;         // name for that guess ("" when the id is not in the table)
    int repetition = 0;          // repetition field of the header
    int presentation = 0;        // 0 IA5, 6 ITA2, 7 binary
    int packets = 0;             // packets received for this message
    bool complete = false;       // the last packet (continuation bit clear) and all before it were received
    bool truncated = false;      // the text was cut to keep the report small
    uint32_t seen = 0;           // how many times the complete message was received
    uint32_t frameNo = 0;        // frame number of the first packet
    int64_t rxUnix = 0;          // time received, seconds since 1970 (UTC)
    std::string rxTime;          // the same as HH:MM:SS UTC
    std::string text;            // text assembled across packets and frames
    int channel = 0;             // 0 the channel tuned to, 1 and up: other channels found in the band (see InmcChannelInfo)
    uint64_t order = 0;          // grows with every packet received; the report sorts by it
};

// One Inmarsat-C channel decoded at the same time. Channel 0 is the one the user tuned to; the others were found by the channel search.
struct InmcChannelInfo {
    int index = 0;
    double offsetHz = 0;         // where the receiver looks, relative to the centre of the capture
    double carrierHz = 0;        // carrier found, relative to the centre of the capture (offset plus carrier error)
    int state = 0;               // 0 searching, 1 carrier locked, 2 frames decoding
    float ebn0Db = 0;
    float uwErrorsAvg = 0;
    uint32_t frameNumber = 0;
    uint64_t framesOk = 0, framesBad = 0;
    uint32_t messages = 0;       // messages seen on this channel
    int sat = -1, lesId = -1, channelType = 0;   // from its bulletin board
    std::string region, lesName, channelTypeName;
};

// System information from the bulletin board packet (descriptor 0x7D) and the signalling channel packet (0x6C).
struct InmcNcsInfo {
    bool valid = false;
    int sat = -1;                // ocean region code 0 AOR-W, 1 AOR-E, 2 POR, 3 IOR
    std::string region;          // its name
    int lesId = -1;              // station that sends this channel (the NCS is 44)
    std::string lesName;
    int channelType = 0;         // 1 NCS, 2 LES TDM, 3 joint NCS and TDM, 4 stand-by NCS
    std::string channelTypeName;
    int networkVersion = 0;
    int signallingChannel = 0;
    int count = 0;
    uint8_t status = 0;          // status byte: 0x80 600 baud, 0x40 operational, 0x20 in service, 0x10 clear, 0x08 links open
    std::string statusText;
    uint16_t services = 0;       // services bit mask
    std::string servicesText;
    int randomInterval = 0;
    double signallingUplinkMhz = 0;   // from the signalling channel packet, 0 when not seen
    uint32_t frameNo = 0;        // frame number in the last bulletin board
    std::string frameTime;       // frame number times 8.64 s as hh:mm:ss.s
};

struct InmcTelemetry {
    // common prefix
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 carrier locked, 2 frames decoding
    double cfoHz = 0;            // carrier error
    float snrDb = 0;             // Es/N0 in dB, from the symbols (valid while the carrier is locked)
    bool dataValid = false;      // a message has been decoded
    uint64_t blocksOk = 0;       // frames whose bulletin board packet has a good check
    uint64_t blocksBad = 0;      // frames found by their unique word whose bulletin board did not check

    // channel
    bool carrierLock = false;    // carrier loop locked
    bool frameLock = false;      // frames found at the right spacing
    uint32_t frameNumber = 0;    // number in the last good frame
    int uwErrors = 0;            // wrong unique word symbols (of 128) in the last frame found
    float uwErrorsAvg = 0;       // average of the last frames
    float symbolErrorRate = 0;   // channel symbol errors seen by the Viterbi decoder (re-encoded result against the received symbols), last frame
    float esn0Db = 0, ebn0Db = 0;   // from the symbols; Eb/N0 = Es/N0 + 3 dB (rate 1/2)
    double driftHzS = 0;         // carrier drift
    uint64_t framesFound = 0;    // frames found by their unique word
    uint64_t polaritySlips = 0;  // frames in which the carrier loop flipped the polarity and the unique word repaired it
    uint64_t packetsOk = 0, packetsBad = 0;
    uint32_t pktOk[256] = {};    // packets with a good check, by descriptor
    uint32_t pktBad[256] = {};   // packets whose check failed, by descriptor

    InmcNcsInfo ncs;                     // of the channel tuned to
    std::vector<InmcMessage> messages;   // newest first, at most 150, from all channels
    uint32_t messageCount = 0;           // messages seen since the start (can be above the list length)
    std::vector<InmcChannelInfo> channels;   // the channel tuned to first, then the ones found in the band that have a carrier lock
};

inline std::string inmcSummary(const InmcTelemetry& t) {
    char b[160];
    if (t.state == 0) return "Inmarsat-C: searching";
    if (t.state == 1) { snprintf(b, sizeof b, "Inmarsat-C: carrier locked, Es/N0 %.1f dB", t.esn0Db); return b; }
    snprintf(b, sizeof b, "Inmarsat-C: frame %u, Eb/N0 %.1f dB, %zu messages", t.frameNumber, t.ebn0Db, t.messages.size());
    return b;
}

} // namespace dect2
