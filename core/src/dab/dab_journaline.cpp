// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Journaline decoder, see dab_journaline.h. Clause numbers are those of ETSI TS 102 979 V1.1.1 unless named otherwise.
#include "dect2/dab/dab_journaline.h"
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_proto.h"
#include "dect2/common/inflate.h"
#include <algorithm>
#include "dect2/common/endian.h"

namespace dect2 {

namespace {

constexpr size_t kMaxField = 4092;

// UTF-8 text with the invalid bytes replaced (a bad object must not put garbage into the UI)
void appendUtf8(std::string& s, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n;) {
        const uint8_t c = d[i];
        size_t l = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        bool ok = l && i + l <= n;
        for (size_t k = 1; ok && k < l; k++) ok = (d[i + k] & 0xC0) == 0x80;
        if (ok) { s.append((const char*)d + i, l); i += l; }
        else { s += '?'; i++; }
    }
}

// The content section: JML codes 0x00..0x0F, escape sequences 0x10..0x1F.
struct Parser {
    const uint8_t* d; size_t n; size_t p = 0;
    JlObject& o;
    int block = -1;               // -1 none, else the JML code of the open block
    std::string text;             // text of the open block
    int linkIdx = -1;

    void closeBlock() {
        switch (block) {
            case 1: o.title = text; break;
            case 2: if (linkIdx >= 0 && linkIdx < (int)o.links.size()) o.links[(size_t)linkIdx].text = text; break;
            case 3: o.body = text; break;
            case 4: o.items.push_back(text); break;
            default: break;
        }
        text.clear();
        block = -1;
    }
    void run() {
        while (p < n) {
            const uint8_t c = d[p];
            if (c < 0x10) {
                p++;
                if (c == 0x00) { closeBlock(); continue; }
                if (c == 0x02) {   // link item: 2 byte target, then the text
                    closeBlock();
                    if (p + 2 > n) return;
                    JlLink l; l.target = (uint16_t)((d[p] << 8) | d[p + 1]); p += 2;
                    if (o.links.size() < 64) { o.links.push_back(l); linkIdx = (int)o.links.size() - 1; block = 2; }
                    continue;
                }
                if (c == 0x01 || c == 0x03 || c == 0x04) { closeBlock(); block = c; continue; }
                if (c == 0x05) { if (block == 4) text += '\t'; continue; }
                continue;   // unknown JML code: ignored, the block goes on
            }
            if (c < 0x20) { escape(c); continue; }
            // visible text up to the next control code
            size_t e = p;
            while (e < n && d[e] >= 0x20) e++;
            if (block >= 0) appendUtf8(text, d + p, e - p);
            p = e;
        }
        closeBlock();
    }
    void escape(uint8_t c) {
        p++;
        switch (c) {
            case 0x10: text += (block == 3) ? '\n' : ' '; return;   // preferred line break (a space where it cannot be shown)
            case 0x11: case 0x12: case 0x13: return;
            case 0x14: if (block == 3 && !text.empty()) text += "\n\n"; return;   // end of the introductory section
            case 0x1A: case 0x1B: {
                if (c == 0x1B) {   // a stray continuation: skip its bytes
                    if (p >= n) return;
                    p += (size_t)d[p] + 2;
                    if (p > n) p = n;
                    return;
                }
                std::vector<uint8_t> pl;
                bool more = true;
                bool first = true;
                while (more) {
                    if (p >= n) return;
                    const size_t len = (size_t)d[p] + 1;
                    p++;
                    if (p + len > n) { p = n; return; }
                    pl.insert(pl.end(), d + p, d + p + len);
                    p += len;
                    more = len == 256 && p < n && d[p] == 0x1B && (p++, true);
                    first = false;
                }
                (void)first;
                dataSection(pl);
                return;
            }
            case 0x1C: case 0x1D: if (p < n) p++; return;   // extended codes: none defined
            default: return;
        }
    }
    void dataSection(const std::vector<uint8_t>& pl) {
        if (pl.empty()) return;
        if (pl[0] == 0xA0 && pl.size() >= 4) o.language.assign((const char*)&pl[1], 3);
        else if (pl[0] == 0x03 && pl.size() >= 2 && o.general.size() < 16) {   // general link target (5.3.2.3)
            JlGeneralLink g; g.type = pl[1];
            if (g.type == 0) {
                if (pl.size() < 4) return;
                g.target = (uint16_t)((pl[2] << 8) | pl[3]);
                if (pl.size() > 5 && pl[4] == 0x00) { Parser sub{pl.data() + 5, pl.size() - 5, 0, o}; sub.block = 3; sub.runInto(g.label); }
            } else if (g.type >= 1 && g.type <= 4) {
                size_t e = 2;
                while (e < pl.size() && pl[e] != 0) e++;
                appendUtf8(g.address, pl.data() + 2, e - 2);
                if (e + 1 < pl.size()) { Parser sub{pl.data() + e + 1, pl.size() - e - 1, 0, o}; sub.block = 3; sub.runInto(g.label); }
            } else return;
            o.general.push_back(g);
        }
    }
    // text only (the link label): escape sequences resolved, no blocks
    void runInto(std::string& out) {
        while (p < n) {
            const uint8_t c = d[p];
            if (c < 0x10) { p++; continue; }
            if (c < 0x20) { escape(c); continue; }
            size_t e = p;
            while (e < n && d[e] >= 0x20) e++;
            appendUtf8(text, d + p, e - p);
            p = e;
        }
        out = text;
    }
};

uint16_t be16(const uint8_t* p) { return (uint16_t)dect2::endian::loadBe16(p); }

} // namespace

