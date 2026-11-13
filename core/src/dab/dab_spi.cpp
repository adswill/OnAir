// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB SPI (electronic programme guide): the TS 102 371 binary encoding and plain XML -> SpiNode tree, time and duration conversions, and the
// SpiStore model built from the trees. See dab_spi.h.
#include "dect2/dab/dab_spi.h"
#include "dect2/common/inflate.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {

namespace {
const std::string kEmpty;
constexpr int kMaxDepth = 24;
constexpr size_t kMaxNodes = 200000;

// ---- time helpers
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)((int64_t)yoe + era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
}
std::string hex(unsigned v, int digits = 0) {
    char b[16];
    snprintf(b, sizeof b, "%0*x", digits, v);
    return b;
}
}  // namespace

int64_t spiMjdToUtc(int mjd, int hour, int minute, int second) { return ((int64_t)mjd - 40587) * 86400 + hour * 3600 + minute * 60 + second; }

bool spiParseTime(const std::string& s, int64_t& utc, int& offMin) {
    int y, mo, d, h = 0, mi = 0, se = 0;
    const char* p = s.c_str();
    if (sscanf(p, "%4d-%2d-%2d", &y, &mo, &d) != 3 || s.size() < 10) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1970 || y > 2200) return false;
    size_t i = 10;
    offMin = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
        if (sscanf(p + i + 1, "%2d:%2d:%2d", &h, &mi, &se) < 2) return false;
        i += 1;
        // skip hh:mm[:ss[.fff]]
        int colons = 0;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == ':' || s[i] == '.')) { if (s[i] == ':') colons++; if (s[i] == '.') { while (i < s.size() && std::isdigit((unsigned char)s[i + 1])) i++; } i++; (void)colons; }
        if (i < s.size() && s[i] == '.') i++;
    }
    if (h > 24 || mi > 59 || se > 60) return false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        const int sign = s[i] == '-' ? -1 : 1;
        int oh = 0, om = 0;
        const std::string r = s.substr(i + 1);
        if (r.size() >= 5 && r[2] == ':') { oh = atoi(r.substr(0, 2).c_str()); om = atoi(r.substr(3, 2).c_str()); }
        else if (r.size() >= 4 && std::isdigit((unsigned char)r[2])) { oh = atoi(r.substr(0, 2).c_str()); om = atoi(r.substr(2, 2).c_str()); }
        else if (r.size() >= 2) oh = atoi(r.substr(0, 2).c_str());
        if (oh > 15 || om > 59) return false;
        offMin = sign * (oh * 60 + om);
    }
    utc = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se - offMin * 60;
    return true;
}

std::string spiFormatTime(int64_t utc, int offMin) {
    const int64_t loc = utc + offMin * 60;
    int64_t days = loc / 86400, rem = loc % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y, m, d;
    civilFromDays(days, y, m, d);
    char b[48];
    if (offMin == 0) snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
    else {
        const int a = offMin < 0 ? -offMin : offMin;
        snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60), offMin < 0 ? '-' : '+', a / 60, a % 60);
    }
    return b;
}

bool spiParseDuration(const std::string& s, int& seconds) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-') i++;
    if (i >= s.size() || s[i] != 'P') return false;
    i++;
    bool timePart = false, any = false;
    double total = 0;
    while (i < s.size()) {
        if (s[i] == 'T') { timePart = true; i++; continue; }
        size_t j = i;
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) j++;
        if (j == i || j >= s.size()) return false;
        const double v = atof(s.substr(i, j - i).c_str());
        const char u = s[j];
        if (!timePart) { if (u == 'D') total += v * 86400; else if (u == 'W') total += v * 604800; else if (u == 'Y' || u == 'M') return false; else return false; }
        else if (u == 'H') total += v * 3600;
        else if (u == 'M') total += v * 60;
        else if (u == 'S') total += v;
        else return false;
        any = true;
        i = j + 1;
    }
    if (!any || total < 0 || total > 4e7) return false;
    seconds = (int)(total + 0.5);
    return true;
}

// ---------------------------------------------------------------- tree
const std::string& SpiNode::get(const char* key) const {
    auto it = attr.find(key);
    return it == attr.end() ? kEmpty : it->second;
}
const SpiNode* SpiNode::child(const char* nm) const {
    for (const SpiNode& k : kids) if (k.name == nm) return &k;
    return nullptr;
}

