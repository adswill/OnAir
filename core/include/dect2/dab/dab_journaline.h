// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Journaline (ETSI TS 102 979 V1.1.1): the text / menu "news and information" data service of DAB (user application 0x44a) and DRM.
// Written from the public specification only. Layers: MSC data group (EN 300 401 clause 5.3.3, restricted as in TS 102 979 clause 8.1.1),
// JML objects (header, deflate, JML codes, escape sequences, general link targets) and TOC management blocks. The DAB packet mode demultiplexer
// (EN 300 401 clause 5.3.2) hands the data groups of one packet address to the decoder; so does the X-PAD data group decoder and the DRM packet mode.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct JlLink { uint16_t target = 0; std::string text; };            // a menu link item
struct JlGeneralLink { int type = 0; uint16_t target = 0; std::string address, label; };  // data section 0x03 (type 0: object ID in `target`)

struct JlObject {
    uint16_t id = 0;
    int type = 0;                 // 1 menu, 2 plain text, 3 title only, 4 list
    bool isStatic = false;
    bool compressed = false;
    int revision = 0;             // 0..7
    std::string title;            // UTF-8, escape sequences resolved
    std::vector<JlLink> links;    // menu
    std::string body;             // plain text message (line breaks as '\n')
    std::vector<std::string> items;   // list message: one string per item, columns separated by '\t'
    std::vector<JlGeneralLink> general;
    std::string language;         // default language (DS 0xA0), "" = none
    size_t size = 0;              // bytes of the received object (header + content, as sent)
    uint64_t seq = 0;             // the update counter of the service when this object arrived
};

struct JlTocEntry { uint16_t id = 0; uint8_t desc = 0; };
struct JlToc {
    bool have = false;
    int revision = 0, blockCount = 0, objectCount = 0, timeoutMin = 0;
    std::map<int, bool> blocks;            // block indices seen of the current revision
    std::map<uint16_t, uint8_t> entries;   // object ID -> object description of the current TOC revision
};

struct JlStats {
    uint64_t groups = 0, badCrc = 0, rejected = 0;   // data groups accepted, with a wrong CRC, discarded for other reasons
    uint64_t objects = 0, updates = 0, badObjects = 0, tocBlocks = 0;
    int cached = 0;               // objects in the cache now
    int expected = 0;             // objects the TOC says the service has (0 = no TOC yet)
    int expectedHave = 0;         // of those, how many are in the cache
};

// Checks one MSC data group as Journaline restricts it (2 byte header, CRC, no segmentation or session header, field <= 4092 bytes).
// type: 0 JML object, 6 management data. field points into d.
bool jlDataGroup(const uint8_t* d, size_t n, int& type, const uint8_t*& field, size_t& fieldLen, bool* crcBad = nullptr);
// Parses one JML object (header section + content section). extHeader = "length of extended header" of the service signalling.
bool jlParseObject(const uint8_t* d, size_t n, int extHeader, JlObject& out);
// Parses one TOC management block (type byte 0x54 first).
bool jlParseToc(const uint8_t* d, size_t n, int& revision, int& blockCount, int& blockIndex, int& objectCount, int& timeoutMin, std::vector<JlTocEntry>& entries);
// User application data of FIG 0/13 (clause 8.1.2): version, extended header length, point of entry (-1 = default root 0x0000)
bool jlUserAppData(const uint8_t* d, size_t n, int& version, int& extHeader, int& entry);

class JournalineDecoder {
public:
    void reset();
    void setExtHeader(int n) { std::lock_guard<std::mutex> lk(mu_); ext_ = n; }
    // One complete MSC data group, CRC included.
    void feedDataGroup(const uint8_t* d, size_t n);
    bool get(uint16_t id, JlObject& out) const;
    JlStats stats() const;
    uint64_t seq() const { std::lock_guard<std::mutex> lk(mu_); return seq_; }   // changes whenever the cache does
    std::vector<uint16_t> ids() const;
    std::vector<JlObject> all() const;
private:
    mutable std::mutex mu_;
    int ext_ = 0;
    uint64_t seq_ = 0;
    std::map<uint16_t, JlObject> objs_;
    JlToc toc_;
    JlStats st_;
};