// The framing checks (data group, TOC block, user application data, packet headers) are dab_proto (Rust).
bool jlDataGroup(const uint8_t* d, size_t n, int& type, const uint8_t*& field, size_t& fieldLen, bool* crcBad) {
    if (crcBad) *crcBad = false;
    const dabp::JlGroup g = dabp::jlDataGroup(d, n);
    if (g.status == 2) { if (crcBad) *crcBad = true; return false; }
    if (g.status != 0) return false;
    type = g.type;
    field = d + 2;
    fieldLen = g.fieldLen;
    return true;
}

bool jlParseObject(const uint8_t* d, size_t n, int extHeader, JlObject& o) {
    if (extHeader < 0 || n < 3 + (size_t)extHeader) return false;
    if (n > kMaxField) return false;
    JlObject r;
    r.id = be16(d);
    r.type = d[2] >> 5;
    r.isStatic = d[2] & 0x10;
    r.compressed = d[2] & 0x08;
    r.revision = d[2] & 7;
    r.size = n;
    if (r.type < 1 || r.type > 4) return false;   // reserved types are ignored
    size_t p = 3 + (size_t)extHeader;
    std::vector<uint8_t> content;
    if (r.compressed) {
        if (p >= n || d[p] != 0x08) return false;   // 0x08 = deflate, the only method
        p++;
        if (!inflateRaw(d + p, n - p, content, 1u << 16)) return false;
    } else content.assign(d + p, d + n);
    Parser ps{content.data(), content.size(), 0, r};
    ps.run();
    if (r.type == 1 && r.links.empty()) return false;
    if (r.type == 4 && r.items.empty()) return false;
    o = std::move(r);
    return true;
}

bool jlParseToc(const uint8_t* d, size_t n, int& revision, int& blockCount, int& blockIndex, int& objectCount, int& timeoutMin, std::vector<JlTocEntry>& entries) {
    dabp::JlTocInfo info;
    std::vector<dabp::JlTocEntry> ents;
    if (!dabp::jlParseToc(d, n, info, ents)) return false;
    revision = info.revision; blockCount = info.blockCount; blockIndex = info.blockIndex; objectCount = info.objectCount; timeoutMin = info.timeoutMin;
    entries.clear();
    for (const dabp::JlTocEntry& e : ents) entries.push_back({(uint16_t)e.id, (uint8_t)e.desc});
    return true;
}

bool jlUserAppData(const uint8_t* d, size_t n, int& version, int& extHeader, int& entry) { return dabp::jlUserAppData(d, n, version, extHeader, entry); }