namespace {
// ---------------------------------------------------------------- binary (TS 102 371)
enum VT { VStr, VTime, VDur, VBearer, VEnsId, VGenre, VU16, VU24, VEnum };
struct AttrDef { int elem; int tag; const char* name; VT type; const char* const* enums; int ne; };   // enums: index = value, null = unknown

const char* const kEnGroupType[] = {nullptr, nullptr, "series", "show", "programConcept", "magazine", "programCompilation", "otherCollection", "otherChoice", "topic"};
const char* const kEnBroadcast[] = {nullptr, "on-air", "off-air"};
const char* const kEnRecommend[] = {nullptr, "no", "yes"};
const char* const kEnMultimedia[] = {nullptr, nullptr, "logo_unrestricted", nullptr, "logo_colour_square", nullptr, "logo_colour_rectangle"};
const char* const kEnGenreType[] = {nullptr, "main", "secondary", "other"};

struct ElemDef { int tag; const char* name; };
const ElemDef kElems[] = {
    {0x02, "epg"}, {0x03, "serviceInformation"}, {0x10, "shortName"}, {0x11, "mediumName"}, {0x12, "longName"}, {0x13, "mediaDescription"},
    {0x14, "genre"}, {0x16, "keywords"}, {0x17, "memberOf"}, {0x18, "link"}, {0x19, "location"}, {0x1A, "shortDescription"},
    {0x1B, "longDescription"}, {0x1C, "programme"}, {0x20, "programmeGroups"}, {0x21, "schedule"}, {0x23, "programmeGroup"}, {0x24, "scope"},
    {0x25, "serviceScope"}, {0x26, "ensemble"}, {0x28, "service"}, {0x29, "bearer"}, {0x2B, "multimedia"}, {0x2C, "time"}, {0x2D, "bearer"},
    {0x2E, "programmeEvent"}, {0x2F, "relativeTime"}, {0x31, "radiodns"}, {0x32, "geolocation"}, {0x33, "country"}, {0x34, "point"},
    {0x35, "polygon"}, {0x36, "onDemand"}, {0x37, "presentationTime"}, {0x38, "acquisitionTime"},
};
const char* elemName(int tag) {
    for (const ElemDef& e : kElems) if (e.tag == tag) return e.name;
    return nullptr;
}

// The attribute tables (annex E). Element = its tag.
const AttrDef kAttrs[] = {
    {0x14, 0x80, "href", VGenre, nullptr, 0}, {0x14, 0x81, "type", VEnum, kEnGenreType, 4},
    {0x16, 0x80, "xml:lang", VStr, nullptr, 0},
    {0x18, 0x80, "uri", VStr, nullptr, 0}, {0x18, 0x81, "mimeValue", VStr, nullptr, 0}, {0x18, 0x82, "xml:lang", VStr, nullptr, 0}, {0x18, 0x83, "description", VStr, nullptr, 0}, {0x18, 0x84, "expiryTime", VTime, nullptr, 0},
    {0x10, 0x80, "xml:lang", VStr, nullptr, 0}, {0x11, 0x80, "xml:lang", VStr, nullptr, 0}, {0x12, 0x80, "xml:lang", VStr, nullptr, 0},
    {0x1A, 0x80, "xml:lang", VStr, nullptr, 0}, {0x1B, 0x80, "xml:lang", VStr, nullptr, 0},
    {0x2B, 0x80, "mimeValue", VStr, nullptr, 0}, {0x2B, 0x81, "xml:lang", VStr, nullptr, 0}, {0x2B, 0x82, "url", VStr, nullptr, 0}, {0x2B, 0x83, "type", VEnum, kEnMultimedia, 7},
    {0x2B, 0x84, "width", VU16, nullptr, 0}, {0x2B, 0x85, "height", VU16, nullptr, 0},
    {0x29, 0x80, "id", VBearer, nullptr, 0}, {0x29, 0x82, "url", VStr, nullptr, 0}, {0x2D, 0x80, "id", VBearer, nullptr, 0}, {0x2D, 0x82, "url", VStr, nullptr, 0},
    // service information
    {0x03, 0x80, "version", VU16, nullptr, 0}, {0x03, 0x81, "creationTime", VTime, nullptr, 0}, {0x03, 0x82, "originator", VStr, nullptr, 0}, {0x03, 0x83, "serviceProvider", VStr, nullptr, 0},
    {0x26, 0x80, "id", VEnsId, nullptr, 0}, {0x28, 0x80, "version", VU16, nullptr, 0},
    {0x31, 0x80, "fqdn", VStr, nullptr, 0}, {0x31, 0x81, "serviceIdentifier", VStr, nullptr, 0},
    // programme and group information
    {0x20, 0x80, "version", VU16, nullptr, 0}, {0x20, 0x81, "creationTime", VTime, nullptr, 0}, {0x20, 0x82, "originator", VStr, nullptr, 0},
    {0x23, 0x80, "id", VStr, nullptr, 0}, {0x23, 0x81, "shortId", VU24, nullptr, 0}, {0x23, 0x82, "version", VU16, nullptr, 0}, {0x23, 0x83, "type", VEnum, kEnGroupType, 10}, {0x23, 0x84, "numOfItems", VU16, nullptr, 0},
    {0x21, 0x80, "version", VU16, nullptr, 0}, {0x21, 0x81, "creationTime", VTime, nullptr, 0}, {0x21, 0x82, "originator", VStr, nullptr, 0},
    {0x24, 0x80, "startTime", VTime, nullptr, 0}, {0x24, 0x81, "stopTime", VTime, nullptr, 0},
    {0x25, 0x80, "id", VBearer, nullptr, 0},
    {0x1C, 0x80, "id", VStr, nullptr, 0}, {0x1C, 0x81, "shortId", VU24, nullptr, 0}, {0x1C, 0x82, "version", VU16, nullptr, 0}, {0x1C, 0x83, "recommendation", VEnum, kEnRecommend, 3},
    {0x1C, 0x84, "broadcast", VEnum, kEnBroadcast, 3}, {0x1C, 0x86, "xml:lang", VStr, nullptr, 0},
    {0x2E, 0x80, "id", VStr, nullptr, 0}, {0x2E, 0x81, "shortId", VU24, nullptr, 0}, {0x2E, 0x82, "version", VU16, nullptr, 0}, {0x2E, 0x83, "recommendation", VEnum, kEnRecommend, 3},
    {0x2E, 0x84, "broadcast", VEnum, kEnBroadcast, 3}, {0x2E, 0x86, "xml:lang", VStr, nullptr, 0},
    {0x2C, 0x80, "time", VTime, nullptr, 0}, {0x2C, 0x81, "duration", VDur, nullptr, 0}, {0x2C, 0x82, "actualTime", VTime, nullptr, 0}, {0x2C, 0x83, "actualDuration", VDur, nullptr, 0},
    {0x2F, 0x80, "time", VDur, nullptr, 0}, {0x2F, 0x81, "duration", VDur, nullptr, 0}, {0x2F, 0x82, "actualTime", VDur, nullptr, 0}, {0x2F, 0x83, "actualDuration", VDur, nullptr, 0},
    {0x17, 0x80, "id", VStr, nullptr, 0}, {0x17, 0x81, "shortId", VU24, nullptr, 0}, {0x17, 0x82, "index", VU16, nullptr, 0},
    {0x37, 0x80, "start", VTime, nullptr, 0}, {0x37, 0x81, "end", VTime, nullptr, 0}, {0x37, 0x82, "duration", VDur, nullptr, 0},
    {0x38, 0x80, "start", VTime, nullptr, 0}, {0x38, 0x81, "end", VTime, nullptr, 0},
};
const AttrDef* findAttr(int elem, int tag) {
    for (const AttrDef& a : kAttrs) if (a.elem == elem && a.tag == tag) return &a;
    return nullptr;
}

std::string timeValue(const uint8_t* d, size_t n, bool* ok) {
    *ok = false;
    if (n < 4) return "";
    const uint32_t w = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    const int mjd = (int)((w >> 14) & 0x1FFFF);
    const bool lto = (w >> 12) & 1, lng = (w >> 11) & 1;
    int hour, minute, second = 0;
    size_t need = 4;
    if (!lng) {
        hour = (int)((w >> 6) & 31); minute = (int)(w & 63);
    } else {
        if (n < 6) return "";
        hour = (int)((w >> 6) & 31); minute = (int)(w & 63);
        second = d[4] >> 2;     // 6 bits seconds, then 10 bits reserved (the rest of byte 4 and byte 5)
        need = 6;
    }
    int off = 0;
    if (lto) {
        if (n < need + 1) return "";
        const uint8_t b = d[need];
        off = (b & 31) * 30;
        if (b & 0x20) off = -off;
        need++;
    }
    if (hour > 23 || minute > 59 || second > 59) return "";
    *ok = true;
    return spiFormatTime(spiMjdToUtc(mjd, hour, minute, second), off);
}

std::string bearerValue(const uint8_t* d, size_t n) {
    // flags byte: Rfa, Ens flag, X-PAD flag, SId flag, SCIdS(4); ECC; EId(16); SId(16 or 32)
    if (n < 1) return "";
    const bool sid32 = d[0] & 0x10;
    const int scids = d[0] & 15;
    const size_t need = 1 + 1 + 2 + (sid32 ? 4 : 2);
    if (n < need) return "";
    const unsigned ecc = d[1], eid = ((unsigned)d[2] << 8) | d[3];
    uint32_t sid = sid32 ? ((uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7]) : ((uint32_t)d[4] << 8 | d[5]);
    return "dab:" + hex(eid >> 12) + hex(ecc, 2) + "." + hex(eid, 4) + "." + hex(sid, sid32 ? 8 : 4) + "." + hex((unsigned)scids);
}

struct BinCtx {
    std::string tokens[256];
    bool haveTokens = false;
    std::string defLang;
    size_t nodes = 0;
};

std::string expand(const BinCtx& c, const uint8_t* d, size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) {
        if (c.haveTokens && d[i] < 0x14 && !c.tokens[d[i]].empty()) s += c.tokens[d[i]];
        else s += (char)d[i];
    }
    return s;
}

