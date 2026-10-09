// HD Radio receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
// Sound: HD Radio audio uses the patented HDC codec, which OnAir does not decode. The receiver counts the audio packets and their bit rate;
// everything else on the signal (station information, program data, pictures and other data services) is decoded.
#pragma once
#include <cstdint>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One audio program: HD1 (the main program service, number 0) to HD8 (supplemental services 1 to 7)
struct HdrProgram {
    int number = 0;                 // 0..7; shown as HD<number + 1>
    std::string name;               // from the station information guide (SIG), may be empty
    int type = -1;                  // program type (news, rock, ...): from the PDU header or the station information service
    int access = 0;                 // 0 public, 1 restricted
    int codecMode = -1;             // audio codec mode of the PDU headers (HDC, not decoded)
    int blend = -1;                 // blend control of the PDU header (MPS)
    int soundExp = -1;              // sound experience of the SIS service descriptor
    double kbps = 0;                // audio bit rate measured from the packets of the last frames
    uint64_t packetsOk = 0, packetsBad = 0;   // audio packets with a good / bad CRC (not decoded)
    bool inSis = false, inSig = false, onAir = false;   // listed in the SIS, the SIG, and audio PDUs seen
    // program service data (ID3)
    uint64_t psdCount = 0;          // PSD messages received
    std::string title, artist, album, genre, comment, ufid;
    std::string comPrice, comSeller, comDesc, comUrl, comValid;   // commercial frame (COMR)
    int xhdrLot = -1;               // XHDR: the LOT object with the album art (-1: none)
    uint32_t xhdrMime = 0;
    int artPort = -1;               // the port of the program's primary image service (from the SIG), -1 if unknown
};

// A data service: from the SIG (with port and transport) or from the SIS data service descriptors
struct HdrDataService {
    bool fromSig = false;
    int service = -1;               // SIG service number
    std::string name;               // SIG service name
    int program = -1;               // the audio program it belongs to (-1: a station-wide service)
    int port = -1;                  // AAS port
    int aasType = -1;               // 0 stream, 1 packet, 3 LOT
    uint32_t mime = 0;              // MIME hash of the SIG component
    int sdType = -1;                // service data type (SIG) or the SIS data service type
    int sisMime = -1;               // SIS: 12-bit MIME type
    uint64_t packets = 0, bytes = 0;   // AAS packets received on the port
};

// A large object (LOT) file seen on a port
struct HdrLotInfo {
    int port = 0;
    int lot = 0;                    // the LOT id
    std::string name;
    uint32_t mime = 0;              // MIME hash from the LOT header
    uint32_t size = 0;              // bytes (from the header; 0 until the header came)
    uint32_t have = 0;              // bytes received so far
    bool complete = false;
    uint64_t version = 0;           // changes whenever the bytes change (complete again with new content)
    std::string expires;            // UTC, from the header
    double timeSec = 0;             // signal time when it was last updated
};

struct HdrTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal found (coarse timing and frequency), 2 locked (block sync), 3 decoding (L1 frames)
    double cfoHz = 0;            // carrier error
    float snrDb = 0;             // MER of the digital subcarriers
    float levelDb = -200;        // power of the whole input, dBFS (-200 = nothing measured yet)
    bool dataValid = false;      // station data has been decoded
    uint64_t blocksOk = 0;       // P1 transfer frames with a valid header (PCI) / without
    uint64_t blocksBad = 0;
    // set-up
    double timeSec = 0;          // signal seconds since the start (now)
    double inputRate = 0;
    // waveform
    int band = 0;                // 0 not known yet, 1 FM hybrid, 2 AM hybrid
    int serviceMode = -1;        // FM: PSMI (1 = MP1, 2 = MP2, 3 = MP3, ...); AM: 1 = MA1, 2 = MA3
    std::string modeName;        // "MP1", "MA1", ...
    float merLower = 0, merUpper = 0;   // MER of each sideband, dB
    double ber = 0;              // channel bit errors of the P1 code word (re-encoded)
    int blockCount = -1;         // L1 block count of the reference subcarriers
    uint64_t syncCount = 0;      // times the block sync was found
    uint64_t pidsOk = 0, pidsBad = 0;     // PIDS frames with a good / bad CRC
    uint64_t p3Ok = 0, p3Bad = 0;         // P3 (and P4) transfer frames: FM extended partitions; AM secondary and tertiary sidebands
    std::vector<std::complex<float>> constel;   // equalised data subcarriers of the last block (FM QPSK at +-1, AM 64-QAM up to +-3.5)
    uint64_t pduCount = 0;       // audio PDUs parsed
    uint64_t aasPackets = 0, aasBad = 0;  // AAS packets (HDLC frames) good / bad FCS
    bool p1Ok = false, pidsRecent = false, psdSeen = false, dataSeen = false;   // for the lamps
    float loadPct = 0;           // the receiver's own share of real time (measured in feed)
    // station information service (SIS)
    std::string callSign;        // short station name (4 letters, may end in -FM)
    std::string stationName;     // universal short station name
    std::string longName;
    std::string slogan;
    std::string message;
    std::string alert;           // emergency alert text
    std::string countryCode;
    int facilityId = -1;
    bool haveLocation = false;
    double latitude = 0, longitude = 0;
    int altitudeM = 0;
    std::vector<HdrProgram> programs;     // sorted by number
    std::vector<HdrDataService> dataServices;
    std::vector<HdrLotInfo> lots;         // newest first
    uint64_t lotVersion = 0;     // changes when any LOT object completes
};

const char* hdrProgramTypeName(int type);   // "Rock", "News", ... or "" for an unknown code
const char* hdrDataTypeName(int type);      // data service types: "Traffic", "Weather", ... or ""

inline std::string hdrSummary(const HdrTelemetry& t) {
    char b[200];
    if (t.state < 2) { snprintf(b, sizeof b, "HD Radio: searching, level %.1f dBFS", t.levelDb); return b; }
    snprintf(b, sizeof b, "HD Radio %s %s, MER %.1f dB, %zu program%s%s%s", t.band == 2 ? "AM" : "FM", t.modeName.c_str(), t.snrDb, t.programs.size(),
             t.programs.size() == 1 ? "" : "s", t.callSign.empty() ? "" : ", ", t.callSign.c_str());
    return b;
}

} // namespace dect2
