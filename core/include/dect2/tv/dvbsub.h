// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB subtitles (ETSI EN 300 743): the segments of one subtitling PID (PES private stream 1, data_identifier 0x20) decoded into bitmaps -
// page composition, region composition, CLUT definition, object data (2/4/8-bit run-length pixel strings, map tables) and display
// definition - with the presentation time of every display set, so the player's clock decides when it is shown.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <deque>
#include <map>
#include <vector>

namespace dect2 {

struct DvbSubRegionImage {
    int x = 0, y = 0, w = 0, h = 0;      // position in the display (see DvbSubDisplay::dispW / dispH)
    std::vector<uint8_t> rgba;           // w * h pixels, straight (not premultiplied) alpha
};

struct DvbSubDisplay {
    double pts = 0;                      // seconds (PES PTS / 90000); -1 = the PES carried none (show at once)
    double timeout = 0;                  // page_time_out: seconds after pts at which the page goes away
    int dispW = 720, dispH = 576;        // display definition (default 720 x 576)
    uint64_t seq = 0;
    std::vector<DvbSubRegionImage> regions;   // none = an empty page (clears the screen)
};

// Distance a - b in seconds on the 33-bit PTS circle (26.5 hours), so a clock that wrapped still compares correctly
double ptsDelta(double a, double b);

// Alpha-blend the regions of a display into one dispW x dispH RGBA picture (transparent where nothing is drawn)
void dvbSubComposite(const DvbSubDisplay& d, std::vector<uint8_t>& rgba);

// The pure layer below the decoder (Rust crate rust/onair_dvbsi, dvbsub_rs.cpp): PES header, segment walk, segment parsers,
// colours and the run-length pixel decoder (ETSI EN 300 743).
struct DvbSubPesInfo { uint32_t payloadOff = 0; bool hasPts = false; double pts = -1; };   // payloadOff: the first segment (after data_identifier and stream id)
bool dvbSubPesInfo(const uint8_t* pes, size_t n, DvbSubPesInfo& out);   // false: not a DVB subtitle PES packet (private stream 1, data_identifier 0x20)
struct DvbSubSegment { int type = 0, page = 0; uint32_t off = 0, len = 0; };
std::vector<DvbSubSegment> dvbSubSegments(const uint8_t* d, size_t n);   // sync byte 0x0F, 6 byte headers; a segment past the end stops the walk
struct DvbSubPageReg { int id = 0, x = 0, y = 0; };
struct DvbSubPageInfo { int timeout = 0, version = 0, state = 0; std::vector<DvbSubPageReg> regions; };
bool dvbSubParsePage(const uint8_t* d, int n, DvbSubPageInfo& out);       // false: n < 2
struct DvbSubObjRef { int id = 0, type = 0, x = 0, y = 0, fg = 0, bg = 0; };
struct DvbSubRegionInfo { int id = 0, version = 0; bool fill = false; int w = 0, h = 0, depth = 4, clut = 0, pix8 = 0, pix4 = 0, pix2 = 0; std::vector<DvbSubObjRef> objs; };
bool dvbSubParseRegion(const uint8_t* d, int n, DvbSubRegionInfo& out);   // false: n < 10
struct DvbSubClutEntry { int id = 0, flags = 0; uint32_t rgba = 0; };     // flags 0x80 / 0x40 / 0x20: the 2-, 4-, 8-bit table; rgba = r | g << 8 | b << 16 | a << 24
bool dvbSubParseClut(const uint8_t* d, int n, int& id, std::vector<DvbSubClutEntry>& out);   // false: n < 2
bool dvbSubParseDisplay(const uint8_t* d, int n, int& w, int& h);          // false: n < 5; 720 x 576 when the stated size is out of range
struct DvbSubObjectInfo { int id = 0; uint32_t topOff = 0, topLen = 0, botOff = 0, botLen = 0; };
bool dvbSubParseObject(const uint8_t* d, int n, DvbSubObjectInfo& out);   // false: not a bitmap object, too short, or the field lengths do not fit
void dvbSubDefaultClut(uint32_t e2[4], uint32_t e4[16], uint32_t e8[256]);
uint32_t dvbSubYcrcb(int y, int cr, int cb, int t);
// One field's pixel-data sub-blocks into a region of w * h palette indices of `depth` bits at (x0, y0); with doubleLines each line is also written one row down.
void dvbSubDecodeField(const uint8_t* f, size_t n, int x0, int y0, int lineStep, bool doubleLines, int w, int h, int depth, uint8_t* pix);
void dvbSubRegionRgba(const uint8_t* pix, size_t n, int depth, const uint32_t* e2, const uint32_t* e4, const uint32_t* e8, uint8_t* out);   // 4 bytes per pixel

class DvbSubDecoder {
public:
    // Follow the subtitle service with this composition page id (and its ancillary page id) on this PID; pid -1 switches decoding off.
    void setStream(int pid, int compositionPage, int ancillaryPage);
    int pid() const;
    int compositionPage() const;
    void reset();
    void feedTs(const uint8_t* pkt188);             // cheap PID check; reassembles PES packets
    void feedPes(const uint8_t* pes, size_t n);     // a whole PES packet (exposed for tests)
    // The segments of a PES data field (starting at the 0x0F sync byte of the first segment), presented at `pts` seconds (-1 = none)
    void feedSegments(const uint8_t* data, size_t n, double pts);
    // What should be on screen when the video being shown has presentation time clockPts; null = nothing.
    // A display set stays up until its page_time_out or the next display set is due, but never shorter than kMinShowSec from the moment it was first
    // returned here: the picture clock does not tick evenly (smoothed gaps, a stalled picture), so a page and its clearing page / expiry can fall
    // between two calls and the subtitle would never be seen (#112). Call it with the picture clock, not wall time.
    static constexpr double kMinShowSec = 1.5;
    std::shared_ptr<const DvbSubDisplay> displayAt(double clockPts) const;
    uint64_t displays() const;                      // display sets decoded so far

private:
    struct Obj { int id = 0; int x = 0, y = 0; int type = 0; int fg = 0, bg = 0; };
    struct Region {
        int id = 0, w = 0, h = 0, depth = 4, clut = 0, version = -1;
        int pix8 = 0, pix4 = 0, pix2 = 0;
        bool fill = false;
        std::vector<uint8_t> pix;                   // palette indices, w * h
        std::vector<Obj> objs;
    };
    struct Clut { uint32_t e2[4], e4[16], e8[256]; };   // RGBA as 0xAABBGGRR bytes: r | g << 8 | b << 16 | a << 24
    struct PageReg { int id, x, y; };
    struct ObjData { int id; std::vector<uint8_t> top, bottom; };