// reads tag + length at p; returns false when truncated. hdr = bytes used by tag and length.
bool readTL(const uint8_t* d, size_t end, size_t p, int& tag, size_t& len, size_t& hdr) {
    if (p + 2 > end) return false;
    tag = d[p];
    len = d[p + 1];
    hdr = 2;
    if (len == 0xFE) { if (p + 4 > end) return false; len = ((size_t)d[p + 2] << 8) | d[p + 3]; hdr = 4; }
    else if (len == 0xFF) { if (p + 5 > end) return false; len = ((size_t)d[p + 2] << 16) | ((size_t)d[p + 3] << 8) | d[p + 4]; hdr = 5; }
    return p + hdr + len <= end;
}

bool parseElement(BinCtx& c, const uint8_t* d, size_t start, size_t end, int depth, bool top, SpiNode& out, size_t& next) {
    int tag; size_t len, hdr;
    if (depth > kMaxDepth || ++c.nodes > kMaxNodes) return false;
    if (!readTL(d, end, start, tag, len, hdr)) return false;
    const char* nm = elemName(tag);
    next = start + hdr + len;
    if (!nm) { out.name.clear(); return true; }       // an element the specification does not define: skipped
    out.name = nm;
    size_t p = start + hdr;
    const size_t e = start + hdr + len;
    while (p < e) {
        const int t = d[p];
        int tg; size_t l, h;
        if (!readTL(d, e, p, tg, l, h)) return false;
        if (t >= 0x80) {                                     // attribute
            const AttrDef* ad = findAttr(tag, t);
            const uint8_t* v = d + p + h;
            if (ad) {
                std::string val;
                bool ok = true;
                switch (ad->type) {
                case VStr: val = expand(c, v, l); break;
                case VTime: val = timeValue(v, l, &ok); break;
                case VDur: if (l >= 2) val = "PT" + std::to_string(((unsigned)v[0] << 8) | v[1]) + "S"; else ok = false; break;
                case VBearer: val = bearerValue(v, l); ok = !val.empty(); break;
                case VEnsId: if (l >= 3) val = hex(v[0], 2) + "." + hex(((unsigned)v[1] << 8) | v[2], 4); else ok = false; break;
                case VGenre: {
                    if (l < 1) { ok = false; break; }
                    const unsigned cs = v[0] & 15;
                    val = std::to_string(cs);
                    for (size_t i = 1; i < l && i < 4; i++) val += "." + std::to_string(v[i]);
                    break;
                }
                case VU16: if (l >= 2) val = std::to_string(((unsigned)v[0] << 8) | v[1]); else ok = false; break;
                case VU24: if (l >= 3) val = std::to_string(((unsigned)v[0] << 16) | ((unsigned)v[1] << 8) | v[2]); else ok = false; break;
                case VEnum: {
                    const char* e = (l >= 1 && ad->enums && v[0] < ad->ne) ? ad->enums[v[0]] : nullptr;
                    if (e) val = e; else ok = false;
                    break;
                }
                }
                if (ok) out.attr[ad->name] = val;
            }
            p += h + l;
            continue;
        }
        if (t == 0x01) { out.text += expand(c, d + p + h, l); p += h + l; continue; }   // CDATA
        if (t == 0x04) {                                                                  // string token table (top level only)
            if (top) {
                size_t q = p + h;
                const size_t te = p + h + l;
                while (q + 2 <= te) {
                    const int tk = d[q];
                    const size_t tl = d[q + 1];
                    if (q + 2 + tl > te) break;
                    c.tokens[tk & 255].assign((const char*)d + q + 2, tl);
                    c.haveTokens = true;
                    q += 2 + tl;
                }
            }
            p += h + l;
            continue;
        }
        if (t == 0x06) { if (top) { c.defLang.assign((const char*)d + p + h, l); out.attr["xml:lang"] = c.defLang; } p += h + l; continue; }   // default language
        if (t < 0x10 || t > 0x7E) { p += h + l; continue; }
        SpiNode k;
        size_t nx;
        if (!parseElement(c, d, p, e, depth + 1, false, k, nx)) return false;
        if (!k.name.empty()) out.kids.push_back(std::move(k));
        p = nx;
    }
    return true;
}
}  // namespace

