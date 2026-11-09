// File broadcasts on a DVB-S2 carrier (docs/SATDUMP-TIER2-3.7.md): HimawariCast and GeoNetCast. The carrier is received by the DVB-S/S2 receiver of the app, IP comes out of
// its transport stream through the MPE extractor (dvbs_ip.h), and the UDP payloads hold Fazzt packets: a file announced by a head packet, cut into numbered parts, closed by
// a tail packet. HimawariCast files are bzip2 compressed HRIT segments, put back together here into pictures; GeoNetCast files are stored as they come.
//
// Formats from SatDump (https://github.com/SatDump/SatDump, GPL-3.0, Copyright Aang23 and contributors: src-core/common/mpeg_ts/fazzt_processor, plugins/himawari_support,
// plugins/geonetcast_support). Code written for OnAir; see THIRD_PARTY.md. What arrives is untrusted: every length is checked and file names are cleaned before use.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace wxsat {

// bzip2. Decompresses every stream of the buffer in turn (HimawariCast files may hold several, and bytes between them are skipped), checking the block and stream CRCs.
// False on damaged data or when the output would pass maxOut.
bool bunzip2(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t maxOut = 256u << 20);
// A plain bzip2 stream, for the test signal: small inputs only (the sort is simple), no attempt at good compression.
std::vector<uint8_t> bzip2Simple(const uint8_t* p, size_t n);

struct FazztFile {
    std::string name;
    uint32_t id = 0, size = 0;
    int parts = 0, have = 0;
    bool complete = false;
    std::vector<uint8_t> data;       // `size` bytes; parts that did not arrive are zero
};

struct FazztStats { uint64_t frames = 0, heads = 0, bodies = 0, tails = 0, bad = 0, orphan = 0, repeats = 0, evicted = 0; };

class FazztAssembler {
public:
    explicit FazztAssembler(int payloadBytes) : payload_(payloadBytes) {}
    // One Fazzt packet; files finished by it are appended to `out`. `now`: seconds of signal time, for the timeouts.
    void push(const uint8_t* f, size_t n, double now, std::vector<FazztFile>& out);
    void expire(double now, std::vector<FazztFile>& out);   // what has had no packet for maxSilence goes out incomplete
    void clear();
    size_t filesOpen() const { return open_.size(); }
    FazztStats stats;
    double maxSilence = 120;
    size_t maxOpenBytes = 768u << 20, maxFileBytes = 256u << 20;
private:
    struct Open { FazztFile f; std::vector<bool> got; double last = 0; bool sized = false; };
    void finish(uint32_t id, double now, std::vector<FazztFile>& out);
    int payload_;
    size_t openBytes_ = 0;
    std::map<uint32_t, Open> open_;
    std::map<uint32_t, double> done_;   // finished recently: the carousel repeats heads and parts
};

// Lower-case-safe file name for the disk: no separators, no leading dots, no control characters, at most 200 bytes. Empty when nothing is left.
std::string cleanFileName(const std::string& name);

// HimawariCast / JMA HRIT segments back together: "IMG_DK01IR1_201703080230_001". Counts are 10 bit in 16 bit words; the top count (1023) means no data (space).
struct HritSegment { std::string channel, stamp; int seg = 0, total = 0, columns = 0, lines = 0; bool ok = false; };
HritSegment parseHritName(const std::string& name);

class HritMosaic {
public:
    // name: without ".bz2". True when it was a segment of a picture. A finished or superseded picture goes to onImage.
    bool push(const std::string& name, const std::vector<uint8_t>& hrit);
    void flush();                    // everything still open goes to onImage (partial ones too)
    std::function<void(const std::string& channel, const std::string& stamp, int w, int h, const uint8_t* px, int have, int total)> onImage;   // 8 bit, full size
    std::function<void(const std::string& channel, const std::string& stamp, int w, int h, const uint8_t* px, int have, int total)> onProgress;   // a segment has landed
    uint64_t bad = 0;
private:
    struct Pic { std::string channel, stamp; int w = 0, segH = 0, total = 0, have = 0; std::vector<bool> got; std::vector<uint8_t> px; };
    void emit(Pic& p);
    std::map<std::string, Pic> open_;   // by channel
};

// The file layer of a profile, without the radio part: transport stream packets in, files and pictures out. The packet side may run on the receiver thread; files are
// handled on a worker thread of its own so that the receiver never waits for the disk or for bzip2.
class FilecastLayer {
public:
    FilecastLayer(int profile);
    ~FilecastLayer();
    void setSaveDir(const std::string& dir);        // "" keeps nothing on disk
    void setPid(int pid);                           // 0: the PID of the link
    int pid() const;
    void pushTs(const uint8_t* packets, size_t nPackets, double secs);   // 188-byte packets
    void pushFile(FazztFile f);                     // a whole file, as the assembler hands it over (tests, real files)
    void pushFazzt(const uint8_t* f, size_t n, double now);              // one Fazzt packet (tests, real packet captures)
    void flush();                                   // waits for the worker, closes the open pictures (tests)
    void reset();                                   // forget the half-received files and the counters; saved files stay
    FilecastTelemetry telemetry() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The test carousel of a profile as a transport stream: returns one 188-byte packet at a time, forever
std::function<void(uint8_t*)> filecastTestTs(int profile);
constexpr int kFcTestColumns = 640, kFcTestSegLines = 64, kFcTestSegments = 10;
int filecastTestPixel(int x, int y);   // the 10 bit count of the test picture

} // namespace wxsat
} // namespace dect2
