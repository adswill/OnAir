// A/65 tables: see psip.h.
#include "dect2/tv/psip.h"
#include "dect2/tv/ts_proto.h"
#include <cstring>

namespace dect2 {
namespace psip {

namespace {
bool crcOk(const uint8_t* s, int len) {
    if (len < 12) return false;
    return tsp::crc32(s, (size_t)len) == 0;
}
void utf8(std::string& o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
    else { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
}
}  // namespace

std::string mss(const uint8_t* p, int len, bool* compressed) {
    if (compressed) *compressed = false;
    std::string out;
    if (len < 1) return out;
    int nStr = p[0], i = 1;
    if (nStr < 1) return out;
    if (i + 4 > len) return out;
    int nSeg = p[i + 3]; i += 4;
    for (int s = 0; s < nSeg && i + 3 <= len; s++) {
        const int comp = p[i], mode = p[i + 1], n = p[i + 2]; i += 3;
        if (i + n > len) break;
        if (comp != 0) { if (compressed) *compressed = true; }
        else if (mode == 0x3F) { for (int k = 0; k + 1 < n; k += 2) utf8(out, (uint32_t)p[i + k] << 8 | p[i + k + 1]); }
        else if (mode <= 0x33) { for (int k = 0; k < n; k++) utf8(out, (uint32_t)mode << 8 | p[i + k]); }  // mode: the upper byte of the Unicode block (0 = Latin-1)
        i += n;
    }
    return out;
}

bool parseMgt(const uint8_t* s, int len, State& st) {
    if (len < 14 || s[0] != 0xC7 || !crcOk(s, len)) return false;
    int n = s[9] << 8 | s[10], i = 11;
    for (int k = 0; k < n; k++) {
        if (i + 11 > len - 4) return false;
        const int type = s[i] << 8 | s[i + 1], pid = (s[i + 2] & 0x1F) << 8 | s[i + 3], dl = (s[i + 9] & 0x0F) << 8 | s[i + 10];
        if (type >= 0x0100 && type <= 0x017F) st.eitPids.insert(pid);
        else if (type >= 0x0200 && type <= 0x027F) st.ettPids.insert(pid);
        i += 11 + dl;
    }
    st.mgt++;
    return true;
}

bool parseVctSources(const uint8_t* s, int len, State& st) {
    if (len < 16 || (s[0] != 0xC8 && s[0] != 0xC9) || !crcOk(s, len)) return false;
    int n = s[9], i = 10;
    for (int k = 0; k < n; k++) {
        if (i + 32 > len - 4) return false;
        const int prog = s[i + 24] << 8 | s[i + 25], src = s[i + 28] << 8 | s[i + 29], dl = (s[i + 30] & 0x03) << 8 | s[i + 31];
        st.sourceToProgram[src] = prog;
        i += 32 + dl;
    }
    return true;
}

bool parseEit(const uint8_t* s, int len, State& st, std::vector<Event>& out) {
    if (len < 14 || s[0] != 0xCB || !crcOk(s, len)) return false;
    const int src = s[3] << 8 | s[4], n = s[9];
    int i = 10;
    for (int k = 0; k < n; k++) {
        if (i + 10 > len - 4) return false;
        Event e; e.sourceId = src;
        e.eventId = (s[i] & 0x3F) << 8 | s[i + 1];
        const int64_t gps = (int64_t)((uint32_t)s[i + 2] << 24 | s[i + 3] << 16 | s[i + 4] << 8 | s[i + 5]);
        e.start = gpsToUtc(gps, st.gpsUtcOffset);
        e.duration = (s[i + 6] & 0x0F) << 16 | s[i + 7] << 8 | s[i + 8];
        const int tl = s[i + 9];
        if (i + 10 + tl + 2 > len - 4) return false;
        e.title = mss(s + i + 10, tl, &e.compressed);
        if (e.compressed) st.compressedTitles++;
        const int dpos = i + 10 + tl;
        const int dl = (s[dpos] & 0x0F) << 8 | s[dpos + 1];
        i = dpos + 2 + dl;
        out.push_back(std::move(e));
    }
    st.eit++;
    return true;
}

bool parseEtt(const uint8_t* s, int len, State& st, uint32_t* id, std::string* text) {
    if (len < 18 || s[0] != 0xCC || !crcOk(s, len)) return false;
    const uint32_t etm = (uint32_t)s[9] << 24 | s[10] << 16 | s[11] << 8 | s[12];
    bool comp = false;
    std::string t = mss(s + 13, len - 13 - 4, &comp);
    st.ett[etm] = t; st.ettCount++;
    if (id) *id = etm;
    if (text) *text = t;
    return true;
}

bool parseStt(const uint8_t* s, int len, State& st) {
    if (len < 17 || s[0] != 0xCD || !crcOk(s, len)) return false;
    const int64_t gps = (int64_t)((uint32_t)s[9] << 24 | s[10] << 16 | s[11] << 8 | s[12]);
    st.gpsUtcOffset = s[13];
    st.systemTimeUtc = gpsToUtc(gps, st.gpsUtcOffset);
    st.stt++;
    return true;
}

}  // namespace psip
}  // namespace dect2