bool spiParseBinary(const uint8_t* d, size_t n, SpiNode& top) {
    top = SpiNode();
    if (n < 2 || n > (32u << 20)) return false;
    if (d[0] != 0x02 && d[0] != 0x03) return false;
    BinCtx c;
    size_t next;
    SpiNode t;
    if (!parseElement(c, d, 0, n, 0, true, t, next)) return false;
    if (t.name.empty()) return false;
    top = std::move(t);
    return true;
}

// ---------------------------------------------------------------- XML
namespace {
std::string decodeEntities(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        const size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) { o += '?'; }
            else if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}
std::string localName(const std::string& n) {
    const size_t c = n.find(':');
    return c == std::string::npos || n.compare(0, 4, "xml:") == 0 ? n : n.substr(c + 1);
}

struct XmlParser {
    const char* d; size_t n, p = 0; size_t nodes = 0;
    bool skipTo(const char* s) {
        const size_t l = strlen(s);
        for (; p + l <= n; p++) if (memcmp(d + p, s, l) == 0) { p += l; return true; }
        p = n;
        return false;
    }
    void ws() { while (p < n && std::isspace((unsigned char)d[p])) p++; }
    bool element(SpiNode& out, int depth) {
        if (depth > kMaxDepth || ++nodes > kMaxNodes) return false;
        // at '<' of a start tag
        p++;
        size_t s = p;
        while (p < n && !std::isspace((unsigned char)d[p]) && d[p] != '>' && d[p] != '/') p++;
        out.name = localName(std::string(d + s, p - s));
        bool empty = false;
        for (;;) {
            ws();
            if (p >= n) return false;
            if (d[p] == '>') { p++; break; }
            if (d[p] == '/') { if (p + 1 < n && d[p + 1] == '>') { p += 2; empty = true; break; } return false; }
            s = p;
            while (p < n && d[p] != '=' && !std::isspace((unsigned char)d[p]) && d[p] != '>' && d[p] != '/') p++;
            const std::string an = std::string(d + s, p - s);
            ws();
            if (p >= n || d[p] != '=') return false;
            p++; ws();
            if (p >= n || (d[p] != '"' && d[p] != '\'')) return false;
            const char q = d[p++];
            s = p;
            while (p < n && d[p] != q) p++;
            if (p >= n) return false;
            const std::string av = decodeEntities(std::string(d + s, p - s));
            p++;
            if (an.compare(0, 5, "xmlns") != 0 && an.compare(0, 4, "xsi:") != 0) out.attr[localName(an)] = av;
        }
        if (empty) return true;
        for (;;) {
            if (p >= n) return false;
            if (d[p] != '<') {
                const size_t t = p;
                while (p < n && d[p] != '<') p++;
                out.text += decodeEntities(std::string(d + t, p - t));
                continue;
            }
            if (p + 1 < n && d[p + 1] == '/') {
                skipTo(">");
                return true;
            }
            if (p + 3 < n && memcmp(d + p, "<!--", 4) == 0) { p += 4; skipTo("-->"); continue; }
            if (p + 8 < n && memcmp(d + p, "<![CDATA[", 9) == 0) {
                p += 9;
                const size_t t = p;
                if (!skipTo("]]>")) return false;
                out.text.append(d + t, p - 3 - t);
                continue;
            }
            if (p + 1 < n && (d[p + 1] == '?' || d[p + 1] == '!')) { skipTo(">"); continue; }
            SpiNode k;
            if (!element(k, depth + 1)) return false;
            out.kids.push_back(std::move(k));
        }
    }
};
}  // namespace

