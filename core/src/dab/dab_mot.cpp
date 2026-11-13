// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MOT object reassembly (ETSI EN 301 234), see dab_mot.h. Shared by the DAB PAD / packet mode decoders and the DRM data services.
#include "dect2/dab/dab_mot.h"
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_proto.h"
#include "dect2/common/diag.h"
#include <mutex>
#include <set>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {
MotTime pub(const dabp::MotTime& c) {
    MotTime t;
    t.valid = c.valid; t.now = c.now; t.longForm = c.longForm; t.mjd = c.mjd; t.hour = c.hour; t.minute = c.minute; t.second = c.second; t.ms = c.ms;
    return t;
}
dabp::MotTime priv(const MotTime& t) {
    dabp::MotTime c;
    c.valid = t.valid; c.now = t.now; c.longForm = t.longForm; c.mjd = t.mjd; c.hour = t.hour; c.minute = t.minute; c.second = t.second; c.ms = t.ms;
    return c;
}
} // namespace

// The parsing of MOT times, headers, data groups and directories is dab_proto (Rust); this file keeps the objects and the reassembly.
size_t motParseTime(const uint8_t* d, size_t n, MotTime& t) {
    dabp::MotTime c;
    const size_t used = dabp::motParseTime(d, n, c);
    t = pub(c);
    return used;
}

std::string motTimeText(const MotTime& t) { return dabp::motTimeText(priv(t)); }

bool motParseHeader(const uint8_t* h, size_t n, MotObject& o) {
    dabp::MotHeader hd;
    if (!dabp::motParseHeader(h, n, hd)) { DIAG_DEBUG(442878, "A MOT header of %zu bytes did not parse, so the object it describes is not built.", n); return false; }
    o.bodySize = (size_t)hd.bodySize;
    o.headerSize = (size_t)hd.headerSize;
    o.contentType = hd.contentType;
    o.subtype = hd.subtype;
    o.params.clear();
    for (const dabp::MotParamRef& p : hd.params) { MotParam pr; pr.id = p.id; pr.data.assign(h + p.data.off, h + p.data.off + p.data.len); o.params.push_back(pr); }
    o.name.clear(); o.caption.clear(); o.category.clear(); o.clickUrl.clear(); o.altUrl.clear();
    o.categoryId = o.slideId = -1;
    o.hasTrigger = o.hasExpire = false;
    if (hd.hasName) o.name.assign((const char*)h + hd.name.off, hd.name.len);       // ContentName: the bytes after the character set byte, as sent
    o.caption = hd.caption;                                                          // ContentDescription, UTF-8
    if (hd.hasSlide) { o.categoryId = hd.categoryId; o.slideId = hd.slideId; }       // CategoryID / SlideID
    o.category.assign((const char*)h + hd.category.off, hd.category.len);            // CategoryTitle
    o.clickUrl.assign((const char*)h + hd.clickUrl.off, hd.clickUrl.len);            // ClickThroughURL
    o.altUrl.assign((const char*)h + hd.altUrl.off, hd.altUrl.len);                  // AlternativeLocationURL
    if (hd.hasTrigger) { o.hasTrigger = true; o.trigger = pub(hd.trigger); }
    if (hd.hasExpire) { o.hasExpire = true; o.expire = pub(hd.expire); }
    return true;
}

const char* motContentTypeName(int type, int sub) {
    // the names are made by dab_proto; hand out a stable pointer for each distinct one
    static std::mutex mu;
    static std::set<std::string> names;
    const std::string s = dabp::motContentTypeName(type, sub);
    std::lock_guard<std::mutex> lk(mu);
    return names.insert(s).first->c_str();
}

bool motIsText(const MotObject& o) { return o.contentType == 1; }

void MotDecoder::reset() {
    objs_.clear(); order_.clear(); dir_ = Dir(); dirList_.clear(); dirEntries_.clear();
}

MotDecoder::Obj& MotDecoder::obj(uint16_t tid) {
    auto it = objs_.find(tid);
    if (it == objs_.end()) {
        if (order_.size() >= maxObjects_) {
            DIAG_DEBUG_EVERY(972389, 60.0, "The MOT decoder holds %zu objects in reassembly, its limit, so the oldest one (transport id %u) is dropped.", order_.size(), (unsigned)order_.front());
            objs_.erase(order_.front()); order_.pop_front();
        }
        it = objs_.emplace(tid, Obj()).first;
        order_.push_back(tid);
    }
    return it->second;
}

void MotDecoder::dataGroup(const uint8_t* d, int n) {
    const dabp::MotGroup g = dabp::motParseGroup(d, n < 0 ? 0 : (size_t)n);   // header, CRC, session header (transport id), segmentation header
    if (g.status == dabp::MotGroup::Ignored) return;
    stats_.groups++;
    if (g.status == dabp::MotGroup::Bad) { stats_.groupsBad++; return; }
    if (g.status == dabp::MotGroup::Scrambled) { stats_.scrambled++; return; }    // a scrambled body needs conditional access
    if (g.status == dabp::MotGroup::Compressed) { stats_.compressed++; return; }  // a compressed directory (zlib) is not decoded
    segment((uint16_t)g.tid, g.type, g.segNo, g.last, d + g.off, (int)g.len);
}

