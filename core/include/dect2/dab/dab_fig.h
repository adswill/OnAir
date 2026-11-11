// DAB fast information channel analysis: every FIG the receiver sees, kept as raw bytes, and a decoder that turns each one into a tree of
// fields (ETSI EN 300 401 clause 5.2 and 8, TS 101 756). The field decoding follows the FIG analyser of etisnoop
// (https://github.com/Opendigitalradio/etisnoop, GPL-3.0-or-later, Copyright (C) 2014 CSP Innovazione nelle ICT, 2018 Matthias P. Braendli,
// 2015 Data Path), reworked to work on a single FIG with no global state.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One distinct FIG as received (same header and payload), counted
struct DabFigRecord {
    int type = 0;                 // FIG type 0..7 (7 is the end marker, never stored)
    int ext = 0;                  // extension: FIG 0 and 5: 5 bits; FIG 1 and 2: 3 bits; other types 0
    int cn = 0, oe = 0, pd = 0;   // FIG 0 header flags (C/N, other ensemble, programme / data); oe is also set for FIG 1 and 2
    std::vector<uint8_t> data;    // the FIG data field, first byte is the header byte with the extension
    uint64_t count = 0;           // times received
    uint64_t firstFrame = 0, lastFrame = 0;   // receiver frame numbers (96 ms each)
    int lastFib = 0;              // FIB (0..11) of the frame that carried it last
};

// Totals per FIG type and extension, over all the instances
struct DabFicStat {
    int type = 0, ext = 0;
    uint64_t count = 0;           // FIGs received
    uint64_t bytes = 0;           // bytes in the FIC, 1 byte header included
    uint64_t firstFrame = 0, lastFrame = 0;
};

struct DabFicAnalysis {
    uint64_t seq = 0;
    uint64_t frames = 0;          // frames processed
    uint64_t fibOk = 0, fibBad = 0;
    uint64_t fillBytes = 0;       // bytes of FIGs (headers included) in the good FIBs, over the frames
    uint64_t fibCount = 0;        // good FIBs counted in fillBytes
    int lastFill[12] = {};        // bytes used in each FIB of the last frame (30 available), -1 = FIB with a bad CRC
    std::vector<DabFigRecord> figs;
    std::vector<DabFicStat> stats;
};

struct DabFigLine {
    int level = 0;
    std::string text;
};

struct DabFigDecoded {
    std::vector<DabFigLine> lines;        // a line belongs to the closest line above it with a lower level
    std::vector<std::string> errors;      // invalid values the decoder noticed
};

// What the decoding of one FIG needs from the others
struct DabFigContext {
    int intlTable = 1;                    // international table id (FIG 0/9): programme type names
    int mode = 1;                         // transmission mode (range of the TII main ids)
    std::map<int, std::pair<double, double>> mainPos;   // FIG 0/22 main identifier key (oe << 8 | pd << 7 | main id) -> latitude, longitude
    const std::vector<DabFigRecord>* all = nullptr;     // all the records: the segments of an extended label (FIG 2) are put together from them
};

// What the FIGs say beyond the sub-channel / service / label tables of DabEnsemble (those the receiver follows itself): the data of the analyser panel
struct DabFicInfo {
    int country = -1, eref = -1;                 // FIG 0/0
    bool alarm = false;
    int cifHigh = -1, cifLow = -1;               // CIF counter (mod 20 / mod 250)
    bool hasEcc = false; int ecc = 0;            // FIG 0/9
    int intlTable = 1;
    bool hasLto = false; int ltoHalfHours = 0;   // ensemble local time offset, in half hours
    bool hasTime = false; int mjd = 0, hour = 0, minute = 0, second = -1;   // FIG 0/10
    int services = -1, reconfCount = -1;         // FIG 0/7
    std::map<uint32_t, int> pty;                 // SId -> programme type (FIG 0/17)
    std::map<uint32_t, int> language;            // SId -> language (FIG 0/17)
    std::map<int, int> subLanguage;              // sub-channel id -> language (FIG 0/5)
    std::map<int, int> subFec;                   // sub-channel id -> FEC scheme (FIG 0/14)
    std::map<uint32_t, unsigned> announce;       // SId -> announcement support flags (FIG 0/18)
    std::map<uint32_t, std::vector<int>> userApps;   // SId -> user application types (FIG 0/13)
    std::map<uint32_t, std::vector<uint16_t>> otherEnsembles;   // SId -> ensembles that carry it too (FIG 0/24)
    struct Freq { int rm = 0; uint32_t id = 0; std::vector<double> kHz; };   // FIG 0/21: rm 0 DAB ensemble (id = EId), 8/9 FM (RDS PI), 10/12 AM, 6 DRM, 14 AMSS
    std::vector<Freq> freqs;
    struct Link { int lsn = 0; bool active = false, hard = false, intl = false; std::vector<uint32_t> ids; };   // FIG 0/6
    std::vector<Link> links;
    struct Tx { int mainId = 0; double lat = 0, lng = 0; };   // FIG 0/22 main identifiers
    std::vector<Tx> transmitters;
    // FIG 0/22 sub-identifiers: a transmitter of the network at an offset from its main identifier's position (the main one must have been heard)
    struct SubTx { int mainId = 0, subId = 0, delayUs = 0; double lat = 0, lng = 0; bool placed = false; };
    std::vector<SubTx> subTransmitters;
    // FIG 0/11 region definitions (EN 300 401 V1.4.1 clause 8.1.16.1, dropped from V2): the transmitters (MainId, SubId) that serve a region,
    // or a rectangle (corner + extents), with the region's name from FIG 1/3 where it is sent. Few networks send them.
    struct Region { int id = 0; bool global = false; std::vector<std::pair<int, int>> tii; bool hasBox = false; double lat = 0, lng = 0, extLat = 0, extLng = 0; std::string label; };
    std::vector<Region> regions;
    const Region* regionOf(int mainId, int subId) const {   // the region whose TII list names this transmitter, else null
        for (const auto& g : regions) for (const auto& t : g.tii) if (t.first == mainId && t.second == subId) return &g;
        return nullptr;
    }
};
DabFicInfo dabFicInfo(const DabFicAnalysis& a);
const char* dabLanguageName(int code);
const char* dabPtyName(int table, int pty);
const char* dabAnnouncementName(int bit);
std::string dabUserAppName(int type);

DabFigContext dabFigContext(const DabFicAnalysis& a);
DabFigDecoded dabFigDecode(const DabFigRecord& r, const DabFigContext& ctx);
const char* dabFigName(int type, int ext);           // "Ensemble information", "Programme service label" ...
std::string dabFigHeader(const DabFigRecord& r);     // "0/2 C/N 0 OE 0 P/D 1"
// The whole analysis as text, FIG by FIG, in the layout of etisnoop
std::string dabFicText(const DabFicAnalysis& a);

} // namespace dect2
