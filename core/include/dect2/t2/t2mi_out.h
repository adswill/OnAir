// DVB-T2 baseband export: the BB frames of the decoded PLP written to a file and/or sent by UDP, for tools outside OnAir (t2mi extractors,
// re-modulators, analysers). Two formats:
//   Bbf  raw BB frames, each BBHEADER (10 bytes, descrambled) + DATA FIELD + padding, kBch/8 bytes per frame, back to back.
//   T2mi the same frames as T2-MI "baseband frame" packets (ETSI TS 102 773, packet type 0x00) carried in 188-byte TS packets on one PID.
// Only the selected PLP is exported (the one OnAir decodes). Blocks that failed BCH are left out (the T2-MI packet counter shows the gap).
// Not exported: T2-MI L1-current / timestamp packets (OnAir does not rebuild them), so a re-modulator needs them from elsewhere.
#pragma once
#include "dect2/t2/t2plp.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct T2ExportConfig {
    enum Format { Bbf = 0, T2mi = 1 };
    Format format = T2mi;
    std::string file;               // empty: no file
    std::string udpHost;            // dotted IPv4, empty: no UDP
    int udpPort = 0;
    int pid = 0x1000;               // T2-MI: PID of the TS packets
};

namespace t2mi {
uint32_t crc32(const uint8_t* p, size_t n);   // CRC-32 of the T2-MI packet (poly 04C11DB7, init FFFFFFFF, MSB first, no final xor)
std::vector<uint8_t> bbBytes(const std::vector<uint8_t>& bits);   // one bit per byte -> packed MSB first (padded with zero bits)
// one T2-MI baseband-frame packet: header (6) + payload (frame_idx, plp_id, flags, BB frame) + CRC-32
std::vector<uint8_t> bbPacket(uint8_t packetCount, uint8_t superframeIdx, uint8_t frameIdx, uint8_t plpId, bool intlStart, const std::vector<uint8_t>& bbBytes);
// a T2-MI packet in TS packets of `pid` (each T2-MI packet starts a TS packet, pointer_field 0, 0xFF padding at the end); cc is the continuity counter, updated
void toTs(const std::vector<uint8_t>& pkt, int pid, uint8_t& cc, std::vector<uint8_t>& ts);

// the other way, for tests and tools: TS packets of `pid` in, BB frames out
class Extract {
public:
    explicit Extract(int pid) : pid_(pid) {}
    void feed(const uint8_t* ts188);
    std::vector<std::vector<uint8_t>> frames;   // BB frame bytes, in order
    std::vector<uint8_t> plpIds, frameIdx, counts;
    uint64_t crcBad = 0;
private:
    void parse();
    int pid_;
    std::vector<uint8_t> buf_;
    bool synced_ = false;
};
}   // namespace t2mi

class T2Export {
public:
    T2Export();
    ~T2Export();
    T2Export(const T2Export&) = delete;
    T2Export& operator=(const T2Export&) = delete;
    // false (and error()): the file cannot be opened, a bad address or port. An empty config (no file, no UDP) switches the export off.
    bool configure(const T2ExportConfig& c);
    void onPlp(const PlpResult& r);   // call for every decoded result
    bool active() const { return active_; }
    std::string error() const { return err_; }
    uint64_t frames() const { return frames_; }
    uint64_t bytes() const { return bytes_; }
    void close();
private:
    void emit(const std::vector<uint8_t>& b);
    void flushUdp();
    T2ExportConfig cfg_;
    bool active_ = false;
    std::string err_;
    FILE* f_ = nullptr;
    intptr_t sock_ = -1;
    std::vector<uint8_t> udpBuf_;
    uint64_t frames_ = 0, bytes_ = 0;
    uint8_t count_ = 0, cc_ = 0, sf_ = 0;
    int lastIdx_ = -1;
};

}   // namespace dect2