// ---------------------------------------------------------------- the service

void JournalineDecoder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    objs_.clear(); toc_ = JlToc(); st_ = JlStats(); seq_++;
}

void JournalineDecoder::feedDataGroup(const uint8_t* d, size_t n) {
    int type; const uint8_t* f; size_t fl; bool bad = false;
    const bool ok = jlDataGroup(d, n, type, f, fl, &bad);
    std::lock_guard<std::mutex> lk(mu_);
    if (!ok) { if (bad) st_.badCrc++; else st_.rejected++; return; }
    st_.groups++;
    if (type == 0) {
        JlObject o;
        if (!jlParseObject(f, fl, ext_, o)) { st_.badObjects++; return; }
        st_.objects++;
        auto it = objs_.find(o.id);
        if (it != objs_.end() && it->second.size == o.size && it->second.revision == o.revision && it->second.type == o.type && it->second.title == o.title &&
            it->second.body == o.body && it->second.items == o.items && it->second.links.size() == o.links.size()) return;   // the carousel again
        o.seq = ++seq_;
        st_.updates++;
        objs_[o.id] = std::move(o);
    } else {
        int rev, cnt, idx, nobj, to;
        std::vector<JlTocEntry> ent;
        if (!jlParseToc(f, fl, rev, cnt, idx, nobj, to, ent)) { st_.badObjects++; return; }
        st_.tocBlocks++;
        if (!toc_.have || toc_.revision != rev) { toc_ = JlToc(); toc_.have = true; toc_.revision = rev; seq_++; }
        toc_.blockCount = cnt; toc_.objectCount = nobj; toc_.timeoutMin = to;
        toc_.blocks[idx] = true;
        for (const auto& e : ent) toc_.entries[e.id] = e.desc;
    }
}

bool JournalineDecoder::get(uint16_t id, JlObject& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    out = it->second;
    return true;
}

JlStats JournalineDecoder::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    JlStats s = st_;
    s.cached = (int)objs_.size();
    if (toc_.have) {
        s.expected = toc_.objectCount;
        int have = 0;
        for (const auto& kv : toc_.entries) if (objs_.count(kv.first)) have++;
        s.expectedHave = have;
    }
    return s;
}

std::vector<uint16_t> JournalineDecoder::ids() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint16_t> v;
    for (const auto& kv : objs_) v.push_back(kv.first);
    return v;
}

std::vector<JlObject> JournalineDecoder::all() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<JlObject> v;
    for (const auto& kv : objs_) v.push_back(kv.second);
    return v;
}

// ---------------------------------------------------------------- packet mode (EN 300 401 clause 5.3.2.1)
// header: [length code 2][continuity index 2][first 1][last 1][address 10 bits] [command 1][useful data length 7], data field, CRC-16 of all before.

bool DabPacketDemux::packet(const uint8_t* p, size_t len, std::vector<uint8_t>& out) {
    const dabp::DabPacket k = dabp::dabPacket(p, len);
    if (!k.crcOk) { badPackets++; return false; }
    packets++;
    const int ci = k.ci;
    const bool first = k.first, last = k.last;
    const int addr = k.addr;
    const bool cmd = k.cmd;
    const size_t ul = k.ul;
    if (addr != addr_ || cmd || ul > len - 5) return true;   // somebody else's packet, a command or padding: valid, not ours
    if (ci == lastCi_) return true;   // the continuity index counts every packet of the address: the same one again is a repetition
    if (on_ && !first && ci != ((lastCi_ + 1) & 3)) { on_ = false; asm_.clear(); }   // a packet was lost: the group is gone
    lastCi_ = ci;
    if (first) { asm_.clear(); on_ = true; }
    else if (!on_) return true;
    asm_.insert(asm_.end(), p + 3, p + 3 + ul);
    if (asm_.size() > (maxLen_ ? maxLen_ : (size_t)kMaxField + 4 + 8)) { on_ = false; asm_.clear(); return true; }
    if (last) { out.swap(asm_); asm_.clear(); on_ = false; }
    return true;
}

} // namespace dect2
