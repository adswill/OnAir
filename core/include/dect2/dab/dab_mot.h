// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MOT (Multimedia Object Transfer, ETSI EN 301 234) object reassembly, shared by DAB (X-PAD and packet mode data groups) and DRM (packet mode data units):
// MSC data groups of type 3 (header), 4 (body) and 6 (directory) -> segments -> objects, in header mode and in directory mode. The parameters of the
// header extension are parsed here, including the slide show ones of ETSI TS 101 499 (category / slide id, category title, click-through URL,
// alternative location URL, trigger time) and the content description that serves as the caption.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

// MOT time (EN 301 234 clause 6.2.4, the coding of EN 300 401 FIG 0/10): validity flag, 17 bit MJD, LSI, confidence, UTC flag, hours and minutes,
// and with the long form seconds and milliseconds. now = the validity flag is 0 (the time is "now").
struct MotTime {
    bool valid = false, now = false;
    int mjd = 0, hour = 0, minute = 0, second = 0, ms = 0;
    bool longForm = false;
};
// d: 4 bytes (short) or 6 bytes (long form). Returns the bytes used, 0 when n is too small.
size_t motParseTime(const uint8_t* d, size_t n, MotTime& t);
std::string motTimeText(const MotTime& t);   // "now", "2026-10-06 12:00" ...

struct MotParam { int id = 0; std::vector<uint8_t> data; };

struct MotObject {
    uint16_t tid = 0;
    size_t bodySize = 0;              // from the header core, 0 = not stated
    size_t headerSize = 0;
    int contentType = 0, subtype = 0;
    std::vector<MotParam> params;
    std::string name;                 // ContentName (0x0C): the bytes after the character set byte, as sent
    std::string caption;              // ContentDescription (0x17), UTF-8 (slide show caption)
    std::string category;             // CategoryTitle (0x26)
    int categoryId = -1, slideId = -1;   // CategoryID / SlideID (0x25)
    std::string clickUrl, altUrl;     // ClickThroughURL (0x27), AlternativeLocationURL (0x28)
    bool hasTrigger = false; MotTime trigger;   // TriggerTime (0x05)
    bool hasExpire = false; MotTime expire;     // ExpireTime (0x04)
    bool fromDirectory = false;
    std::vector<uint8_t> body;
};

// Header core (7 bytes) and header extension. n = all bytes of the header (the header size field is honoured). False when it is too short.
bool motParseHeader(const uint8_t* h, size_t n, MotObject& o);
const char* motContentTypeName(int type, int subtype);   // "image/JPEG", "text/HTML", "general data" ...
bool motIsText(const MotObject& o);                       // general data text (ASCII) or HTML

struct MotStats {
    uint64_t groups = 0, groupsBad = 0;       // data groups taken in and the ones dropped for a bad CRC / bad structure
    uint64_t headerSegments = 0, bodySegments = 0, directorySegments = 0, repeats = 0;
    uint64_t objects = 0;                     // objects completed
    uint64_t directories = 0;                 // complete directories
    uint64_t scrambled = 0, compressed = 0;   // groups of the types that are not decoded (scrambled body, compressed directory)
};

class MotDecoder {
public:
    // maxObjects: objects under assembly at the same time (the oldest is dropped); maxBytes: the largest body that is kept
    explicit MotDecoder(size_t maxObjects = 6, size_t maxBytes = 1u << 20) : maxObjects_(maxObjects), maxBytes_(maxBytes) {}
    void setSink(std::function<void(const MotObject&)> cb) { sink_ = std::move(cb); }   // survives reset()
    void reset();
    // One MSC data group (EN 300 401 clause 5.3.3.1): header, session header (transport id), segmentation header and segment
    void dataGroup(const uint8_t* d, int n);
    const MotStats& stats() const { return stats_; }
    // The directory last received (directory mode): the objects it announces, in order
    const std::vector<MotObject>& directory() const { return dirList_; }

private:
    struct Obj {
        std::map<int, std::vector<uint8_t>> head, body;
        int headLast = -1, bodyLast = -1;
        bool done = false;
        bool haveDir = false;
        MotObject dir;                   // header from the directory
        std::vector<uint8_t> dirRaw;     // its bytes, to see a change
    };
    Obj& obj(uint16_t tid);
    void segment(uint16_t tid, int type, int segNo, bool last, const uint8_t* d, int n);
    void tryComplete(Obj& o, uint16_t tid);
    void directoryDone(const std::vector<uint8_t>& dir);
    std::function<void(const MotObject&)> sink_;
    size_t maxObjects_, maxBytes_;
    std::map<uint16_t, Obj> objs_;
    std::deque<uint16_t> order_;
    struct Dir { std::map<int, std::vector<uint8_t>> parts; int last = -1; uint16_t tid = 0; bool have = false; std::vector<uint8_t> raw; };
    Dir dir_;
    std::map<uint16_t, std::vector<uint8_t>> dirEntries_;   // transport id -> header bytes, from the last directory
    std::vector<MotObject> dirList_;
    MotStats stats_;
};

} // namespace dect2