bool spiParseXml(const char* d, size_t n, SpiNode& top) {
    top = SpiNode();
    if (n < 4 || n > (32u << 20)) return false;
    XmlParser x{d, n};
    if (n >= 3 && (uint8_t)d[0] == 0xEF && (uint8_t)d[1] == 0xBB && (uint8_t)d[2] == 0xBF) x.p = 3;
    for (;;) {
        x.ws();
        if (x.p >= n) return false;
        if (d[x.p] != '<') return false;
        if (x.p + 3 < n && memcmp(d + x.p, "<!--", 4) == 0) { x.p += 4; x.skipTo("-->"); continue; }
        if (x.p + 1 < n && (d[x.p + 1] == '?' || d[x.p + 1] == '!')) { x.skipTo(">"); continue; }
        break;
    }
    SpiNode t;
    if (!x.element(t, 0)) return false;
    top = std::move(t);
    return true;
}

// ---------------------------------------------------------------- model
std::string spiGenreCode(const std::string& href) {
    if (href.empty()) return "";
    // the numbers after the last ':' of the URN, or the whole string when it is a dotted number
    const size_t c = href.rfind(':');
    const std::string t = c == std::string::npos ? href : href.substr(c + 1);
    if (t.empty()) return "";
    for (char ch : t) if (!std::isdigit((unsigned char)ch) && ch != '.') return "";
    return t;
}

