// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB+ programme associated data: the PAD carried in the data stream element of every access unit (ETSI TS 102 563 clause 5.2, EN 300 401
// clause 7.4), the MSC data groups sent in its X-PAD (EN 300 401 clause 5.3.3, X-PAD application types 1, 12 and 13) and the MOT objects in those
// (ETSI EN 301 234, header mode). A MOT object whose content type is an image is the station's slide show picture.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "dab_mot.h"
#include "dab_tel.h"

namespace dect2 {

struct DabSlide {
    uint64_t seq = 0;                // counts the pictures received; 0 = none yet
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;       // w * h pixels, R G B A
    std::string name;                // the MOT ContentName, when sent
};

// A station logo: from the service information (SPI, ETSI TS 102 818) media description, or a slide show picture in the category "logo".
struct DabLogo {
    uint32_t sid = 0;                // the service it belongs to; 0 = the service whose PAD carried it (category "logo")
    std::string type;                // the SPI multimedia type ("logo_colour_square" ...), empty for a slide show logo
    std::string name;                // the MOT ContentName of the picture
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;       // w * h pixels, R G B A
};

class DabPadDecoder {
public:
    DabPadDecoder() : mot_(6, 1u << 20) { mot_.setSink([this](const MotObject& o) { motObject(o); }); }
    DabPadDecoder(const DabPadDecoder&) = delete;
    DabPadDecoder& operator=(const DabPadDecoder&) = delete;
    void reset();
    void setDataGroupSink(std::function<void(const uint8_t*, size_t)> cb) { sink_ = std::move(cb); }   // survives reset()
    // The MOT objects that are not pictures (SPI programme guide objects sent in the PAD), as they complete; survives reset()
    void setMotSink(std::function<void(const MotObject&)> cb) { motSink_ = std::move(cb); }
    // One DAB+ access unit (raw AAC data block, CRC removed). Looks for the data stream element that carries the PAD.
    // The PAD bytes in the element are in the order of EN 300 401 figure 29 (as at the end of an MP2 frame): the two F-PAD bytes last, the X-PAD
    // before them with the byte sent first right next to the F-PAD.
    void accessUnit(const uint8_t* au, int n);
    // The PAD of one audio frame: xpad = the X-PAD bytes in the order they were sent, fpad = the last two bytes of the PAD.
    void pad(const uint8_t* xpad, int xlen, const uint8_t fpad[2]);
    // One DAB (MPEG-1/2 layer II) audio frame: its last bytes are the PAD, the two F-PAD bytes last, the X-PAD in front of them. The length of a variable
    // size X-PAD without content indicators is not signalled: it is taken as the length of the last data subfield.
    void mp2Frame(const uint8_t* frame, int len);
    uint64_t seq() const { return slide_.seq; }
    const std::string& label() const { return label_; }   // the dynamic label (X-PAD application types 2 and 3), UTF-8
    const DabSlide& slide() const { return slide_; }
    // DL Plus (TS 102 980): the tagged parts of the current label, [] when the station sends none for it
    const std::vector<DabDlpTag>& dlPlus() const { return dlp_; }
    bool dlPlusRunning() const { return dlpRunning_; }
    // The logos known so far (SPI service information in a MOT object, with the pictures it points to, and slide show pictures of the category
    // "logo"); logoSeq() counts the changes.
    uint64_t logoSeq() const { return logoSeq_; }
    const std::vector<DabLogo>& logos() const { return logos_; }

private:
    std::function<void(const uint8_t*, size_t)> sink_;
    std::function<void(const MotObject&)> motSink_;
    void subfield(int type, const uint8_t* d, int n);
    void dataGroup(const uint8_t* d, int n);
    void motObject(const MotObject& o);   // a MOT object is complete (the reassembly is MotDecoder, shared with DRM)
    void labelSegment(const uint8_t* d, int n);
    void dlPlusCommand(int toggle, const uint8_t* d, int n);
    void resolveDlPlus();
    void spi(const std::string& xml);
    void resolveLogos();
    bool isLogoName(const std::string& name) const;

    int lastCont_ = 0;                   // MP2: length of the last data subfield, for the X-PAD without indicators that follows
    int lastType_ = 0;                   // application type of the last content indicator, for X-PAD without one
    std::vector<uint8_t> group_;         // the data group being collected
    int groupLen_ = 0;                   // its length from the data group length indicator, 0 = none seen
    bool collecting_ = false;
    std::vector<uint8_t> dls_;           // the dynamic label segment being collected
    bool dlsOn_ = false;
    int dlsToggle_ = -1;                 // toggle bit of the label being assembled
    std::map<int, std::vector<uint8_t>> dlsParts_;
    int dlsLast_ = -1, dlsCharset_ = 0;
    std::string label_;
    std::vector<std::string> labelChars_;   // the characters of the label, each in UTF-8: DL Plus counts characters
    struct RawTag { int type, start, len; };
    std::vector<RawTag> dlpRaw_;
    int dlpToggle_ = -1;                 // toggle bit of the label the tags belong to
    bool dlpRunning_ = false;
    std::vector<DabDlpTag> dlp_;
    MotDecoder mot_;
    DabSlide slide_;
    struct SpiLogo { uint32_t sid; std::string type, url; int w, h; };
    std::vector<SpiLogo> spiLogos_;          // the logo entries of the service information
    std::map<std::string, DabSlide> images_; // the named pictures, to match later service information
    std::deque<std::string> imageOrder_;
    std::vector<DabLogo> logos_;
    uint64_t logoSeq_ = 0;
};

// DL Plus content type names ("Title", "Artist" ...), TS 102 980 table 2
const char* dabDlpTypeName(int type);

// Decodes JPEG, PNG, GIF or BMP (MOT content subtype 1, 3, 0, 2) to RGBA.
bool decodeSlideImage(const uint8_t* data, size_t n, int subtype, int& w, int& h, std::vector<uint8_t>& rgba);

} // namespace dect2