// DAB packet mode (EN 300 401 clause 5.3.2): the packets of one packet address -> MSC data groups. Feed it the logical frames of the sub-channel.
// Packets of other addresses, padding, command packets and the FEC packets (address 1022) are skipped; the Reed-Solomon FEC is not applied
// (every packet has its own CRC, so error free packets are used as they are).
class DabPacketDemux {
public:
    explicit DabPacketDemux(int address = -1) : addr_(address) {}
    void setAddress(int a) { addr_ = a; reset(); }
    void setMaxGroup(size_t n) { maxLen_ = n; }   // longest data group taken (default: the Journaline limit; MOT data groups of the SPI go up to about 8200 bytes)
    void reset() { buf_.clear(); asm_.clear(); on_ = false; lastCi_ = -1; }
    // Calls onGroup(const uint8_t*, size_t) for every complete data group (the bytes as carried: header, field, CRC).
    template <class F> void feed(const uint8_t* d, size_t n, F onGroup) {
        buf_.insert(buf_.end(), d, d + n);
        size_t pos = 0;
        std::vector<uint8_t> grp;
        while (buf_.size() - pos >= 3) {
            const size_t len = 24 * ((size_t)(buf_[pos] >> 6) + 1);
            if (buf_.size() - pos < len) break;
            if (packet(&buf_[pos], len, grp)) { pos += len; if (!grp.empty()) { onGroup(grp.data(), grp.size()); grp.clear(); } }
            else pos += 24;   // out of step: look at the next 24 byte boundary
        }
        buf_.erase(buf_.begin(), buf_.begin() + (long)pos);
        if (buf_.size() > 4096) buf_.clear();
    }
    uint64_t packets = 0, badPackets = 0;   // packets with a good / a wrong CRC-16
    uint64_t lostPackets = 0;               // packets missing according to the continuity index (not the ones with a wrong CRC: those are in badPackets)
    uint64_t orphanPackets = 0;             // continuation packets without the first packet of their data unit
    uint64_t otherIdPackets = 0;            // good packets of another Packet Id (other applications of the stream)
    uint64_t emptyPackets = 0;              // packets without data (padding)
    uint64_t unitsOk = 0, unitsDropped = 0; // data units delivered / started and abandoned (lost packet, bad CRC, too long)
private:
    bool packet(const uint8_t* p, size_t len, std::vector<uint8_t>& out);
    int addr_;
    size_t maxLen_ = 0;
    std::vector<uint8_t> buf_, asm_;
    bool on_ = false;
    int lastCi_ = -1;
};

// DRM packet mode (ES 201 980 clause 6.6): packets of `dataLen` + 3 bytes (8 bit header, data field, CRC-16) of one Packet Id; the data units
// (first to last packet) are MSC data groups for Journaline (TS 102 979 clause 8.2) and for MOT (TS 101 499 slide show, broadcast website).
// Feed it whole logical frames of the stream. A packet with a wrong CRC-16 is dropped; the 3 bit continuity index of the packets of one Packet Id
// shows packets that were lost (the data unit they belong to is dropped); a continuation packet without a first packet is an orphan.
class DrmPacketDemux {
public:
    void configure(int dataLen, int packetId);
    void reset() { asm_.clear(); on_ = false; lastCi_ = -1; }
    template <class F> void feed(const uint8_t* d, size_t n, F onUnit) {
        std::vector<uint8_t> out;
        const size_t sz = (size_t)len_ + 3;
        if (len_ <= 0) return;
        for (size_t p = 0; p + sz <= n; p += sz) if (packet(d + p, sz, out)) { onUnit(out.data(), out.size()); out.clear(); }
    }
    uint64_t packets = 0, badPackets = 0;   // packets with a good / a wrong CRC-16
    uint64_t lostPackets = 0;               // packets missing according to the continuity index (not the ones with a wrong CRC: those are in badPackets)
    uint64_t orphanPackets = 0;             // continuation packets without the first packet of their data unit
    uint64_t otherIdPackets = 0;            // good packets of another Packet Id (other applications of the stream)
    uint64_t emptyPackets = 0;              // packets without data (padding)
    uint64_t unitsOk = 0, unitsDropped = 0; // data units delivered / started and abandoned (lost packet, bad CRC, too long)
private:
    bool packet(const uint8_t* p, size_t sz, std::vector<uint8_t>& out);   // true: `out` holds a complete data unit
    int len_ = 0, id_ = 0;
    std::vector<uint8_t> asm_;
    bool on_ = false;
    int lastCi_ = -1;
};

} // namespace dect2