void MotDecoder::segment(uint16_t tid, int type, int segNo, bool last, const uint8_t* d, int n) {
    if (n < 0 || segNo > 4095) { DIAG_DEBUG_EVERY(794779, 60.0, "A MOT segment numbered %d (of %d bytes) is beyond the 4095 allowed, so it is dropped.", segNo, n); return; }
    if (type == 6) {                                           // directory mode: the directory is an object of its own
        stats_.directorySegments++;
        if (dir_.tid != tid || (dir_.have && segNo == 0 && !dir_.raw.empty() && dir_.parts.count(0) && dir_.parts[0] != std::vector<uint8_t>(d, d + n)))
            dir_ = Dir();
        if (dir_.have) return;                                 // this version is complete already
        dir_.tid = tid;
        dir_.parts[segNo].assign(d, d + n);
        if (last) dir_.last = segNo;
        if (dir_.last >= 0 && dir_.parts.rbegin()->first > dir_.last) {
            DIAG_DEBUG(153134, "MOT directory %u has a segment numbered after its last one (%d), so its reassembly starts over.", (unsigned)tid, dir_.last);
            dir_ = Dir();
            return;
        }
        if (dir_.last < 0 || (int)dir_.parts.size() != dir_.last + 1) return;
        std::vector<uint8_t> all;
        for (auto& kv : dir_.parts) all.insert(all.end(), kv.second.begin(), kv.second.end());
        if (all.size() > (4u << 20)) { DIAG_DEBUG(117763, "MOT directory %u is %zu bytes, over the 4 MB limit, so it is dropped.", (unsigned)tid, all.size()); dir_ = Dir(); return; }
        dir_.have = true; dir_.raw = all;
        directoryDone(all);
        return;
    }
    Obj& o = obj(tid);
    auto& parts = type == 3 ? o.head : o.body;
    int& lastNo = type == 3 ? o.headLast : o.bodyLast;
    if (type == 3) stats_.headerSegments++; else stats_.bodySegments++;
    if (type == 3 && segNo == 0) {                              // a different header under the same transport id: a new object
        auto h0 = o.head.find(0);
        if (h0 != o.head.end() && (h0->second.size() != (size_t)n || std::memcmp(h0->second.data(), d, (size_t)n) != 0)) o = Obj();
    }
    if (type == 4 && !o.haveDir) { auto de = dirEntries_.find(tid); if (de != dirEntries_.end()) { o.haveDir = true; o.dirRaw = de->second; } }
    if (o.done) { stats_.repeats++; return; }
    auto& slot = parts[segNo];
    if (!slot.empty()) stats_.repeats++;
    slot.assign(d, d + n);
    if (last) lastNo = segNo;
    if (lastNo >= 0 && parts.rbegin()->first > lastNo) {
        DIAG_DEBUG_EVERY(444147, 60.0, "MOT object %u has a %s segment numbered after its last one (%d), so its reassembly starts over.", (unsigned)tid, type == 3 ? "header" : "body", lastNo);
        parts.clear(); lastNo = -1; return;
    }
    tryComplete(o, tid);
}

void MotDecoder::tryComplete(Obj& o, uint16_t tid) {
    if (o.bodyLast < 0 || (int)o.body.size() != o.bodyLast + 1) return;
    std::vector<uint8_t> h;
    if (o.haveDir) h = o.dirRaw;                                // directory mode: the header comes from the directory
    else {
        if (o.headLast < 0 || (int)o.head.size() != o.headLast + 1) return;
        for (auto& kv : o.head) h.insert(h.end(), kv.second.begin(), kv.second.end());
    }
    std::vector<uint8_t> b;
    for (auto& kv : o.body) b.insert(b.end(), kv.second.begin(), kv.second.end());
    if (h.size() < 7 || b.size() > maxBytes_) {
        DIAG_DEBUG(251995, "MOT object %u has a %zu byte header and a %zu byte body (limit %zu), so it is not built.", (unsigned)tid, h.size(), b.size(), maxBytes_);
        return;
    }
    o.done = true;
    o.body.clear();                                             // the header segments stay: a different header under the same transport id is a new object
    MotObject m;
    if (!motParseHeader(h.data(), h.size(), m)) return;
    m.tid = tid; m.fromDirectory = o.haveDir;
    if (m.bodySize && m.bodySize != b.size()) {   // the body is not what the header says
        DIAG_DEBUG(994622, "MOT object %u has a %zu byte body but its header says %zu bytes, so it is dropped.", (unsigned)tid, b.size(), m.bodySize);
        return;
    }
    m.body = std::move(b);
    stats_.objects++;
    if (sink_) sink_(m);
}

// The directory object (EN 301 234 clause 7.1), walked by dab_proto: the transport id and header of each object it announces.
void MotDecoder::directoryDone(const std::vector<uint8_t>& dir) {
    stats_.directories++;
    dirList_.clear();
    std::vector<dabp::DirEntry> ents;
    if (!dabp::motParseDirectory(dir.data(), dir.size(), ents)) {
        DIAG_DEBUG(207604, "A MOT directory of %zu bytes did not parse, so the objects it announces wait for the next directory.", dir.size());
        return;
    }

    std::map<uint16_t, std::vector<uint8_t>> entries;
    for (const dabp::DirEntry& e : ents) {
        const uint16_t tid = (uint16_t)e.tid;
        std::vector<uint8_t> raw(dir.begin() + e.off, dir.begin() + e.off + e.size);
        MotObject m;
        if (motParseHeader(raw.data(), raw.size(), m)) { m.tid = tid; m.fromDirectory = true; dirList_.push_back(m); }
        entries[tid] = raw;
    }
    // a changed header under a known transport id starts that object over; bodies that arrived before the directory complete now
    dirEntries_ = entries;
    for (auto it = objs_.begin(); it != objs_.end(); ++it) {
        auto e = entries.find(it->first);
        if (e == entries.end()) continue;
        Obj& o = it->second;
        if (o.haveDir && o.dirRaw != e->second) o = Obj();
        else if (!o.haveDir && o.done) o = Obj();
        o.haveDir = true; o.dirRaw = e->second;
    }
    for (auto& e : entries) { auto it = objs_.find(e.first); if (it != objs_.end()) tryComplete(it->second, e.first); }
}

} // namespace dect2
