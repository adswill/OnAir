// Weather satellite links that follow SatDump's list (docs/SATDUMP-TIER1-3.7.md): one profile per mode-list entry, one receiver class.
// Telemetry, pictures and the profile table, shared by the receiver and the screens.
//
// Formats and parameters from SatDump (https://github.com/SatDump/SatDump, GPL-3.0, Copyright Aang23 and contributors); see THIRD_PARTY.md.
#pragma once
#include "dect2/elektro/elektro_ggak.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

enum WxsatProfile {
    kWxApt = 0,        // NOAA APT, 137 MHz FM, 2 lines a second (NOAA-15 / 18 / 19 retired)
    kWxNoaaHrpt,       // NOAA HRPT, PM Manchester, 665.4 kbit/s (retired)
    kWxNoaaGac,        // NOAA GAC, BPSK 2.6616 Msym/s (retired)
    kWxNoaaDsb,        // NOAA DSB (TIP), PM Manchester, 8320 bit/s on 137.35 / 137.77 MHz (retired)
    kWxMeteorHrpt,     // Meteor-M HRPT, PM Manchester, 665.4 kbit/s on 1700 MHz
    kWxElektroRdas,    // Elektro-L RDAS, BPSK 30.72 Msym/s, NRZ-M, X-band 7500 MHz
    kWxArktikaRdas,    // Arktika-M RDAS, the same at 7865 MHz
    kWxArktikaGgak,    // Arktika-M GGAK, PM 5 ksym/s on 1703 MHz
    kWxAws,            // Arctic Weather Satellite, QPSK 1.785 Msym/s on 1707 MHz, CCSDS with Reed-Solomon interleave 5
    kWxMsgRaw,         // Meteosat Second Generation raw data, QPSK 3.75 Msym/s on 1686.83 MHz, the same coding as AWS, SEVIRI
    kWxHimawariCast,   // HimawariCast: DVB-S2 2.586 Msym/s, C-band 4148 MHz, files of HRIT segments (filecast.h)
    kWxGeoNetCast,     // GeoNetCast Americas: DVB-S2 30 Msym/s, C-band 4080 MHz, files
    kWxNumProfiles
};

constexpr int kWxFirstFamily = 260;     // numbers: docs/MODE-NUMBERS.md (family, active standard = family + 1, engine standard = family + 2)
constexpr int kWxFirstPollSlot = 220;

struct WxProfileInfo {
    const char* id;
    const char* name;
    int family;
    double defMhz;
    double sampleRate;      // what the app asks the radio for
    double minRate;         // below this the receiver says so
    double symbolRate;      // symbols (chips for the Manchester links) a second; 0: APT
    const char* product;    // the first picture's instrument
    bool retired;
};
const WxProfileInfo& wxProfile(int p);

// One scan line, 8 bit, channel major: px[c * width + x]. Immutable once published.
struct WxLine {
    int idx = 0;
    std::vector<uint8_t> px;
};

struct WxImage {
    uint64_t id = 0;
    std::string name;                   // "AVHRR", "MSU-MR", "HIRS" ...
    int width = 0, channels = 0, lines = 0;
    std::vector<std::shared_ptr<const WxLine>> line;   // by index; nullptr where nothing arrived
};

// File broadcasts (filecast.h): what has arrived
struct FilecastFileRow { std::string name, path; uint64_t size = 0; int parts = 0, have = 0; bool complete = false; };
struct FilecastPicture {
    std::string channel, stamp, path;   // path: the PNG, when saved
    int w = 0, h = 0, have = 0, total = 0;
    std::shared_ptr<const std::vector<uint8_t>> px;   // 8 bit, w x h: reduced for the screen
    uint64_t id = 0;
};
struct FilecastTelemetry {
    int pid = 0;
    uint64_t tsPackets = 0, datagrams = 0, notUdp = 0, frames = 0, badFrames = 0, filesComplete = 0, filesPartial = 0, filesFailed = 0, filesDropped = 0, bytes = 0, open = 0;
    std::string lastError, saveDir;
    std::vector<FilecastFileRow> recent;       // newest first
    std::vector<FilecastPicture> pictures;     // one per channel, newest of each
    std::string carrier;                       // the DVB-S2 side in words: MODCOD, frame count
    uint64_t picturesDone = 0;
};

struct WxsatTelemetry {
    uint64_t seq = 0;
    int profile = 0;
    int state = 0;                      // 0 searching, 1 carrier and symbols, 2 frame sync, 3 frames good
    double cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double symbolRate = 0, inputRate = 0, timeSec = 0;
    float carrierLock = 0;
    bool frameLock = false;
    uint64_t asmHits = 0, rsCorrected = 0, packets = 0, lines = 0, tipFrames = 0;
    int scid = -1, vcid = -1;
    std::string satName, hint;
    uint64_t vcidFrames[64] = {};
    std::vector<std::pair<float, float>> constellation;
    std::shared_ptr<const WxImage> image;   // main picture
    std::shared_ptr<const WxImage> image2;  // HIRS of the NOAA digital links, MSU-GS visible (reduced) of RDAS
    uint64_t imageSeq = 0;
    GgakTelemetry ggak;                 // Arktika GGAK
    FilecastTelemetry files;            // HimawariCast, GeoNetCast
    uint64_t cyclesDone = 0;            // MSG: full discs finished; imageDone holds the last of them
    std::shared_ptr<const WxImage> imageDone, image2Done;
};

inline std::string wxsatSummary(const WxsatTelemetry& t) {
    char b[160];
    const WxProfileInfo& p = wxProfile(t.profile);
    if (t.profile == kWxArktikaGgak) {
        if (t.ggak.state >= 2) snprintf(b, sizeof b, "GGAK, %llu frames", (unsigned long long)t.ggak.blocksOk);
        else snprintf(b, sizeof b, "GGAK, searching");
        return b;
    }
    if (t.profile == kWxHimawariCast || t.profile == kWxGeoNetCast) {
        if (t.state >= 3 || t.files.filesComplete + t.files.filesPartial > 0) snprintf(b, sizeof b, "%s, %llu files, %llu pictures", p.name, (unsigned long long)(t.files.filesComplete + t.files.filesPartial), (unsigned long long)t.files.picturesDone);
        else if (t.state >= 1) snprintf(b, sizeof b, "%s, carrier found, no files yet", p.name);
        else snprintf(b, sizeof b, "%s, searching", p.name);
        return b;
    }
    if (t.state >= 3) snprintf(b, sizeof b, "%s, %d lines, %llu frames", p.name, t.image ? t.image->lines : 0, (unsigned long long)t.blocksOk);
    else if (t.state == 2) snprintf(b, sizeof b, "%s, frame sync, frames failing", p.name);
    else if (t.state == 1) snprintf(b, sizeof b, "%s, symbols, no frame sync", p.name);
    else snprintf(b, sizeof b, "%s, searching", p.name);
    return b;
}

} // namespace dect2
