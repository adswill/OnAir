// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM data services (ETSI ES 201 980 clauses 6.4.3.6 and 6.6): the application information of the SDC names the application of each data service;
// the packet mode applications that are decoded here carry MOT objects (ETSI EN 301 234) in MSC data groups: the MOT slide show (user application 0x002,
// TS 101 499), the MOT broadcast website (0x003, TS 101 498), and EPG (0x007, TS 102 371) which is listed; TPEG (0x004, TS 102 818) is identified and
// counted. Journaline (0x44A) has its own decoder (dab_journaline.h). Nothing that is received is ever run or opened: web pages are shown as text.
#pragma once
#include "dect2/dab/dab_mot.h"
#include "dect2/dab/dab_journaline.h"
#include "drm_msg.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {

// The name of an application: domain 0 (DAB specified) with its 11 bit user application type, TS 101 756 table 16
const char* drmAppName(int domain, int userApp);

struct DrmSlide {
    uint64_t seq = 0;                  // counts the pictures received; 0 = none yet
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;         // w * h pixels, R G B A
    std::string name, caption, category, clickUrl, altUrl, trigger, type;   // trigger: the trigger time as text ("now", date and time)
    int categoryId = -1, slideId = -1;
    size_t bytes = 0;                  // size of the picture file
};

struct DrmObjectInfo {
    uint16_t tid = 0;
    std::string name, type;            // the MOT content name (or the transport id) and the content type ("text/HTML", "image/JPEG" ...)
    size_t size = 0;
    bool text = false;                 // `content` holds the text (HTML and plain text are shown as text, never rendered or run)
    std::string content;               // the first 16 kbyte of a text object, control characters replaced
    uint64_t received = 0;             // how many times the object came round
    uint64_t order = 0;                // counts new objects, for "latest"
    std::string note;                  // the slide show / directory parameters, EPG: "XML in the clear"
};

// What the UI shows of one data service
struct DrmDataView {
    int shortId = 0, domain = 0, userApp = -1;
    std::string appName;               // "MOT slide show" ...
    bool packet = false, dataUnit = false;
    int streamId = 0, packetId = 0, packetLen = 0;
    bool decoded = false;              // this receiver decodes the application (a packet mode MOT application, TPEG identified, Journaline)
    bool journaline = false;
    std::string modeText;              // "packet mode, data units, packet id 0, 45 byte packets" / "stream mode data, not decoded"
    uint64_t seq = 0;                  // grows with every change
    // packet layer (ES 201 980 clause 6.6.1)
    uint64_t packets = 0, badPackets = 0, lostPackets = 0, orphanPackets = 0, emptyPackets = 0, otherIdPackets = 0, unitsOk = 0, unitsDropped = 0;
    uint64_t unitBytes = 0;
    MotStats mot;
    DrmSlide slide;
    std::vector<DrmObjectInfo> objects;   // by order of arrival, at most 96
    uint64_t tpegFrames = 0, tpegBytes = 0;
    std::vector<MotObject> directory;    // objects announced by the last MOT directory (without bodies)
};

// One data service of the multiplex. Not thread safe: the receiver guards it with a mutex.
class DrmDataService {
public:
    DrmDataService();
    DrmDataService(const DrmDataService&) = delete;
    DrmDataService& operator=(const DrmDataService&) = delete;
    // The application information (SDC type 5) of the service; a change of any part starts over
    void configure(const drm::SdcApp& a);
    // A whole logical frame of the service's stream (part A then part B)
    void feed(const uint8_t* d, size_t n);
    // One data unit (an MSC data group) of the application: used by feed() and by tests
    void unit(const uint8_t* g, size_t n);
    void fill(DrmDataView& v) const;
    // The pictures that came since the last call, oldest first (at most 16 are kept): the view only holds the latest one, and several can arrive within one poll
    bool takeSlide(DrmSlide& out);
    int userApp() const { return view_.userApp; }

private:
    void motObject(const MotObject& m);
    DrmDataView view_;
    DrmPacketDemux demux_;
    MotDecoder mot_;
    int key_ = -1;
    uint64_t nextOrder_ = 1;
    uint64_t slideHash_ = 0;
    std::deque<DrmSlide> fresh_;
};

} // namespace dect2
