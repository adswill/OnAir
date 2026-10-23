#include "dect2/tv/mheg.h"
#include <cstdio>

namespace dect2 {
namespace {
constexpr int kMaxDepth = 40;
constexpr size_t kMaxNodes = 400000;

std::string tagName(int cls, uint32_t tag) {
    char b[32];
    if (cls == 0) {
        switch (tag) {
        case 1: return "BOOLEAN";
        case 2: return "INTEGER";
        case 4: return "OCTET STRING";
        case 5: return "NULL";
        case 10: return "ENUMERATED";
        case 16: return "SEQUENCE";
        case 17: return "SET";
        }
        snprintf(b, sizeof b, "UNIVERSAL %u", tag);
    } else if (cls == 1) snprintf(b, sizeof b, "APPLICATION %u", tag);
    else if (cls == 2) snprintf(b, sizeof b, "[%u]", tag);
    else snprintf(b, sizeof b, "PRIVATE %u", tag);
    return b;
}

// parses one TLV at p[pos..end); returns false on malformed data
bool parseOne(const uint8_t* p, size_t& pos, size_t end, MhegNode& out, int depth, size_t& nodes, std::string& err) {
    if (depth > kMaxDepth) { err = "nesting too deep"; return false; }
    if (++nodes > kMaxNodes) { err = "too many objects"; return false; }
    if (pos >= end) { err = "truncated tag"; return false; }
    uint8_t b0 = p[pos++];
    out.cls = b0 >> 6;
    out.constructed = (b0 & 0x20) != 0;
    uint32_t tag = b0 & 0x1F;
    if (tag == 0x1F) {
        tag = 0;
        for (int i = 0;; i++) {
            if (pos >= end || i > 4) { err = "bad tag number"; return false; }
            uint8_t c = p[pos++];
            tag = (tag << 7) | (c & 0x7F);
            if (!(c & 0x80)) break;
        }
    }
    out.tag = tag;
    out.name = tagName(out.cls, tag);
    if (pos >= end) { err = "truncated length"; return false; }
    uint8_t l0 = p[pos++];
    size_t len = 0;
    if (l0 == 0x80) {
        if (!out.constructed) { err = "indefinite length on a primitive"; return false; }
        out.indefinite = true;
        for (;;) {
            if (pos + 2 <= end && p[pos] == 0 && p[pos + 1] == 0) { pos += 2; return true; }
            MhegNode k;
            if (!parseOne(p, pos, end, k, depth + 1, nodes, err)) return false;
            out.kids.push_back(std::move(k));
        }
    }
    if (l0 & 0x80) {
        int nb = l0 & 0x7F;
        if (nb > 4 || pos + (size_t)nb > end) { err = "bad length"; return false; }
        for (int i = 0; i < nb; i++) len = (len << 8) | p[pos++];
    } else len = l0;
    if (len > end - pos) { err = "length beyond the data"; return false; }
    size_t stop = pos + len;
    if (out.constructed) {
        while (pos < stop) {
            MhegNode k;
            if (!parseOne(p, pos, stop, k, depth + 1, nodes, err)) return false;
            out.kids.push_back(std::move(k));
        }
    } else {
        out.value.assign(p + pos, p + stop);
        pos = stop;
    }
    return true;
}

void dumpRec(const MhegNode& n, int depth, int maxDepth, std::string& s) {
    s.append((size_t)depth * 2, ' ');
    s += n.name;
    if (n.constructed) {
        char b[48];
        snprintf(b, sizeof b, " (%zu)", n.kids.size());
        s += b;
    } else {
        bool text = n.value.size() > 1;
        for (uint8_t c : n.value) if (c < 0x20 || c > 0x7E) text = false;
        if (text) s += " \"" + std::string(n.value.begin(), n.value.end()) + "\"";
        else if (n.value.size() <= 8 && !n.value.empty()) {
            int64_t v = (n.value[0] & 0x80) ? -1 : 0;
            for (uint8_t c : n.value) v = (int64_t)(((uint64_t)v << 8) | c);
            char b[48];
            snprintf(b, sizeof b, " %lld", (long long)v);
            s += b;
        } else if (!n.value.empty()) {
            char b[32];
            snprintf(b, sizeof b, " %zu bytes", n.value.size());
            s += b;
        }
    }
    s += '\n';
    if (depth + 1 >= maxDepth) { if (!n.kids.empty()) { s.append((size_t)(depth + 1) * 2, ' '); s += "...\n"; } return; }
    for (const auto& k : n.kids) dumpRec(k, depth + 1, maxDepth, s);
}

void putLen(std::vector<uint8_t>& o, size_t len) {
    if (len < 0x80) { o.push_back((uint8_t)len); return; }
    uint8_t t[4]; int n = 0;
    while (len) { t[n++] = (uint8_t)len; len >>= 8; }
    o.push_back((uint8_t)(0x80 | n));
    while (n) o.push_back(t[--n]);
}

// ---- names of the MHEG-5 tags, from the ASN.1 module ISO13522-MHEG-5 (ITU-T T.172 02/1998) as published in redbutton-browser/ISO13522-MHEG-5.asn
// (RedButton MHEG-5 engine, GPL-2.0-or-later; the tag numbers are facts of the standard). The module uses IMPLICIT TAGS, so the numbers are context-specific tags
// and a tag number means different things in different places: the names are given by where a node sits (group, item list, item). Tags not in that module
// text stay numbered ("[n]").
const char* groupAttr(uint32_t t, bool scene) {
    switch (t) {
    case 2: return "standard-identifier"; case 3: return "standard-version"; case 4: return "object-information";
    case 5: return "on-start-up"; case 6: return "on-close-down"; case 7: return "original-group-cache-priority"; case 8: return "items";
    }
    if (!scene) switch (t) { case 35: return "on-spawn-close-down"; case 36: return "on-restart"; case 37: return "default-attributes"; }
    else switch (t) { case 51: return "input-event-register"; case 52: return "scene-coordinate-system"; case 53: return "aspect-ratio"; case 54: return "moving-cursor"; case 55: return "next-scenes"; }
    return nullptr;
}
const char* itemClass(uint32_t t) {   // GroupItem ::= CHOICE { resident-program [9] ... list-group [34] }
    static const char* names[] = {"ResidentProgram", "RemoteProgram", "InterchangedProgram", "Palette", "Font", "CursorShape", "BooleanVariable", "IntegerVariable",
        "OctetStringVariable", "ObjectRefVariable", "ContentRefVariable", "Link", "Stream", "Bitmap", "LineArt", "DynamicLineArt", "Rectangle", "Hotspot", "SwitchButton",
        "PushButton", "Text", "EntryField", "HyperText", "Slider", "TokenGroup", "ListGroup"};
    return t >= 9 && t <= 34 ? names[t - 9] : nullptr;
}
const char* itemAttr(uint32_t cls, uint32_t t) {   // cls: the item class tag (9..34)
    switch (t) {   // IngredientClass
    case 56: return "initially-active"; case 57: return "content-hook"; case 58: return "original-content"; case 59: return "shared";
    }
    if (cls == 20) switch (t) { case 62: return "link-condition"; case 63: return "link-effect"; }
    if (cls >= 9 && cls <= 11) switch (t) { case 64: return "name"; case 65: return "initially-available"; }   // ProgramClass (resident, remote, interchanged)
    if (cls >= 22 && cls <= 32) switch (t) {   // VisibleClass
    case 76: return "original-box-size"; case 77: return "original-position"; case 78: return "original-palette-ref";
    }
    if (cls == 22) switch (t) { case 79: return "tiling"; case 80: return "original-transparency"; }
    if (cls == 23 || cls == 24 || cls == 25) switch (t) {   // LineArt, DynamicLineArt, Rectangle
    case 81: return "bordered-bounding-box"; case 82: return "original-line-width"; case 83: return "original-line-style"; case 84: return "original-ref-line-colour"; case 85: return "original-ref-fill-colour";
    }
    if (cls == 29) switch (t) {   // Text
    case 38: return "character-set"; case 39: return "background-colour"; case 41: return "text-colour"; case 43: return "font-attributes";
    case 86: return "original-font"; case 87: return "horizontal-justification"; case 88: return "vertical-justification"; case 89: return "line-orientation"; case 90: return "start-corner"; case 91: return "text-wrapping";
    }
    return nullptr;
}
void nameItem(MhegNode& item, uint32_t cls) {
    for (auto& k : item.kids) {
        if (k.cls != 2) continue;
        if (const char* n = itemAttr(cls, k.tag)) k.name = n;
        if (k.tag == 58)   // ContentBody: included-content OCTET STRING or ReferencedContent SEQUENCE { content-reference, [60] content-size, [61] content-cache-priority }
            for (auto& r : k.kids) if (r.cls == 0 && r.tag == 16) for (auto& f : r.kids) { if (f.cls == 2 && f.tag == 60) f.name = "content-size"; if (f.cls == 2 && f.tag == 61) f.name = "content-cache-priority"; }
    }
}
void nameTree(MhegNode& root) {
    if (root.cls != 2 || root.tag > 1) return;
    const bool scene = root.tag == 1;
    root.name = scene ? "Scene" : "Application";   // InterchangedObject ::= CHOICE { application [0], scene [1] }
    for (auto& k : root.kids) {
        if (k.cls != 2) continue;
        if (const char* n = groupAttr(k.tag, scene)) k.name = n;
        if (k.tag == 8) for (auto& it : k.kids) if (it.cls == 2) if (const char* n = itemClass(it.tag)) { const uint32_t c = it.tag; it.name = n; nameItem(it, c); }
    }
}
} // namespace

MhegParse mhegParse(const uint8_t* p, size_t n) {
    MhegParse r;
    size_t pos = 0;
    while (pos < n) {
        MhegNode nd;
        size_t before = pos;
        if (!parseOne(p, pos, n, nd, 0, r.nodes, r.error)) { pos = before; break; }
        nameTree(nd);
        r.roots.push_back(std::move(nd));
    }
    r.consumed = pos;
    return r;
}

bool mhegLooksLikeApplication(const uint8_t* p, size_t n) {
    if (n < 2 || (p[0] != 0xA0 && p[0] != 0xA1)) return false;
    MhegNode nd;
    size_t pos = 0, nodes = 0;
    std::string err;
    return parseOne(p, pos, n, nd, 0, nodes, err);
}

std::string mhegDump(const MhegNode& n, int maxDepth) {
    std::string s;
    dumpRec(n, 0, maxDepth, s);
    return s;
}

std::vector<uint8_t> mhegEncode(const MhegNode& n) {
    std::vector<uint8_t> body;
    if (n.constructed) for (const auto& k : n.kids) { auto e = mhegEncode(k); body.insert(body.end(), e.begin(), e.end()); }
    else body = n.value;
    std::vector<uint8_t> o;
    uint8_t b0 = (uint8_t)((n.cls << 6) | (n.constructed ? 0x20 : 0));
    if (n.tag < 31) o.push_back(b0 | (uint8_t)n.tag);
    else {
        o.push_back(b0 | 0x1F);
        uint8_t t[5]; int k = 0; uint32_t v = n.tag;
        while (v) { t[k++] = v & 0x7F; v >>= 7; }
        while (k) { uint8_t c = t[--k]; o.push_back(k ? (c | 0x80) : c); }
    }
    putLen(o, body.size());
    o.insert(o.end(), body.begin(), body.end());
    return o;
}

} // namespace dect2