    void segment(int type, int page, const uint8_t* d, int n);
    void pageComposition(const uint8_t* d, int n);
    void regionComposition(const uint8_t* d, int n);
    void clutDefinition(const uint8_t* d, int n);
    void objectData(const uint8_t* d, int n);
    void displayDefinition(const uint8_t* d, int n);
    void drawObject(Region& r, const Obj& o, const ObjData& od);
    void endOfDisplaySet(double pts);
    Clut defaultClut() const;

    mutable std::mutex mu_;
    int pid_ = -1, comp_ = 0, anc_ = 0;
    std::vector<uint8_t> pes_;
    bool inPes_ = false;
    size_t pesLen_ = 0;
    int cc_ = -1;
    // decoder state (guarded by mu_)
    int timeout_ = 0, pageVersion_ = -1, pageState_ = 0;
    bool havePage_ = false;
    std::vector<PageReg> pageRegs_;
    std::map<int, Region> regions_;
    std::map<int, Clut> cluts_;
    std::map<int, ObjData> objects_;
    int dispW_ = 720, dispH_ = 576;
    std::deque<std::shared_ptr<DvbSubDisplay>> queue_;
    uint64_t seq_ = 0;
    mutable std::shared_ptr<const DvbSubDisplay> shown_;   // what displayAt() returned last (guarded by mu_)
    mutable double shownClock_ = 0;                        // the picture clock at which it first came back
};

} // namespace dect2
