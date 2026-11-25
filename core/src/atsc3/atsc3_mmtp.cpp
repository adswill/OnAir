// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/atsc3/atsc3_mmtp.h"
#include "dect2/common/endian.h"
#include <algorithm>
#include <deque>

namespace dect2 {
namespace atsc3 {

namespace {
using dect2::endian::loadBe16;
using dect2::endian::loadBe32;
using dect2::endian::loadBe64;
using dect2::endian::putBe16;
using dect2::endian::putBe32;
using dect2::endian::putBe64;

constexpr uint32_t fourcc(const char* s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3]; }
std::string fourccText(uint32_t t) { std::string s(4, ' '); for (int i = 0; i < 4; i++) s[i] = (char)(t >> (24 - 8 * i)); return s; }

// One ISO BMFF box between `pos` and `end`. Sizes come from the air (MMTP has no checksum): a box that does not fit is the end of the walk.
struct Box { size_t pos = 0, size = 0, hdr = 0; uint32_t type = 0; };
bool nextBox(const uint8_t* d, size_t end, size_t& pos, Box& b) {
    if (pos + 8 > end) return false;
    uint64_t sz = loadBe32(d + pos);
    size_t hdr = 8;
    if (sz == 1) {
        if (pos + 16 > end) return false;
        sz = loadBe64(d + pos + 8);
        hdr = 16;
    } else if (sz == 0) sz = end - pos;
    if (sz < hdr || sz > end - pos) return false;
    b.pos = pos; b.size = (size_t)sz; b.hdr = hdr; b.type = loadBe32(d + pos + 4);
    pos += (size_t)sz;
    return true;
}
const uint8_t* body(const uint8_t* d, const Box& b) { return d + b.pos + b.hdr; }
size_t bodySize(const Box& b) { return b.size - b.hdr; }
void append(std::vector<uint8_t>& v, const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); }
void putBox(std::vector<uint8_t>& v, uint32_t type, const std::vector<uint8_t>& content) {
    putBe32(v, (uint32_t)(content.size() + 8));
    putBe32(v, type);
    v.insert(v.end(), content.begin(), content.end());
}
// serial number order of 32-bit sequence numbers
int32_t seqDiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
} // namespace

// ---------------------------------------------------------------- packets

bool parseMmtpPacket(const uint8_t* d, size_t n, MmtpPacket& p) {
    p = MmtpPacket();
    if (n < 12) return false;
    p.version = d[0] >> 6;
    p.counterFlag = (d[0] >> 5) & 1;
    p.fecType = (d[0] >> 3) & 3;
    if (p.version == 0) {
        p.extensionFlag = (d[0] >> 1) & 1;
        p.rap = d[0] & 1;
        p.type = d[1] & 0x3F;
    } else if (p.version == 1) {   // version 1 moves the extension and RAP flags one bit up (the last bit is the QoS flag); 4-bit type
        p.extensionFlag = (d[0] >> 2) & 1;
        p.rap = (d[0] >> 1) & 1;
        p.type = d[1] & 0x0F;
    } else return false;
    p.packetId = loadBe16(d + 2);
    p.timestamp = loadBe32(d + 4);
    p.sequence = loadBe32(d + 8);
    size_t pos = 12;
    if (p.counterFlag) {
        if (pos + 4 > n) return false;
        p.counter = loadBe32(d + pos);
        pos += 4;
    }
    if (p.version == 1) pos += 2;   // reliability, type of bitrate, delay sensitivity, priority, flow label
    if (p.extensionFlag) {
        if (pos + 4 > n) return false;
        const size_t len = loadBe16(d + pos + 2);
        pos += 4;
        if (len > n - pos) return false;
        pos += len;
    }
    if (pos > n) return false;
    p.payload = d + pos;
    p.payloadSize = n - pos;
    return true;
}