namespace {
struct Bearer { bool ok = false; int ecc = -1, eid = -1; uint32_t sid = 0; int scids = 0; };
Bearer parseBearer(const std::string& s0) {
    Bearer b;
    std::string s = s0;
    if (s.compare(0, 4, "dab:") == 0) s.erase(0, 4);
    else if (s.find(':') != std::string::npos) return b;       // another domain
    std::vector<std::string> seg;
    size_t st = 0;
    for (size_t i = 0; i <= s.size(); i++) if (i == s.size() || s[i] == '.') { seg.push_back(s.substr(st, i - st)); st = i + 1; }
    if (seg.size() < 3) return b;
    auto hx = [](const std::string& t, unsigned long& v) {
        if (t.empty() || t.size() > 8) return false;
        char* e = nullptr;
        v = strtoul(t.c_str(), &e, 16);
        return e && !*e;
    };
    unsigned long g, e, sid, sc = 0;
    if (!hx(seg[0], g) || !hx(seg[1], e) || !hx(seg[2], sid)) return b;
    if (seg.size() > 3) hx(seg[3], sc);
    b.ok = true;
    b.ecc = (int)(g & 0xFF);
    b.eid = (int)(e & 0xFFFF);
    b.sid = (uint32_t)sid;
    b.scids = (int)sc;
    return b;
}

const std::string& langOf(const SpiNode& n, const std::string& inherited) {
    const std::string& l = n.get("xml:lang");
    return l.empty() ? inherited : l;
}

void fillNames(const SpiNode& n, std::string& sh, std::string& md, std::string& lg, std::string& sd, std::string& ld) {
    for (const SpiNode& k : n.kids) {
        if (k.name == "mediaDescription") { fillNames(k, sh, md, lg, sd, ld); continue; }   // the descriptions are children of mediaDescription
        std::string* t = k.name == "shortName" ? &sh : k.name == "mediumName" ? &md : k.name == "longName" ? &lg : k.name == "shortDescription" ? &sd : k.name == "longDescription" ? &ld : nullptr;
        if (t && t->empty()) *t = k.text;
    }
}
void collectMedia(const SpiNode& n, std::vector<SpiMedia>& out) {
    for (const SpiNode& k : n.kids) {
        if (k.name == "mediaDescription") { collectMedia(k, out); continue; }
        if (k.name != "multimedia") continue;
        SpiMedia m;
        m.type = k.get("type"); m.url = k.get("url"); m.mime = k.get("mimeValue");
        m.w = atoi(k.get("width").c_str()); m.h = atoi(k.get("height").c_str());
        if (m.url.empty()) continue;
        bool dup = false;
        for (const SpiMedia& o : out) if (o.url == m.url && o.type == m.type) dup = true;
        if (!dup) out.push_back(m);
    }
}
void mergeStr(std::string& dst, const std::string& src) { if (dst.empty() && !src.empty()) dst = src; }
}  // namespace

void SpiStore::handleSi(const SpiNode& top) {
    std::vector<const SpiNode*> svc;
    int ecc = s_.ecc, eid = s_.eid;
    std::vector<const SpiNode*> stack{&top};
    while (!stack.empty()) {
        const SpiNode* n = stack.back();
        stack.pop_back();
        for (const SpiNode& k : n->kids) {
            if (k.name == "ensemble") {
                const std::string& id = k.get("id");
                unsigned e1, e2;
                if (sscanf(id.c_str(), "%x.%x", &e1, &e2) == 2) { ecc = (int)e1; eid = (int)e2; }
                stack.push_back(&k);
            } else if (k.name == "service") svc.push_back(&k);
            else if (k.name == "services") stack.push_back(&k);
        }
    }
    s_.ecc = ecc; s_.eid = eid;
    for (const SpiNode* s : svc) {
        std::vector<Bearer> bs;
        for (const SpiNode& k : s->kids) if (k.name == "bearer" || k.name == "serviceID") { Bearer b = parseBearer(k.get("id")); if (b.ok) bs.push_back(b); }
        if (bs.empty()) continue;
        SpiService sv;
        fillNames(*s, sv.shortName, sv.mediumName, sv.longName, sv.description, sv.description);
        collectMedia(*s, sv.media);
        for (const SpiNode& k : s->kids) if (k.name == "genre") { sv.genre = spiGenreCode(k.get("href")); break; }
        for (const Bearer& b : bs) {
            SpiService x = sv;
            x.sid = b.sid; x.ecc = b.ecc; x.eid = b.eid;
            SpiService& cur = s_.services[b.sid];
            if (cur.sid == 0) cur = x;
            else {
                mergeStr(cur.shortName, x.shortName); mergeStr(cur.mediumName, x.mediumName); mergeStr(cur.longName, x.longName); mergeStr(cur.description, x.description); mergeStr(cur.genre, x.genre);
                for (const SpiMedia& m : x.media) { bool dup = false; for (const SpiMedia& o : cur.media) if (o.url == m.url && o.type == m.type) dup = true; if (!dup) cur.media.push_back(m); }
                if (cur.ecc < 0) cur.ecc = x.ecc;
                if (cur.eid < 0) cur.eid = x.eid;
            }
        }
    }
}

