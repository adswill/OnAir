// Layers above the demodulator of the X-band links, in both directions so the test signal is built with the code that checks it: the CADU (sync word, randomiser,
// Reed-Solomon with interleave and shortening), the transfer frame header (AOS or TM), M_PDU to space packet reassembly, and the channel store of the instruments.
//
// Layouts from SatDump's pipelines and plugins (GPL-3.0, Copyright Aang23 and contributors), written for OnAir; see THIRD_PARTY.md.
#pragma once
#include "dect2/xband/xband_tel.h"
#include "dect2/ccsds/ccsds_tm.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::xband {

constexpr uint32_t kAsm = 0x1ACFFC1Du;

// The CADU of a link (XbLink): decode() takes a frame starting at its sync word, encode() builds one around frame bytes starting at offset 4.
class Cadu {
public:
    explicit Cadu(const XbLink& l);
    // In place: derandomise, Reed-Solomon. Returns the symbols corrected, -1 when a word could not be corrected.
    int decode(uint8_t* cadu) const;
    // cadu[4..] holds the frame, the parity bytes are filled in: Reed-Solomon, randomise, sync word.
    void encode(uint8_t* cadu) const;
    int dataBytes() const { return data_; }      // bytes of frame data after the sync word (no parity)
    int caduBytes() const { return L_.caduBytes; }
private:
    const XbLink& L_;
    int data_ = 0, n_ = 255;
    std::vector<uint8_t> pn_;
    std::unique_ptr<ccsds::Rs> rs_;
};

struct Header {
    int scid = 0, vcid = 0;
    uint32_t counter = 0;
    int fhp = 0x7FF;
    const uint8_t* data = nullptr;
};
// cadu starts at the sync word
void parseHeader(const XbLink& l, const uint8_t* cadu, Header& h);

struct Packet {
    int apid = 0, seqFlag = 3, seqCount = 0;
    std::vector<uint8_t> data;       // the packet data field (the 6-byte primary header is not in it)
};

// Packets of one virtual channel across frames; a gap in the frame counter drops the packet being built.
class Demux {
public:
    std::function<void(const Packet&)> onPacket;
    void reset() { buf_.clear(); have_ = false; last_ = -1; }
    void push(const Header& h, int mpduData, bool counted);
    uint64_t packets = 0, dropped = 0;
private:
    void take(const uint8_t* p, size_t n);
    std::vector<uint8_t> buf_;
    bool have_ = false;
    int64_t last_ = -1;
};

// Packs packets into frames (the test signal); the frame is written from offset 4 of `cadu`.
class Mux {
public:
    Mux(const XbLink& l, int scid, int vcid) : L_(l), scid_(scid), vcid_(vcid) {}
    void add(int apid, int seqFlag, int seqCount, const std::vector<uint8_t>& data);
    bool next(uint8_t* cadu, const Cadu& c);          // false: less than a frame is waiting
    size_t pending() const { return q_.size(); }
private:
    const XbLink& L_;
    int scid_, vcid_;
    uint32_t counter_ = 0;
    std::vector<uint8_t> q_;
    std::vector<size_t> starts_;
};

void unpack12(const uint8_t* in, int words, uint16_t* out);
void pack12(const uint16_t* in, int words, uint8_t* out);

// 16-bit picture of one channel; grows with the pass
struct Plane {
    std::string name;
    double um = 0;
    int width = 0, lines = 0;
    std::vector<uint16_t> px;
    void setRow(int y, const uint16_t* row);
    void setPixel(int y, int x, uint16_t v);
    void ensure(int y);
};

// The channels and how they are published: the receiver thread writes, the screen and "save" read under the mutex.
class Store {
public:
    int add(const std::string& name, double um, int width);    // returns the index
    Plane& plane(int i) { return p_[(size_t)i]; }
    const Plane& plane(int i) const { return p_[(size_t)i]; }
    int count() const { return (int)p_.size(); }
    void clear();
    std::vector<std::shared_ptr<const XbPreview>> previews(int maxWidth);   // only when something changed; shared pointers are kept
    mutable std::mutex mu;
private:
    std::vector<Plane> p_;
    std::vector<std::shared_ptr<const XbPreview>> pub_;
    std::vector<int> pubLines_;
    uint64_t id_ = 0;
};

// One CADU of a link -> frames, packets, instrument channels.
class Instruments {
public:
    Instruments(int profile, Store& s);
    ~Instruments();
    void frame(const uint8_t* cadu);        // after Cadu::decode
    uint64_t packets() const;
    uint64_t lines() const;
    const std::string& satName() const;
    int lastScid() const;
    const struct AvhrrCal* avhrrCal() const; // AVHRR on-board calibration data of MetOp, else null
    void finish();                          // the end of a pass: what is still buffered goes to the channels
    void rawFrame(const uint8_t* f);        // links without CADUs (DMSP RTD frames)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::xband