std::vector<uint8_t> makeMmtpPacket(const MmtpPacket& p, const std::vector<uint8_t>& payload, const std::vector<uint8_t>& ext) {
    std::vector<uint8_t> v;
    uint8_t b0 = (uint8_t)((p.version & 3) << 6 | (p.counterFlag ? 0x20 : 0) | (p.fecType & 3) << 3);
    if (p.version == 1) b0 |= (p.extensionFlag ? 4 : 0) | (p.rap ? 2 : 0);
    else b0 |= (p.extensionFlag ? 2 : 0) | (p.rap ? 1 : 0);
    v.push_back(b0);
    v.push_back((uint8_t)(p.type & (p.version == 1 ? 0x0F : 0x3F)));
    putBe16(v, p.packetId);
    putBe32(v, p.timestamp);
    putBe32(v, p.sequence);
    if (p.counterFlag) putBe32(v, p.counter);
    if (p.version == 1) putBe16(v, 0);
    if (p.extensionFlag) { putBe16(v, 0); putBe16(v, (uint32_t)ext.size()); v.insert(v.end(), ext.begin(), ext.end()); }
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

bool parseMpuPayload(const uint8_t* d, size_t n, MpuPayload& m) {
    m = MpuPayload();
    if (n < 8) return false;
    const size_t len = loadBe16(d);
    m.fragmentType = d[2] >> 4;
    m.timed = (d[2] >> 3) & 1;
    m.fragmentation = (d[2] >> 1) & 3;
    m.aggregated = d[2] & 1;
    m.fragmentCounter = d[3];
    m.mpuSequence = loadBe32(d + 4);
    // the length counts the bytes after itself; a packet may carry padding after them, never fewer
    if (len < 6 || len + 2 > n) return false;
    const uint8_t* p = d + 8;
    const size_t avail = len - 6;
    if (!m.aggregated) { m.units.emplace_back(p, avail); return true; }
    size_t pos = 0;
    while (pos + 2 <= avail) {
        const size_t du = loadBe16(p + pos);
        pos += 2;
        if (du == 0 || du > avail - pos) return false;
        m.units.emplace_back(p + pos, du);
        pos += du;
    }
    return !m.units.empty();
}

bool parseMfuHeader(const uint8_t* d, size_t n, MfuHeader& h) {
    if (n < kMfuHeaderBytes) return false;
    h.fragmentSequence = loadBe32(d);
    h.sample = loadBe32(d + 4);
    h.offset = loadBe32(d + 8);
    h.priority = d[12];
    h.dependencyCounter = d[13];
    return true;
}

// ---------------------------------------------------------------- signalling

namespace {
// PA (0), MPI (1..16) and the ATSC 3.0 messages (0x8000 and up) have a 32-bit length, the others 16 bits
bool longMessageLength(int id) { return id <= 0x10 || id >= 0x8000; }
bool parseOneMessage(const uint8_t* d, size_t n, SignallingMessage& m) {
    if (n < 5) return false;
    m.id = loadBe16(d);
    m.version = d[2];
    size_t pos = 3, len;
    if (longMessageLength(m.id)) {
        if (n < 7) return false;
        len = loadBe32(d + 3);
        pos = 7;
    } else { len = loadBe16(d + 3); pos = 5; }
    if (len > n - pos) return false;
    m.payload.assign(d + pos, d + pos + len);
    return true;
}
} // namespace

bool parseSignallingMessages(const uint8_t* d, size_t n, bool aggregated, bool longLength, std::vector<SignallingMessage>& out) {
    if (!aggregated) {
        SignallingMessage m;
        if (!parseOneMessage(d, n, m)) return false;
        out.push_back(std::move(m));
        return true;
    }
    size_t pos = 0;
    const size_t lb = longLength ? 4 : 2;
    while (pos + lb <= n) {
        const size_t len = longLength ? loadBe32(d + pos) : loadBe16(d + pos);
        pos += lb;
        if (len == 0 || len > n - pos) return false;
        SignallingMessage m;
        if (!parseOneMessage(d + pos, len, m)) return false;
        out.push_back(std::move(m));
        pos += len;
    }
    return true;
}

bool parseMpTable(const uint8_t* d, size_t n, MpTable& t) {
    t = MpTable();
    size_t pos = 0;
    auto need = [&](size_t k) { return pos <= n && k <= n - pos; };
    if (!need(5)) return false;
    t.tableId = d[0];
    t.version = d[1];
    const size_t len = loadBe16(d + 2);
    if (len > n - 4) return false;
    n = 4 + len;   // garbage after the table is not read
    if (n < 5) return false;
    t.mode = d[4] & 3;
    pos = 5;
    if (t.tableId == 0x20 || t.tableId == 0x11) {   // the complete table and subset 0 carry the package id and the table descriptors
        if (!need(1)) return false;
        const size_t pl = d[pos++];
        if (!need(pl)) return false;
        t.packageId.assign((const char*)d + pos, pl);
        pos += pl;
        if (!need(2)) return false;
        const size_t dl = loadBe16(d + pos);
        pos += 2;
        if (!need(dl)) return false;
        pos += dl;
    }
    if (!need(1)) return false;
    const int count = d[pos++];
    for (int a = 0; a < count; a++) {
        MmtpAsset as;
        // identifier_mapping: identifier_type, then for an asset id asset_id_scheme(32) and asset_id_length(32)
        if (!need(9)) return false;
        const int idType = d[pos];
        pos += 5;
        const uint32_t idLen = loadBe32(d + pos);
        pos += 4;
        if (idType != 0 || !need(idLen)) return false;
        as.id.assign((const char*)d + pos, idLen);
        pos += idLen;
        if (!need(5)) return false;
        as.type = fourccText(loadBe32(d + pos));
        pos += 4;
        const bool clock = d[pos++] & 1;
        if (clock) {
            if (!need(2)) return false;
            const bool scale = d[pos + 1] & 1;
            pos += 2;
            if (scale) { if (!need(4)) return false; pos += 4; }
        }
        if (!need(1)) return false;
        const int locs = d[pos++];
        for (int l = 0; l < locs; l++) {
            if (!need(1)) return false;
            const int lt = d[pos++];
            size_t skip;
            switch (lt) {
            case 0: if (!need(2)) return false; as.packetIds.push_back(loadBe16(d + pos)); skip = 2; break;
            case 1: skip = 12; break;    // IPv4 source, destination, port, packet id: another flow, not followed in this receiver
            case 2: skip = 36; break;    // the same over IPv6
            case 3: skip = 6; break;     // an MPEG-2 transport stream: network id, transport stream id, PID
            case 4: skip = 36; break;    // MPEG-2 transport stream over IPv6: source, destination, port, PID
            case 5: if (!need(1)) return false; skip = 1 + d[pos]; break;   // URL
            default: return false;       // no length to step over it: the rest of the table cannot be read
            }
            if (!need(skip)) return false;
            pos += skip;
        }
        if (!need(2)) return false;
        const size_t dl = loadBe16(d + pos);
        pos += 2;
        if (!need(dl)) return false;
        // asset descriptors: the MPU timestamp descriptor (tag 1) gives each MPU's presentation time
        for (size_t q = pos; q + 3 <= pos + dl;) {
            const int tag = loadBe16(d + q);
            const size_t l = d[q + 2];
            if (q + 3 + l > pos + dl) break;
            if (tag == 1)
                for (size_t k = q + 3; k + 12 <= q + 3 + l; k += 12) as.mpuTimes.emplace_back(loadBe32(d + k), loadBe64(d + k + 4));
            q += 3 + l;
        }
        pos += dl;
        t.assets.push_back(std::move(as));
    }
    return true;
}

std::string assetKind(const std::string& t) {
    if (t == "hvc1" || t == "hev1" || t == "avc1" || t == "avc3" || t == "vvc1" || t == "vvi1") return "video";
    if (t == "ac-4" || t == "mp4a" || t == "ac-3" || t == "ec-3" || t == "Opus" || t == "mhm1" || t == "mha1") return "audio";
    if (t == "stpp") return "subtitles";
    return t;
}

// ---------------------------------------------------------------- MPU -> fragmented MP4

namespace {
// minf / stbl / stsd / hvc1 or hev1 / hvcC: the NAL unit length size and the number of temporal sub-layers (ISO/IEC 14496-15 8.3.3)
void hevcConfig(const uint8_t* d, const Box& minf, int& nalLen, int& maxTid) {
    size_t q = minf.pos + minf.hdr;
    Box stbl;
    while (nextBox(d, minf.pos + minf.size, q, stbl)) {
        if (stbl.type != fourcc("stbl")) continue;
        size_t r = stbl.pos + stbl.hdr;
        Box stsd;
        while (nextBox(d, stbl.pos + stbl.size, r, stsd)) {
            if (stsd.type != fourcc("stsd") || bodySize(stsd) < 8) continue;
            size_t e = stsd.pos + stsd.hdr + 8;
            Box entry;
            if (!nextBox(d, stsd.pos + stsd.size, e, entry)) return;
            if (entry.type != fourcc("hvc1") && entry.type != fourcc("hev1")) return;
            size_t c = entry.pos + entry.hdr + 78;   // after the visual sample entry fields
            Box cfg;
            while (c < entry.pos + entry.size && nextBox(d, entry.pos + entry.size, c, cfg)) {
                if (cfg.type != fourcc("hvcC") || bodySize(cfg) < 22) continue;
                const uint8_t b = body(d, cfg)[21];
                nalLen = (b & 3) + 1;
                const int layers = (b >> 3) & 7;   // 0: not given
                maxTid = layers ? layers - 1 : -1;
                if (nalLen == 3) nalLen = 0;       // not a valid length size
                return;
            }
            return;
        }
    }
}
} // namespace

bool readMpuMetadata(const uint8_t* d, size_t n, MpuInit& out) {
    out = MpuInit();
    size_t pos = 0;
    Box b, ftyp, moov;
    bool haveFtyp = false, haveMoov = false;
    while (nextBox(d, n, pos, b)) {
        if (b.type == fourcc("ftyp") && !haveFtyp) { ftyp = b; haveFtyp = true; }
        else if (b.type == fourcc("moov") && !haveMoov) { moov = b; haveMoov = true; }
    }
    if (!haveMoov) return false;
    struct Trak { Box box; uint32_t id = 0, timescale = 0, handler = 0; int nalLen = 0, maxTid = -1; };
    std::vector<Trak> traks;
    std::vector<Box> other;
    Box mvex;
    bool haveMvex = false;
    const size_t mend = moov.pos + moov.size;
    pos = moov.pos + moov.hdr;
    while (nextBox(d, mend, pos, b)) {
        if (b.type == fourcc("trak")) {
            Trak t;
            t.box = b;
            size_t q = b.pos + b.hdr;
            Box c;
            while (nextBox(d, b.pos + b.size, q, c)) {
                if (c.type == fourcc("tkhd") && bodySize(c) >= 24) t.id = loadBe32(body(d, c) + (body(d, c)[0] == 1 ? 20 : 12));
                if (c.type != fourcc("mdia")) continue;
                size_t r = c.pos + c.hdr;
                Box e;
                while (nextBox(d, c.pos + c.size, r, e)) {
                    if (e.type == fourcc("hdlr") && bodySize(e) >= 12) t.handler = loadBe32(body(d, e) + 8);
                    if (e.type == fourcc("mdhd") && bodySize(e) >= 24) t.timescale = loadBe32(body(d, e) + (body(d, e)[0] == 1 ? 20 : 12));
                    if (e.type == fourcc("minf")) hevcConfig(d, e, t.nalLen, t.maxTid);
                }
            }
            traks.push_back(t);
        } else if (b.type == fourcc("mvex")) { mvex = b; haveMvex = true; }
        else other.push_back(b);
    }
    const Trak* media = nullptr;
    for (auto& t : traks) {
        if (!media && (t.handler == fourcc("vide") || t.handler == fourcc("soun"))) media = &t;
        if (t.handler == fourcc("hint") && !out.hintTrackId) out.hintTrackId = t.id;
    }
    if (!media) {
        out.handler = traks.empty() ? std::string() : fourccText(traks[0].handler);
        return true;
    }
    out.handler = fourccText(media->handler);
    out.trackId = media->id;
    out.timescale = media->timescale;
    out.nalLengthSize = media->nalLen;
    out.maxTemporalId = media->maxTid;
    if (!out.trackId || !out.timescale) return false;
    std::vector<uint8_t> m;
    for (auto& o : other) append(m, d + o.pos, o.size);
    append(m, d + media->box.pos, media->box.size);
    if (haveMvex) {
        size_t q = mvex.pos + mvex.hdr;
        Box c;
        while (nextBox(d, mvex.pos + mvex.size, q, c)) {
            if (c.type == fourcc("trex") && bodySize(c) >= 24) {
                const uint8_t* t = body(d, c);
                const uint32_t id = loadBe32(t + 4);
                if (id == out.hintTrackId) out.hintTrexSize = loadBe32(t + 16);
                if (id != out.trackId) continue;
                out.trexDuration = loadBe32(t + 12);
                out.trexSize = loadBe32(t + 16);
                out.trexFlags = loadBe32(t + 20);
            }
        }
        // only the media track's trex stays
        std::vector<uint8_t> keep;
        q = mvex.pos + mvex.hdr;
        while (nextBox(d, mvex.pos + mvex.size, q, c))
            if (c.type != fourcc("trex") || (bodySize(c) >= 8 && loadBe32(body(d, c) + 4) == out.trackId)) append(keep, d + c.pos, c.size);
        putBox(m, fourcc("mvex"), keep);
    }
    if (haveFtyp) append(out.init, d + ftyp.pos, ftyp.size);
    putBox(out.init, fourcc("moov"), m);
    return true;
}

bool readMpuFragment(const uint8_t* d, size_t n, const MpuInit& init, MpuFragment& out) {
    out = MpuFragment();
    size_t pos = 0;
    Box moof;
    bool found = false;
    while (nextBox(d, n, pos, moof)) if (moof.type == fourcc("moof")) { found = true; break; }
    if (!found) return false;
    bool media = false, hint = false;
    size_t q = moof.pos + moof.hdr;
    Box traf;
    while (nextBox(d, moof.pos + moof.size, q, traf)) {
        if (traf.type != fourcc("traf")) continue;
        // tfhd first: it says which track the traf is for
        uint32_t track = 0, tfFlags = 0, defDur = 0, defSize = 0, defFlags = init.trexFlags;
        size_t r = traf.pos + traf.hdr;
        Box c;
        Box tfhd;
        bool haveTfhd = false;
        while (nextBox(d, traf.pos + traf.size, r, c)) {
            if (c.type != fourcc("tfhd") || bodySize(c) < 8) continue;
            tfhd = c;
            haveTfhd = true;
            const uint8_t* t = body(d, c);
            tfFlags = loadBe32(t) & 0xFFFFFF;
            track = loadBe32(t + 4);
            size_t f = 8;
            const size_t lim = bodySize(c);
            if (tfFlags & 0x01) f += 8;
            if (tfFlags & 0x02) f += 4;
            if (tfFlags & 0x08) { if (f + 4 > lim) return false; defDur = loadBe32(t + f); f += 4; }
            if (tfFlags & 0x10) { if (f + 4 > lim) return false; defSize = loadBe32(t + f); f += 4; }
            if (tfFlags & 0x20) { if (f + 4 > lim) return false; defFlags = loadBe32(t + f); f += 4; }
            if (f > lim) return false;
            break;
        }
        if (!haveTfhd) continue;
        if (track != init.trackId) {   // the MMT hint track: its samples sit in front of the media in each MFU's first fragment
            if (track == init.hintTrackId || !hint) {
                uint32_t hs = defSize ? defSize : init.hintTrexSize;
                if (!hs) {   // per-sample sizes: they are all the same size
                    r = traf.pos + traf.hdr;
                    while (nextBox(d, traf.pos + traf.size, r, c)) {
                        if (c.type != fourcc("trun") || bodySize(c) < 8) continue;
                        const uint32_t fl = loadBe32(body(d, c)) & 0xFFFFFF;
                        size_t f = 8 + (fl & 0x01 ? 4 : 0) + (fl & 0x04 ? 4 : 0) + (fl & 0x100 ? 4 : 0);
                        if ((fl & 0x200) && loadBe32(body(d, c) + 4) > 0 && f + 4 <= bodySize(c)) hs = loadBe32(body(d, c) + f);
                    }
                }
                out.hintSize = hs;
                hint = true;
            }
            continue;
        }
        if (media) return false;   // two trafs of the media track
        media = true;
        // tfhd without an explicit base offset: the data offset then counts from the moof (default-base-is-moof)
        {
            const uint8_t* t = body(d, tfhd);
            std::vector<uint8_t> v;
            putBe32(v, (loadBe32(t) & 0xFF000000u) | ((tfFlags & ~0x01u) | 0x020000u));
            putBe32(v, track);
            const size_t skip = (tfFlags & 0x01) ? 8 : 0;
            v.insert(v.end(), t + 8 + skip, t + bodySize(tfhd));
            putBox(out.tfhd, fourcc("tfhd"), v);
        }
        bool haveTrun = false;
        r = traf.pos + traf.hdr;
        while (nextBox(d, traf.pos + traf.size, r, c)) {
            if (c.type == fourcc("tfhd") || c.type == fourcc("tfdt")) continue;
            if (c.type == fourcc("trun")) {
                if (haveTrun) {
                    DIAG_DEBUG_EVERY(561706, 10.0, "An MMTP movie fragment has more than one sample table for its media track, which this receiver does not rebuild, so the MPU is dropped.");
                    return false;
                }
                haveTrun = true;
                const uint8_t* t = body(d, c);
                const size_t lim = bodySize(c);
                if (lim < 8) return false;
                out.trunVersion = t[0];
                out.trunFlags = loadBe32(t) & 0xFFFFFF;
                const uint32_t count = loadBe32(t + 4);
                size_t f = 8;
                if (out.trunFlags & 0x01) f += 4;
                if (out.trunFlags & 0x04) { if (f + 4 > lim) return false; out.firstSampleFlags = loadBe32(t + f); f += 4; }
                const uint32_t fl = out.trunFlags;
                const size_t per = (fl & 0x100 ? 4 : 0) + (fl & 0x200 ? 4 : 0) + (fl & 0x400 ? 4 : 0) + (fl & 0x800 ? 4 : 0);
                // the bound runs before anything is sized by the count
                if (count == 0 || count > kMpuMaxSamples || f > lim || (lim - f) / (per ? per : 1) < (per ? count : 0)) {
                    DIAG_DEBUG_EVERY(447247, 10.0, "An MMTP movie fragment announces %u samples, more than its box holds or than %u, so its sample table is refused and the MPU is dropped.", count, kMpuMaxSamples);
                    return false;
                }
                out.trunEntries.assign(t + f, t + f + per * count);
                const uint32_t dDur = defDur ? defDur : init.trexDuration, dSize = defSize ? defSize : init.trexSize;
                for (uint32_t i = 0; i < count; i++) {
                    const uint8_t* e = t + f + (size_t)i * per;
                    size_t o = 0;
                    uint32_t dur = dDur, size = dSize, sf = (i == 0 && (fl & 0x04)) ? out.firstSampleFlags : defFlags;
                    int64_t cto = 0;
                    if (fl & 0x100) { dur = loadBe32(e + o); o += 4; }
                    if (fl & 0x200) { size = loadBe32(e + o); o += 4; }
                    if (fl & 0x400) { sf = loadBe32(e + o); o += 4; }
                    if (fl & 0x800) cto = out.trunVersion ? (int64_t)(int32_t)loadBe32(e + o) : (int64_t)loadBe32(e + o);
                    if (size > kMpuMaxSampleBytes) {
                        DIAG_DEBUG_EVERY(815691, 10.0, "An MMTP movie fragment announces a sample of %u bytes, more than this receiver accepts, so its sample table is refused and the MPU is dropped.", size);
                        return false;
                    }
                    out.sizes.push_back(size);
                    out.durations.push_back(dur);
                    out.compositionOffsets.push_back(cto);
                    out.flags.push_back(sf);
                    out.mediaBytes += size;
                }
                if (out.mediaBytes > kMpuMaxBytes) {
                    DIAG_DEBUG_EVERY(124932, 10.0, "An MMTP movie fragment announces %llu bytes of media, more than the %zu bytes this receiver accepts for one MPU, so the MPU is dropped.", (unsigned long long)out.mediaBytes, kMpuMaxBytes);
                    return false;
                }
                continue;
            }
            // what stays is what does not point into the mdat: sample dependencies and sample groups (encryption info would need offsets
            // fixed and is not kept; the receiver plays no protected services)
            if (c.type == fourcc("sdtp") || c.type == fourcc("sbgp") || c.type == fourcc("sgpd") || c.type == fourcc("subs")) append(out.trafOther, d + c.pos, c.size);
        }
        if (!haveTrun) return false;
    }
    return media;
}

std::vector<uint8_t> buildMediaSegment(const MpuFragment& f, uint32_t fragmentNumber, uint64_t baseTime, const std::vector<uint8_t>& media) {
    std::vector<uint8_t> mfhd, tfdt, trun, traf, moofBody, out;
    putBe32(mfhd, 0);
    putBe32(mfhd, fragmentNumber);
    putBe32(tfdt, 0x01000000u);
    putBe64(tfdt, baseTime);
    putBe32(trun, ((uint32_t)f.trunVersion << 24) | f.trunFlags | 0x01);
    putBe32(trun, (uint32_t)f.sizes.size());
    const size_t dataOffsetAt = trun.size();
    putBe32(trun, 0);
    if (f.trunFlags & 0x04) putBe32(trun, f.firstSampleFlags);
    trun.insert(trun.end(), f.trunEntries.begin(), f.trunEntries.end());
    traf = f.tfhd;
    putBox(traf, fourcc("tfdt"), tfdt);
    const size_t trunAt = traf.size() + 8;   // where the trun's content starts inside the traf box
    putBox(traf, fourcc("trun"), trun);
    traf.insert(traf.end(), f.trafOther.begin(), f.trafOther.end());
    putBox(moofBody, fourcc("mfhd"), mfhd);
    const size_t trafAt = moofBody.size() + 8;
    putBox(moofBody, fourcc("traf"), traf);
    putBox(out, fourcc("moof"), moofBody);
    const uint32_t dataOffset = (uint32_t)out.size() + 8;
    dect2::endian::storeBe32(&out[8 + trafAt + trunAt + dataOffsetAt], dataOffset);
    putBe32(out, (uint32_t)(media.size() + 8));
    putBe32(out, fourcc("mdat"));
    out.insert(out.end(), media.begin(), media.end());
    return out;
}

// ---------------------------------------------------------------- samples that did not arrive whole

int sampleDisposable(const uint8_t* d, size_t n, const MpuInit& init) {
    const size_t len = (size_t)init.nalLengthSize;
    if (!len || init.maxTemporalId < 0) return -1;
    for (size_t pos = 0; pos + len + 2 <= n;) {
        uint32_t nal = 0;
        for (size_t k = 0; k < len; k++) nal = nal << 8 | d[pos + k];
        const int type = (d[pos + len] >> 1) & 0x3F, tid = (d[pos + len + 1] & 7) - 1;
        if (nal < 2 || tid < 0) return -1;
        // the first picture NAL unit decides: a sub-layer non-reference picture (TRAIL_N, TSA_N, STSA_N, RADL_N, RASL_N, the reserved _N
        // types) in the highest sub-layer is used by no other picture (ITU-T H.265 3.137 and 7.4.2.2)
        if (type < 32) return (type <= 14 && type % 2 == 0 && tid == init.maxTemporalId) ? 1 : 0;
        if (nal > n - pos - len) return -1;   // the next NAL unit is in a part of the sample that is not here
        pos += len + nal;
    }
    return -1;
}

std::vector<char> playableSamples(const MpuFragment& f, const std::string& handler, const std::vector<char>& whole, const std::vector<char>& disposable,
                                  bool gapAfter) {
    const size_t n = f.sizes.size();
    std::vector<char> keep(n, 0);
    bool anyNonSync = false;
    for (size_t i = 0; i < n && i < f.flags.size(); i++) anyNonSync |= (f.flags[i] & 0x10000) != 0;
    const bool video = handler == "vide";
    bool ok = true;   // an MPU starts at a point where decoding can start
    for (size_t i = 0; i < n; i++) {
        const bool sync = anyNonSync ? (i < f.flags.size() && !(f.flags[i] & 0x10000)) : (!video || i == 0);
        if (i >= whole.size() || !whole[i]) {
            if (i >= disposable.size() || disposable[i] != 1) ok = false;
            continue;
        }
        if (sync) ok = true;
        keep[i] = ok;
    }
    // The fragment ends in samples left out and a gap in time follows it: pictures reordered by composition offsets that are kept before
    // that run may be presented after pictures of it, which are gone, so the picture shown last runs ahead of the decode time the fragment
    // ends with. A player that closes a gap in time (ffmpeg does over 10 s of a transport stream) then puts the next picture it gets before
    // pictures it already showed. So the last ones kept are left out too, from the last one backwards (so no picture kept loses its
    // reference; a sync sample too, when it is presented after one left out, as with the leading pictures of an open GOP), until every
    // picture kept is presented before every picture left out at the end. When the next fragment follows on time nothing more is left out:
    // the run is then a still picture between two times that both stand.
    size_t s = n;   // where the run of samples left out at the end starts
    while (s > 0 && !keep[s - 1]) s--;
    if (!gapAfter || s == n || s == 0 || !(f.trunFlags & 0x800) || f.durations.size() < n || f.compositionOffsets.size() < n) return keep;
    std::vector<int64_t> pt(n);
    int64_t t = 0;
    for (size_t i = 0; i < n; i++) { pt[i] = t + f.compositionOffsets[i]; t += f.durations[i]; }
    int64_t first = INT64_MAX;   // the earliest presentation time left out at the end
    for (size_t j = s; j < n; j++) first = std::min(first, pt[j]);
    std::vector<int64_t> last(s);   // last[k]: the latest presentation time kept in samples 0..k
    for (size_t k = 0; k < s; k++) last[k] = std::max(k ? last[k - 1] : INT64_MIN, keep[k] ? pt[k] : INT64_MIN);
    for (size_t k = s; k-- > 0;) {
        if (keep[k] && last[k] < first) break;
        keep[k] = 0;
        first = std::min(first, pt[k]);   // and a sample left out further back is now at the end as well
    }
    return keep;
}

bool keepSamples(const MpuFragment& f, const std::vector<char>& keep, MpuFragment& out, uint64_t& lead) {
    out = f;
    out.sizes.clear();
    out.durations.clear();
    out.compositionOffsets.clear();
    out.flags.clear();
    out.trunEntries.clear();
    out.mediaBytes = 0;
    lead = 0;
    std::vector<uint64_t> dur;
    std::vector<size_t> kept;
    for (size_t i = 0; i < f.sizes.size(); i++) {
        if (i < keep.size() && keep[i]) {
            kept.push_back(i);
            dur.push_back(f.durations[i]);
        } else if (dur.empty()) lead += f.durations[i];
        else dur.back() += f.durations[i];   // the sample before it is shown on through its time
    }
    if (kept.empty()) return false;
    out.trunFlags = 0x01 | 0x100 | 0x200 | 0x400 | (f.trunFlags & 0x800);
    out.firstSampleFlags = 0;
    for (size_t k = 0; k < kept.size(); k++) {
        const size_t i = kept[k];
        if (dur[k] > 0xFFFFFFFFu) return false;
        out.sizes.push_back(f.sizes[i]);
        out.durations.push_back((uint32_t)dur[k]);
        out.compositionOffsets.push_back(f.compositionOffsets[i]);
        out.flags.push_back(i < f.flags.size() ? f.flags[i] : 0);
        out.mediaBytes += f.sizes[i];
        putBe32(out.trunEntries, (uint32_t)dur[k]);
        putBe32(out.trunEntries, f.sizes[i]);
        putBe32(out.trunEntries, out.flags.back());
        if (f.trunFlags & 0x800) putBe32(out.trunEntries, (uint32_t)f.compositionOffsets[i]);
    }
    // the per-sample boxes kept with the traf: sdtp follows the samples kept; sample groups and subsample information are left out
    out.trafOther.clear();
    const uint8_t* d = f.trafOther.data();
    size_t pos = 0;
    Box b;
    while (nextBox(d, f.trafOther.size(), pos, b)) {
        if (b.type == fourcc("sgpd")) append(out.trafOther, d + b.pos, b.size);
        else if (b.type == fourcc("sdtp") && bodySize(b) == 4 + f.sizes.size()) {
            std::vector<uint8_t> v(body(d, b), body(d, b) + 4);
            for (size_t i : kept) v.push_back(body(d, b)[4 + i]);
            putBox(out.trafOther, fourcc("sdtp"), v);
        }
    }
    return true;
}

// ---------------------------------------------------------------- the service

namespace {
// A data unit sent in fragments (MPU metadata, movie fragment): put back together by the fragment counter, which counts the fragments that
// follow (so the first one gives the total).
struct Pieces {
    std::map<int, std::vector<uint8_t>> got;
    int total = -1;
    size_t bytes = 0;
    void add(int fi, int counter, const uint8_t* p, size_t n) {
        if (fi == 0) { counter = 0; total = 1; }
        else if (fi == 1) total = counter + 1;
        else if (fi == 3 && counter != 0) return;
        if (total > 0 && counter >= total) return;
        if (got.count(counter)) return;
        got[counter].assign(p, p + n);
        bytes += n;
    }
    bool complete() const { return total > 0 && (int)got.size() == total && got.rbegin()->first == total - 1; }
    std::vector<uint8_t> join() const {
        std::vector<uint8_t> v;
        for (auto it = got.rbegin(); it != got.rend(); ++it) v.insert(v.end(), it->second.begin(), it->second.end());
        return v;
    }
};

// What one stored MFU fragment costs besides its bytes (map node, key, vector): counted against the limits, so that fragments with little or
// no data cannot hold memory the limits do not see.
constexpr size_t kMfuEntryCost = 96;

// A fragment of an MFU as it came, and whether it is the last fragment of its sample (the fragmentation indicator of its payload)
struct MfuPiece {
    std::vector<uint8_t> data;
    bool last = false;
};

// One movie fragment of an MPU (ISO/IEC 23008-1 lets an MPU have several): its moof, and the MFUs that name its sequence number.
struct Frag {
    std::vector<uint8_t> moof;           // the whole movie fragment once it is in
    std::map<std::pair<uint32_t, uint32_t>, MfuPiece> mfu;   // (sample, media offset) -> fragment, the hint sample in front at offset 0
    size_t mfuBytes = 0;
    bool read = false, bad = false;
    MpuFragment f;
    size_t bytes() const { return moof.size() + mfuBytes + mfu.size() * kMfuEntryCost; }
};

// serial number order, for keys that all lie within 2^31 of each other (the open MPUs of an asset stay within kSeqJump of its newest)
struct SeqLess { bool operator()(uint32_t a, uint32_t b) const { return seqDiff(a, b) < 0; } };

struct OpenMpu {
    Pieces meta, moofPart;               // the metadata, and a movie fragment sent in pieces
    std::map<uint32_t, Frag> frags;      // by movie fragment sequence number (the moof's mfhd, the MFU header's first field)
    long born = 0, touched = 0;
    // once its first movie fragment went out, the MPU stays open for the ones after it
    bool started = false, repaired = false;
    uint32_t fragDone = 0;
    // where the MFU sample numbers of the next movie fragment start, once one went out (they may count on through the MPU)
    bool haveNextSample = false;
    uint32_t nextSample = 0;               // the movie fragments up to this one are delivered or lost
    uint64_t base = 0, end = 0;          // its start on the asset's timeline, and how far its delivered fragments reach from there
    size_t bytes() const {
        size_t n = meta.bytes + moofPart.bytes;
        for (auto& g : frags) n += g.second.bytes();
        return n;
    }
};
typedef std::map<uint32_t, OpenMpu, SeqLess> OpenMap;

struct Lane {
    uint16_t pid = 0;
    OpenMap open;                        // by MPU sequence number
    bool haveInit = false, skip = false;
    MpuInit init;
    std::vector<uint8_t> sentInit;
    bool haveHigh = false, haveDone = false;
    uint32_t high = 0, lastDone = 0;
    uint32_t cand = 0;
    int candCount = 0;
    // timeline
    bool anchored = false;
    uint64_t nextBase = 0;
    uint32_t lastDelivered = 0;
    std::deque<uint64_t> durs;
    uint32_t fragNo = 0;
    std::map<uint32_t, uint64_t> times;  // MPU sequence number -> NTP presentation time (MP table)
    bool haveLast = false;               // the last movie fragment read, for one whose moof is lost
    MpuFragment last;
    bool haveFirst = false;              // the first MPU the asset saw: the receiver may have come in part of the way through it
    uint32_t firstSeq = 0;
};

constexpr int kMaxOpenPerLane = 8;
constexpr int kMaxFragsPerMpu = 64;
constexpr size_t kMaxOpenBytes = 128u << 20;
constexpr size_t kMaxSignallingBytes = 1u << 20;
constexpr size_t kMaxTimesPerLane = 64;
constexpr int kSeqJump = 1024, kSeqAgree = 3, kBehind = 3;
constexpr long kStaleMpus = 64;           // an MPU no packet has added to while the service opened this many others is given up
constexpr double kOriginSeconds = 10.0;   // the first asset starts this far in, so that another asset's earlier start stays positive

// the sequence number in the mfhd of a movie fragment (moof, then on air the mdat header)
bool moofSequence(const uint8_t* d, size_t n, uint32_t& seq) {
    size_t pos = 0;
    Box b;
    while (nextBox(d, n, pos, b)) {
        if (b.type != fourcc("moof")) continue;
        size_t q = b.pos + b.hdr;
        Box c;
        while (nextBox(d, b.pos + b.size, q, c))
            if (c.type == fourcc("mfhd") && bodySize(c) >= 8) { seq = loadBe32(body(d, c) + 4); return true; }
        return false;
    }
    return false;
}
} // namespace

struct MmtpService::Impl {
    uint32_t dstIp = 0;
    int dstPort = 0;
    std::map<uint16_t, Lane> lanes;
    std::map<uint16_t, std::vector<uint8_t>> sigBuf;   // fragmented signalling messages being put back together
    std::map<uint16_t, int> sigNext;
    size_t openBytes = 0;
    long clock = 0;
    bool alignBySeq = false;
    // the common timeline: a reference MPU (sequence number, time in seconds) and the NTP origin when the MP table gives times
    bool haveRef = false, ntpOrigin = false;
    uint32_t refSeq = 0;
    double refTime = 0;
    uint64_t originNtp = 0;
    MmtpStats st;
    std::function<void(uint16_t, const std::string&, bool, const std::vector<uint8_t>&)>* cb = nullptr;
    std::function<void(uint16_t, const MmtpAsset&)>* assetCb = nullptr;
    std::map<uint16_t, bool> announced;

    mutable std::mutex snapMu;           // what other threads read
    MmtpStats snapSt;
    std::vector<MmtpAsset> snapAssets;
    bool mptSeen = false;

    void signalling(uint16_t pid, const uint8_t* d, size_t n) {
        st.signalling++;
        if (n < 2) { st.bad++; return; }
        const int fi = d[0] >> 6;
        const bool longLen = (d[0] >> 1) & 1, agg = d[0] & 1;
        const int counter = d[1];
        std::vector<uint8_t> whole;
        const uint8_t* p = d + 2;
        size_t len = n - 2;
        if (fi != 0) {
            std::vector<uint8_t>& b = sigBuf[pid];
            int& next = sigNext[pid];
            if (fi == 1) { b.assign(p, p + len); next = counter - 1; return; }
            if (b.empty() || counter != next || b.size() + len > kMaxSignallingBytes) { b.clear(); return; }
            b.insert(b.end(), p, p + len);
            next = counter - 1;
            if (fi != 3) return;
            whole.swap(b);
            p = whole.data();
            len = whole.size();
        }
        std::vector<SignallingMessage> msgs;
        if (!parseSignallingMessages(p, len, agg, longLen, msgs)) {
            st.bad++;
            DIAG_DEBUG_EVERY(340807, 10.0, "An MMTP signalling message on packet id %u has a length that does not fit its packet, so it is ignored.", (unsigned)pid);
        }
        for (auto& m : msgs) {
            if (m.id < 0x11 || m.id > 0x20) continue;
            MpTable t;
            if (!parseMpTable(m.payload.data(), m.payload.size(), t)) {
                st.bad++;
                DIAG_DEBUG_EVERY(192662, 10.0, "An MMTP MP table (message %#x) is cut short or has a field this receiver cannot read, so it is ignored.", (unsigned)m.id);
                continue;
            }
            st.mpts++;
            for (auto& a : t.assets)
                for (uint16_t ap : a.packetIds) {
                    if (a.mpuTimes.empty()) continue;
                    auto& tm = lanes[ap].times;
                    for (auto& x : a.mpuTimes) tm[x.first] = x.second;
                    // keep the times nearest the newest MPU of this table; the oldest by serial number order goes first
                    const uint32_t newest = a.mpuTimes.back().first;
                    while (tm.size() > kMaxTimesPerLane) {
                        auto old = tm.begin();
                        for (auto it = tm.begin(); it != tm.end(); ++it)
                            if (seqDiff(it->first, newest) < seqDiff(old->first, newest)) old = it;
                        tm.erase(old);
                    }
                }
            if (t.tableId == 0x20) {
                {
                    std::lock_guard<std::mutex> lk(snapMu);
                    snapAssets = t.assets;
                    mptSeen = true;
                }
                for (auto& a : t.assets) {
                    const std::string k = assetKind(a.type);
                    if (k != "video" && k != "audio") continue;
                    for (uint16_t ap : a.packetIds)
                        if (!announced[ap]) { announced[ap] = true; if (*assetCb) (*assetCb)(ap, a); }
                }
            }
        }
    }

    uint64_t median(const std::deque<uint64_t>& v) const {
        std::vector<uint64_t> s(v.begin(), v.end());
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        return s[s.size() / 2];
    }

    // An MPU leaves the asset: one that sent nothing out is dropped; one that did is over, and the next MPU follows it on the timeline (a
    // duration far from the usual one, from a damaged table or movie fragments lost at its end, is taken as the usual one).
    // Unless `repair` is false, the movie fragments of an MPU that is the asset's oldest open one go out first with the samples of them that
    // can be played (see repair).
    void retire(Lane& l, OpenMap::iterator it, bool repair = true) {
        OpenMpu& o = it->second;
        const size_t had = o.bytes();
        if (repair && !l.skip && it == l.open.begin())
            for (auto g = o.frags.begin(); g != o.frags.end();) {
                if (o.started && g->first <= o.fragDone) { ++g; continue; }
                if (repairFrag(l, it->first, o, g)) g = o.frags.erase(g);
                else ++g;
            }
        openBytes -= std::min(openBytes, had);
        if (!l.haveDone || seqDiff(it->first, l.lastDone) > 0) { l.lastDone = it->first; l.haveDone = true; }
        if (!o.started) st.mpusDropped++;
        else {
            for (auto& g : o.frags)
                if (!g.second.moof.empty() || !g.second.mfu.empty()) {
                    st.fragmentsLost++;
                    DIAG_DEBUG_EVERY(586203, 10.0, "MPU %u of an MMTP asset ended with movie fragment %u incomplete, so that part of it is lost.", it->first, g.first);
                }
            const uint64_t dur = o.end;
            uint64_t step = dur;
            if (l.durs.size() >= 4) {
                const uint64_t m = median(l.durs);
                if (dur < m - m / 4 || dur > m + m / 4) step = m;
            }
            l.durs.push_back(dur);
            if (l.durs.size() > 16) l.durs.pop_front();
            l.nextBase = o.base + step;
        }
        l.open.erase(it);
    }

    bool retireOldestAnywhere() {
        Lane* bl = nullptr;
        OpenMap::iterator bi{};
        long biBorn = 0;
        for (auto& kv : lanes)
            for (auto it = kv.second.open.begin(); it != kv.second.open.end(); ++it)
                if (!bl || it->second.born < biBorn) { bl = &kv.second; bi = it; biBorn = it->second.born; }
        if (!bl) return false;
        retire(*bl, bi);   // repaired only if it is the oldest of its asset
        return true;
    }

    // a movie fragment is whole when the asset's metadata (from this MPU or an earlier one), its moof and every byte of every media sample are
    // in; the MFU sample numbers count either from 1 in each movie fragment or on through the MPU, so both are tried
    bool assemble(Lane& l, Frag& g, std::vector<uint8_t>& media, uint32_t& base) {
        if (!l.haveInit || g.moof.empty()) return false;
        if (!g.read) {
            g.read = true;
            g.bad = !readMpuFragment(g.moof.data(), g.moof.size(), l.init, g.f);
            if (!g.bad) { l.last = g.f; l.haveLast = true; }
        }
        if (g.bad || g.mfu.empty()) return false;
        const MpuFragment& f = g.f;
        if (g.mfuBytes < f.mediaBytes + (uint64_t)f.hintSize * f.sizes.size()) return false;
        const uint32_t first = g.mfu.begin()->first.first;
        base = 0;
        if (assembleFrom(g, 0, media)) return true;
        base = first - 1;
        return first > 1 && assembleFrom(g, first - 1, media);
    }

    bool assembleFrom(const Frag& g, uint32_t base, std::vector<uint8_t>& media) {
        const MpuFragment& f = g.f;
        media.clear();
        media.reserve((size_t)f.mediaBytes);
        auto it = g.mfu.begin();
        for (uint32_t s = 0; s < f.sizes.size(); s++) {
            const uint32_t sample = base + s + 1, want = f.sizes[s];
            uint32_t have = 0;
            while (it != g.mfu.end() && it->first.first < sample) ++it;
            for (; it != g.mfu.end() && it->first.first == sample; ++it) {
                const uint32_t off = it->first.second;
                const uint8_t* p = it->second.data.data();
                size_t n = it->second.data.size();
                if (off == 0) { if (n < f.hintSize) return false; p += f.hintSize; n -= f.hintSize; }
                if (off > have) return false;   // a hole
                if (off + n <= have) continue;
                const size_t skip = have - off;
                const size_t take = std::min<size_t>(n - skip, want - have);
                media.insert(media.end(), p + skip, p + skip + take);
                have += (uint32_t)take;
                if (have == want) { while (it != g.mfu.end() && it->first.first == sample) ++it; break; }
            }
            if (have != want) return false;
        }
        return true;
    }

    // A movie fragment of MPU `seq` goes out: `f` as sent (its time), `out` what is written (`f` itself when whole, the samples kept when
    // repaired, starting `lead` after it); `missing` fragments of the MPU before it were lost and are given its duration.
    void deliver(uint16_t pid, Lane& l, uint32_t seq, OpenMpu& o, const MpuFragment& f, const MpuFragment& out, uint64_t lead2, uint32_t missing,
                 const std::vector<uint8_t>& media) {
        const uint32_t ts = l.init.timescale;
        uint64_t dur = 0;
        int64_t presMin = INT64_MAX, dts = 0;
        for (size_t i = 0; i < f.durations.size(); i++) { presMin = std::min(presMin, dts + f.compositionOffsets[i]); dts += f.durations[i]; dur += f.durations[i]; }
        if (presMin == INT64_MAX) presMin = 0;
        const uint64_t lead = (uint64_t)missing * dur;
        if (!o.started) {
            uint64_t base;
            if (!l.anchored) {
                // the first MPU of an asset: its presentation time on the common timeline, from the MP table's NTP time when there is one,
                // else from its sequence number against the first MPU of the service (MPUs of all assets start together and last the same)
                auto t = l.times.find(seq);
                double sec;
                if (!alignBySeq && t != l.times.end() && (ntpOrigin || !haveRef)) {
                    if (!ntpOrigin) { ntpOrigin = true; originNtp = t->second - (uint64_t)(kOriginSeconds * 4294967296.0); }
                    sec = (double)(int64_t)(t->second - originNtp) / 4294967296.0;
                    if (!haveRef) { haveRef = true; refSeq = seq; refTime = sec; }
                } else {
                    if (!haveRef) { haveRef = true; refSeq = seq; refTime = kOriginSeconds; }
                    sec = refTime + seqDiff(seq, refSeq) * ((double)dur / ts);
                }
                const int64_t b = (int64_t)(sec * ts + 0.5) - presMin;
                base = b > 0 ? (uint64_t)b : 0;
                l.anchored = true;
            } else {
                base = l.nextBase;
                const int32_t gap = seqDiff(seq, l.lastDelivered) - 1;
                if (gap > 0 && gap < kSeqJump && !l.durs.empty()) base += (uint64_t)gap * median(l.durs);
            }
            o.started = true;
            o.base = base;
            o.end = 0;
            l.lastDelivered = seq;
            st.mpusDelivered++;
        }
        const uint64_t at = o.base + o.end + lead;
        o.end += lead + dur;
        if (l.sentInit != l.init.init) {
            l.sentInit = l.init.init;
            st.initsDelivered++;
            if (*cb) (*cb)(pid, l.init.handler, true, l.sentInit);
        }
        const std::vector<uint8_t> seg = buildMediaSegment(out, ++l.fragNo, at + lead2, media);
        st.fragmentsDelivered++;
        if (*cb) (*cb)(pid, l.init.handler, false, seg);
    }

    // movie fragment `n` of an MPU goes out, after the ones before it that went out or were lost
    void send(Lane& l, uint32_t seq, OpenMpu& o, uint32_t n, const MpuFragment& f, const MpuFragment& out, uint64_t lead, const std::vector<uint8_t>& media) {
        const uint32_t from = o.started ? o.fragDone + 1 : (n > 0 ? 1 : 0);
        const uint32_t missing = std::min<uint32_t>(n - std::min(n, from), kMaxFragsPerMpu);
        deliver(l.pid, l, seq, o, f, out, lead, missing, media);
        o.fragDone = n;
    }

    // The media bytes of sample `sample` (MFU sample number) that arrived from offset 0 on without a hole, the hint sample in front taken off:
    // `whole` when they reach `want` bytes (or, with `want` 0, the fragment marked last of its sample), and a pointer to the start of the
    // sample when its first fragment is there.
    struct SampleBytes { size_t have = 0; bool whole = false; const uint8_t* start = nullptr; size_t startSize = 0; };
    static SampleBytes sampleBytes(const Frag& g, uint32_t sample, uint32_t hint, uint32_t want, std::vector<uint8_t>* into) {
        SampleBytes r;
        for (auto it = g.mfu.lower_bound(std::make_pair(sample, 0u)); it != g.mfu.end() && it->first.first == sample; ++it) {
            const uint32_t off = it->first.second;
            const uint8_t* p = it->second.data.data();
            size_t n = it->second.data.size();
            if (off == 0) {
                if (n < hint) return r;
                p += hint;
                n -= hint;
                r.start = p;
                r.startSize = n;
            }
            if (off > r.have) break;   // a hole
            if (off + n <= r.have) continue;
            const size_t skip = r.have - off;
            const size_t take = want ? std::min<size_t>(n - skip, want - r.have) : n - skip;
            if (into) into->insert(into->end(), p + skip, p + skip + take);
            r.have += take;
            if (want ? r.have == want : it->second.last) { r.whole = true; break; }
        }
        // a fragment of the sample that runs past its size: the sample numbers or sizes are not what they seem, so it is not taken as whole
        if (r.whole && want)
            for (auto it = g.mfu.lower_bound(std::make_pair(sample, 0u)); it != g.mfu.end() && it->first.first == sample; ++it)
                if ((uint64_t)it->first.second + it->second.data.size() - (it->first.second == 0 ? hint : 0) > want) { r.whole = false; break; }
        return r;
    }

    // Whether what comes right after movie fragment `gi` of MPU `seq` is in already, so that no gap in time follows it: the next movie
    // fragment of the MPU with its moof, or, when this one ends the MPU (it reaches the usual MPU duration, or with none known yet it is the
    // last fragment here), the next MPU having started or holding a moof. Not known means a gap.
    bool followsOnTime(Lane& l, uint32_t seq, OpenMpu& o, std::map<uint32_t, Frag>::iterator gi, const MpuFragment& f) {
        auto nx = std::next(gi);
        if (nx != o.frags.end()) return nx->first == gi->first + 1 && !nx->second.moof.empty();
        uint64_t dur = 0;
        for (uint32_t d : f.durations) dur += d;
        const uint32_t from = o.started ? o.fragDone + 1 : (gi->first > 0 ? 1 : 0);
        const uint64_t reach = (o.started ? o.end : 0) + (uint64_t)(std::min<uint32_t>(gi->first - std::min(gi->first, from), kMaxFragsPerMpu) + 1) * dur;
        if (!l.durs.empty()) {
            const uint64_t m = median(l.durs);
            if (reach < m - m / 4) return false;   // movie fragments after this one were lost
        }
        auto it = l.open.find(seq + 1);
        if (it == l.open.end()) return false;
        if (it->second.started) return true;
        for (auto& g : it->second.frags)
            if (!g.second.moof.empty()) return true;
        return false;
    }

    // A movie fragment that cannot be whole any more goes out with the samples of it that can be played (playableSamples): the samples that
    // arrived whole, less those whose reference picture is gone or, when a gap in time follows (followsOnTime), that would be presented
    // after a picture cut at its end. Its time stays the time sent, so the lost samples become a still picture or a short silence, not a
    // jump. A movie fragment whose moof was lost is rebuilt from the asset's last one when that is safe to copy: every sample a sync
    // sample, no composition offsets (sound), the same number of samples or fewer here, each sample's size known from its last MFU fragment.
    // Otherwise, or when no sample can be played, it is lost as before.
    bool repairFrag(Lane& l, uint32_t seq, OpenMpu& o, std::map<uint32_t, Frag>::iterator gi) {
        Frag& g = gi->second;
        if (!l.haveInit || g.mfu.empty()) return false;
        MpuFragment f;
        bool rebuilt = false;
        if (!g.moof.empty()) {
            if (!g.read) {
                g.read = true;
                g.bad = !readMpuFragment(g.moof.data(), g.moof.size(), l.init, g.f);
                if (!g.bad) { l.last = g.f; l.haveLast = true; }
            }
            if (g.bad) return false;
            f = g.f;
        } else {
            if (!l.haveLast || l.init.handler != "soun" || l.last.sizes.empty()) return false;
            for (size_t i = 0; i < l.last.sizes.size(); i++)
                if ((i < l.last.flags.size() && (l.last.flags[i] & 0x10000)) || l.last.compositionOffsets[i] != 0) return false;
            f = l.last;
            rebuilt = true;
        }
        const uint32_t count = (uint32_t)f.sizes.size();
        // MFU sample numbers from 1 in each movie fragment, or on through the MPU
        const uint32_t first = g.mfu.begin()->first.first, lastNo = g.mfu.rbegin()->first.first;
        uint32_t base = 0;
        if (first < 1 || lastNo > count) {
            // on through the MPU: from where the movie fragment before ended, else from the first sample number here (when that one is lost,
            // the sizes do not match and the samples are not taken)
            base = o.haveNextSample ? o.nextSample : first - 1;
            if (rebuilt || first <= base || lastNo - base > count) return false;
        }
        std::vector<char> whole(count, 0), disposable(count, -1);
        if (rebuilt) {
            f.mediaBytes = 0;
            for (uint32_t s = 0; s < count; s++) {
                const SampleBytes b = sampleBytes(g, base + s + 1, f.hintSize, 0, nullptr);
                whole[s] = b.whole && b.have > 0 && b.have <= kMpuMaxSampleBytes;
                f.sizes[s] = whole[s] ? (uint32_t)b.have : 0;
                f.mediaBytes += f.sizes[s];
            }
        } else {
            for (uint32_t s = 0; s < count; s++) {
                const SampleBytes b = sampleBytes(g, base + s + 1, f.hintSize, f.sizes[s], nullptr);
                whole[s] = b.whole || f.sizes[s] == 0;
                if (!whole[s] && b.start) disposable[s] = (char)sampleDisposable(b.start, b.startSize, l.init);
            }
        }
        // the first MPU of the asset is only repaired from its start: when the receiver came in part of the way through it, what it has is the
        // end of an MPU, not an MPU with a hole
        if (seq == l.firstSeq && (rebuilt || !whole[0])) return false;
        const std::vector<char> keep = playableSamples(f, l.init.handler, whole, disposable, !followsOnTime(l, seq, o, gi, f));
        MpuFragment out;
        uint64_t lead = 0;
        if (!keepSamples(f, keep, out, lead)) return false;
        std::vector<uint8_t> media;
        media.reserve((size_t)out.mediaBytes);
        for (uint32_t s = 0; s < count; s++)
            if (keep[s] && f.sizes[s]) sampleBytes(g, base + s + 1, f.hintSize, f.sizes[s], &media);
        if (media.size() != out.mediaBytes) return false;
        long lost = 0, cut = 0;
        for (uint32_t s = 0; s < count; s++) { lost += !whole[s]; cut += whole[s] && !keep[s]; }
        if (!o.repaired) { o.repaired = true; st.mpusRepaired++; }
        st.fragmentsRepaired++;
        st.samplesLost += lost;
        st.samplesCut += cut;
        if (rebuilt) st.moofsRebuilt++;
        DIAG_DEBUG_EVERY(915773, 10.0, "MPU %u of MMTP packet id %u is incomplete: %u of its %u samples go out, %ld were lost and %ld more are left out (up to the next sync sample, or presented after one lost).",
                         seq, (unsigned)l.pid, (unsigned)out.sizes.size(), count, lost, cut);
        send(l, seq, o, gi->first, f, out, lead, media);
        o.haveNextSample = true;
        o.nextSample = base + count;
        return true;
    }

    // a sequence number far from the asset's newest (or well behind its last finished MPU) is believed only when a few packets in a row agree
    // on it: then the sender has restarted or skipped, the unfinished MPUs are dropped and the asset carries on from the new numbers
    bool checkSequence(uint16_t pid, Lane& l, uint32_t seq) {
        if (!l.haveHigh) { l.haveHigh = true; l.high = seq; return true; }
        const int32_t dh = seqDiff(seq, l.high);
        const bool far = dh > kSeqJump || dh < -kSeqJump;
        const bool back = !far && l.haveDone && seqDiff(seq, l.lastDone) < -kBehind;
        if (!far && !back) {
            if (dh > 0) l.high = seq;
            l.candCount = 0;
            return true;
        }
        if (l.candCount > 0 && l.cand == seq) l.candCount++;
        else { l.cand = seq; l.candCount = 1; }
        if (l.candCount < kSeqAgree) {
            if (far) st.seqRejected++;
            else st.late++;
            return false;
        }
        if (far) DIAG_DEBUG_EVERY(195666, 10.0, "The MPU sequence numbers of MMTP packet id %u jumped from %u to %u, so its unfinished MPUs are dropped and the asset carries on from the new numbers.", (unsigned)pid, l.high, seq);
        else DIAG_DEBUG_EVERY(824801, 10.0, "The MPU sequence numbers of MMTP packet id %u went back from %u to %u (the sender restarted), so its unfinished MPUs are dropped and the asset carries on from the new numbers.", (unsigned)pid, l.lastDone, seq);
        st.seqRestarts++;
        while (!l.open.empty()) retire(l, l.open.begin());
        l.haveDone = false;
        l.high = seq;
        l.lastDelivered = seq - 1;
        l.candCount = 0;
        return true;
    }

    Frag* fragFor(OpenMpu& o, uint32_t n) {
        if (o.started && n <= o.fragDone) { st.late++; return nullptr; }
        auto it = o.frags.find(n);
        if (it != o.frags.end()) return &it->second;
        if ((int)o.frags.size() >= kMaxFragsPerMpu) { st.bad++; return nullptr; }
        return &o.frags[n];
    }

    void takeMoof(OpenMpu& o, const std::vector<uint8_t>& m) {
        uint32_t n = 0;
        if (!moofSequence(m.data(), m.size(), n)) { st.bad++; return; }
        Frag* g = fragFor(o, n);
        if (g && g->moof.empty()) g->moof = m;
    }

    void mpu(uint16_t pid, const MpuPayload& m) {
        if (!m.timed) { st.nonTimed++; return; }
        Lane& l = lanes[pid];
        l.pid = pid;
        if (l.skip) return;
        const uint32_t seq = m.mpuSequence;
        if (!checkSequence(pid, l, seq)) return;
        if (l.haveDone && seqDiff(seq, l.lastDone) <= 0) { st.late++; return; }
        auto it = l.open.find(seq);
        if (it == l.open.end()) {
            if (!l.haveFirst) { l.haveFirst = true; l.firstSeq = seq; }
            it = l.open.emplace(seq, OpenMpu()).first;
            it->second.born = it->second.touched = ++clock;
            sweep();
        }
        OpenMpu& o = it->second;
        o.touched = clock;
        const size_t before = o.bytes();
        bool metaDone = false;
        for (auto& u : m.units) {
            if (m.fragmentType == MpuMetadata) { o.meta.add(m.fragmentation, m.fragmentCounter, u.first, u.second); metaDone = o.meta.complete(); }
            else if (m.fragmentType == MpuMovieFragment) {
                // each movie fragment is a data unit of its own: whole, or in pieces from a first one to a last one
                if (m.fragmentation == 0) { takeMoof(o, std::vector<uint8_t>(u.first, u.first + u.second)); continue; }
                if (m.fragmentation == 1 && o.moofPart.total > 0) {
                    auto f0 = o.moofPart.got.find(m.fragmentCounter);
                    const bool same = f0 != o.moofPart.got.end() && f0->second.size() == u.second && std::equal(u.first, u.first + u.second, f0->second.begin());
                    if (!same) o.moofPart = Pieces();   // a new movie fragment starts; the one before it lost a piece
                }
                o.moofPart.add(m.fragmentation, m.fragmentCounter, u.first, u.second);
                if (o.moofPart.complete()) { takeMoof(o, o.moofPart.join()); o.moofPart = Pieces(); }
            } else if (m.fragmentType == MpuMfu) {
                MfuHeader h;
                if (!parseMfuHeader(u.first, u.second, h)) { st.bad++; continue; }
                if (u.second == kMfuHeaderBytes) continue;   // no data
                Frag* g = fragFor(o, h.fragmentSequence);
                if (!g) continue;
                auto& slot = g->mfu[std::make_pair(h.sample, h.offset)];
                if (slot.data.empty()) {
                    slot.data.assign(u.first + kMfuHeaderBytes, u.first + u.second);
                    slot.last = m.fragmentation == 0 || m.fragmentation == 3;
                    g->mfuBytes += slot.data.size();
                }
            }
        }
        openBytes += o.bytes() - before;
        if (o.bytes() > kMpuMaxBytes) {
            DIAG_DEBUG_EVERY(383065, 10.0, "An MPU of MMTP packet id %u grew past %zu bytes before it was complete, so it is dropped.", (unsigned)pid, kMpuMaxBytes);
            retire(l, it, false);
            return;
        }
        if (metaDone) {
            const std::vector<uint8_t> meta = o.meta.join();
            MpuInit in;
            if (!readMpuMetadata(meta.data(), meta.size(), in)) st.bad++;
            else if (in.trackId == 0) {   // captions, data: not played yet
                l.skip = true;
                st.assetsSkipped++;
                DIAG_DEBUG_EVERY(627515, 30.0, "MMTP packet id %u carries a %s track, which this receiver does not play yet, so the asset is skipped.", (unsigned)pid, in.handler.c_str());
                while (!l.open.empty()) retire(l, l.open.begin(), false);
                return;
            } else if (!l.haveInit || in.init != l.init.init || in.trackId != l.init.trackId) {
                l.init = in;
                l.haveInit = true;
                for (auto& x : l.open)
                    for (auto& g : x.second.frags) { g.second.read = false; g.second.bad = false; }
            }
        }
        // whole movie fragments go out in order: the MPUs before the one a whole fragment belongs to are finished first, and its own
        // fragments before that one that are not whole are lost
        for (auto w = l.open.begin(); w != l.open.end(); ++w) {
            OpenMpu& x = w->second;
            const size_t xBefore = x.bytes();
            for (auto g = x.frags.begin(); g != x.frags.end();) {
                std::vector<uint8_t> media;
                uint32_t base = 0;
                if (!assemble(l, g->second, media, base)) { ++g; continue; }
                while (l.open.begin() != w) retire(l, l.open.begin());
                for (auto e = x.frags.begin(); e != g;) {
                    if (repairFrag(l, w->first, x, e)) { e = x.frags.erase(e); continue; }
                    st.fragmentsLost++;
                    DIAG_DEBUG_EVERY(902417, 10.0, "MPU %u of MMTP packet id %u lost movie fragment %u before a later one was whole, so that part of it is skipped.", w->first, (unsigned)pid, e->first);
                    e = x.frags.erase(e);
                }
                send(l, w->first, x, g->first, g->second.f, g->second.f, 0, media);
                x.haveNextSample = true;
                x.nextSample = base + (uint32_t)g->second.f.sizes.size();
                g = x.frags.erase(g);
            }
            openBytes -= std::min(openBytes, xBefore - x.bytes());
        }
        // an MPU far behind the newest one will not be completed any more; nor will one past the per-asset or the total limit
        while (!l.open.empty() && seqDiff(l.high, l.open.begin()->first) > kBehind) retire(l, l.open.begin());
        while ((int)l.open.size() > kMaxOpenPerLane) retire(l, l.open.begin());
        while (openBytes > kMaxOpenBytes) {
            DIAG_DEBUG_EVERY(314154, 10.0, "Unfinished MMTP MPUs hold more than %zu bytes, so the oldest one is dropped.", kMaxOpenBytes);
            if (!retireOldestAnywhere()) { openBytes = 0; break; }
        }
    }

    // MPUs no packet has added to for a long time (an asset that stopped being sent, so nothing overtakes them) are given up
    void sweep() {
        for (auto& kv : lanes)
            for (auto it = kv.second.open.begin(); it != kv.second.open.end();) {
                auto next = std::next(it);
                if (clock - it->second.touched > kStaleMpus) {
                    DIAG_DEBUG_EVERY(468293, 30.0, "MPU %u of MMTP packet id %u got no packet while the service opened %ld other MPUs, so it is given up.", it->first, (unsigned)kv.first, kStaleMpus);
                    retire(kv.second, it);
                }
                it = next;
            }
    }
};

MmtpService::MmtpService(uint32_t srcIp, uint32_t dstIp, int dstPort) : p_(new Impl) {
    (void)srcIp;
    p_->dstIp = dstIp;
    p_->dstPort = dstPort;
    p_->cb = &onObject;
    p_->assetCb = &onAsset;
}

MmtpService::~MmtpService() = default;

bool MmtpService::push(const UdpDatagram& d) {
    if (d.dstIp != p_->dstIp || d.dstPort != p_->dstPort) return false;
    Impl& im = *p_;
    im.st.packets++;
    MmtpPacket pk;
    if (!parseMmtpPacket(d.payload.data(), d.payload.size(), pk)) {
        im.st.bad++;
        DIAG_DEBUG_EVERY(241141, 10.0, "An MMTP packet of %zu bytes has a header that does not fit in it, so it is dropped.", d.payload.size());
    } else {
        if (pk.type == MmtpSignalling) im.signalling(pk.packetId, pk.payload, pk.payloadSize);
        else if (pk.type == MmtpMpu) {
            MpuPayload m;
            if (parseMpuPayload(pk.payload, pk.payloadSize, m)) im.mpu(pk.packetId, m);
            else im.st.bad++;
        }
    }
    std::lock_guard<std::mutex> lk(im.snapMu);
    im.snapSt = im.st;
    return true;
}

bool MmtpService::ready() const { std::lock_guard<std::mutex> lk(p_->snapMu); return p_->mptSeen && p_->snapSt.initsDelivered > 0; }
std::vector<MmtpAsset> MmtpService::assets() const { std::lock_guard<std::mutex> lk(p_->snapMu); return p_->snapAssets; }
MmtpStats MmtpService::stats() const { std::lock_guard<std::mutex> lk(p_->snapMu); return p_->snapSt; }
void MmtpService::setAlignBySequence(bool on) { p_->alignBySeq = on; }

} // namespace atsc3
} // namespace dect2