namespace {
void programmeFrom(const SpiNode& p, SpiProgramme& pr, const std::string& defLang) {
    pr.crid = p.get("id");
    pr.shortId = (uint32_t)strtoul(p.get("shortId").c_str(), nullptr, 10);
    pr.version = atoi(p.get("version").c_str());
    pr.recommended = p.get("recommendation") == "yes";
    pr.lang = langOf(p, defLang);
    fillNames(p, pr.shortName, pr.mediumName, pr.longName, pr.shortDesc, pr.longDesc);
    collectMedia(p, pr.media);
    for (const SpiNode& k : p.kids) {
        if (k.name == "genre") { const std::string g = spiGenreCode(k.get("href")); const std::string v = g.empty() ? k.get("href") : g; if (!v.empty() && std::find(pr.genres.begin(), pr.genres.end(), v) == pr.genres.end()) pr.genres.push_back(v); }
        else if (k.name == "link" && !k.get("uri").empty()) pr.links.push_back(k.get("uri"));
        else if (k.name == "memberOf" && !k.get("id").empty()) pr.groups.push_back(k.get("id"));
    }
    if (pr.shortId) pr.key = "s" + std::to_string(pr.shortId);
    else if (!pr.crid.empty()) pr.key = "c" + pr.crid;
}
}  // namespace

void SpiStore::handleEpg(const SpiNode& top) {
    const std::string defLang = top.get("xml:lang");
    for (const SpiNode& sec : top.kids) {
        if (sec.name == "programmeGroups") {
            for (const SpiNode& g : sec.kids) {
                if (g.name != "programmeGroup") continue;
                SpiGroup gr;
                gr.crid = g.get("id"); gr.type = g.get("type"); gr.numOfItems = atoi(g.get("numOfItems").c_str());
                std::string a, b, c, d, e;
                fillNames(g, a, b, c, d, e);
                gr.name = !b.empty() ? b : !c.empty() ? c : a;
                gr.desc = !e.empty() ? e : d;
                if (gr.crid.empty()) continue;
                bool found = false;
                for (SpiGroup& o : s_.groups) if (o.crid == gr.crid) { o = gr; found = true; }
                if (!found && s_.groups.size() < 5000) s_.groups.push_back(gr);
            }
            continue;
        }
        if (sec.name != "schedule") continue;
        std::vector<uint32_t> scopeSids;
        for (const SpiNode& sc : sec.kids) {
            if (sc.name != "scope") continue;
            for (const SpiNode& ss : sc.kids) if (ss.name == "serviceScope") { const Bearer b = parseBearer(ss.get("id")); if (b.ok) scopeSids.push_back(b.sid); }
        }
        for (const SpiNode& p : sec.kids) {
            if (p.name != "programme" && p.name != "programmeEvent") continue;
            SpiProgramme pr;
            programmeFrom(p, pr, defLang);
            if (pr.key.empty()) continue;
            if (s_.programmes.size() < 20000 || s_.programmes.count(pr.key)) {
                SpiProgramme& cur = s_.programmes[pr.key];
                if (cur.key.empty()) cur = pr;
                else {
                    mergeStr(cur.crid, pr.crid); mergeStr(cur.shortName, pr.shortName); mergeStr(cur.mediumName, pr.mediumName); mergeStr(cur.longName, pr.longName);
                    mergeStr(cur.shortDesc, pr.shortDesc); mergeStr(cur.longDesc, pr.longDesc); mergeStr(cur.lang, pr.lang);
                    if (pr.version > cur.version) cur.version = pr.version;
                    for (const std::string& g : pr.genres) if (std::find(cur.genres.begin(), cur.genres.end(), g) == cur.genres.end()) cur.genres.push_back(g);
                    for (const std::string& g : pr.links) if (std::find(cur.links.begin(), cur.links.end(), g) == cur.links.end()) cur.links.push_back(g);
                    for (const std::string& g : pr.groups) if (std::find(cur.groups.begin(), cur.groups.end(), g) == cur.groups.end()) cur.groups.push_back(g);
                    for (const SpiMedia& m : pr.media) { bool dup = false; for (const SpiMedia& o : cur.media) if (o.url == m.url && o.type == m.type) dup = true; if (!dup) cur.media.push_back(m); }
                    cur.recommended = cur.recommended || pr.recommended;
                }
            }
            // the locations: where and when
            for (const SpiNode& loc : p.kids) {
                if (loc.name != "location") continue;
                std::vector<uint32_t> sids;
                for (const SpiNode& b : loc.kids) if (b.name == "bearer") { const Bearer bb = parseBearer(b.get("id")); if (bb.ok) sids.push_back(bb.sid); }
                if (sids.empty()) sids = scopeSids;
                for (const SpiNode& t : loc.kids) {
                    if (t.name != "time") continue;
                    int64_t utc; int off;
                    if (!spiParseTime(t.get("time"), utc, off)) continue;
                    int dur = 0;
                    spiParseDuration(t.get("duration"), dur);
                    for (uint32_t sid : sids) {
                        SpiEvent ev;
                        ev.sid = sid; ev.start = utc; ev.duration = dur; ev.offMin = off; ev.progKey = pr.key;
                        // one programme per service at a time: a later version of the same slot replaces the earlier
                        auto it = ev_.find(ev);
                        if (it != ev_.end()) ev_.erase(it);
                        if (ev_.size() < 60000) ev_.insert(ev);
                    }
                }
            }
        }
    }
}

bool SpiStore::addTree(const SpiNode& top, const MotObject*) {
    std::lock_guard<std::mutex> lk(mu_);
    if (top.name == "epg") { handleEpg(top); st_.piObjects++; }
    else if (top.name == "serviceInformation") { handleSi(top); st_.siObjects++; }
    else return false;
    s_.events.assign(ev_.begin(), ev_.end());
    st_.events = ev_.size();
    seq_++;
    return true;
}

bool SpiStore::addObject(const MotObject& o) {
    // content type 7 (application): sub type 0 service information, 1 programme information, 2 group information; or plain XML
    std::vector<uint8_t> unz;
    const uint8_t* d = o.body.data();
    size_t n = o.body.size();
    if (n < 4) return false;
    bool compressed = false;
    for (const MotParam& p : o.params) if (p.id == 0x11 && !p.data.empty() && p.data[0] == 1) compressed = true;   // CompressionType: gzip
    if (!compressed && n > 2 && d[0] == 0x1F && d[1] == 0x8B) compressed = true;
    if (compressed) {
        if (!gunzip(d, n, unz, 8u << 20)) { std::lock_guard<std::mutex> lk(mu_); st_.objectsBad++; return false; }
        d = unz.data(); n = unz.size();
    }
    SpiNode top;
    bool xml = false;
    size_t k = 0;
    while (k < n && std::isspace(d[k])) k++;
    if (k < n && (d[k] == '<' || (k + 2 < n && d[k] == 0xEF))) xml = true;
    bool ok;
    if (xml) ok = spiParseXml((const char*)d, n, top);
    else {
        if (o.contentType != 7 && !(d[0] == 0x02 || d[0] == 0x03)) return false;
        ok = spiParseBinary(d, n, top);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.objects++;
        if (!ok) { st_.objectsBad++; return false; }
        if (compressed) st_.compressed++;
        if (xml) st_.xml++; else st_.binary++;
    }
    return addTree(top, &o);
}

void SpiStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    s_ = SpiSnapshot();
    ev_.clear();
    st_ = SpiStats();
    seq_++;
}
uint64_t SpiStore::seq() const { std::lock_guard<std::mutex> lk(mu_); return seq_; }
SpiStats SpiStore::stats() const { std::lock_guard<std::mutex> lk(mu_); return st_; }
SpiSnapshot SpiStore::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    SpiSnapshot s = s_;
    s.seq = seq_;
    return s;
}

void SpiStore::prune(int64_t utcNow, int keepSec) {
    std::lock_guard<std::mutex> lk(mu_);
    bool changed = false;
    for (auto it = ev_.begin(); it != ev_.end();) {
        const int64_t end = it->start + (it->duration > 0 ? it->duration : 0);
        if (end < utcNow - keepSec) { it = ev_.erase(it); changed = true; } else ++it;
    }
    if (changed) {
        std::set<std::string> used;
        for (const SpiEvent& e : ev_) used.insert(e.progKey);
        for (auto it = s_.programmes.begin(); it != s_.programmes.end();) { if (!used.count(it->first)) it = s_.programmes.erase(it); else ++it; }
        s_.events.assign(ev_.begin(), ev_.end());
        st_.events = ev_.size();
        seq_++;
    }
}

SpiNowNext SpiStore::nowNext(uint32_t sid, int64_t utcNow) const {
    std::lock_guard<std::mutex> lk(mu_);
    SpiNowNext r;
    SpiEvent lo; lo.sid = sid; lo.start = INT64_MIN / 2;
    for (auto it = ev_.lower_bound(lo); it != ev_.end() && it->sid == sid; ++it) {
        const SpiEvent& e = *it;
        auto nx = std::next(it);
        const bool haveNx = nx != ev_.end() && nx->sid == sid;
        const int64_t end = e.duration > 0 ? e.start + e.duration : (haveNx ? nx->start : e.start);
        if (e.start <= utcNow && utcNow < end) { r.haveNow = true; r.now = e; if (haveNx) { r.haveNext = true; r.next = *nx; } return r; }
        if (e.start > utcNow) { r.haveNext = true; r.next = e; return r; }
    }
    return r;
}

std::vector<SpiEvent> SpiStore::upcoming(uint32_t sid, int64_t from, size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SpiEvent> out;
    SpiEvent lo; lo.sid = sid; lo.start = INT64_MIN / 2;
    for (auto it = ev_.lower_bound(lo); it != ev_.end() && it->sid == sid && out.size() < max; ++it) {
        const int64_t end = it->duration > 0 ? it->start + it->duration : it->start;
        if (end >= from) out.push_back(*it);
    }
    return out;
}

}  // namespace dect2
