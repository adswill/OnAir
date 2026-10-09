// CDR multiplex, GY/T 268.2-2013 (see cdr_mux.h). Reserved and padding bits are ones (4.3).
#include "dect2/cdr_mux.h"
#include "dect2/cdr_defs.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2::cdr {

namespace {

class BitWriter {
public:
    void put(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if ((bits_ & 7) == 0) b_.push_back(0);
            if ((v >> i) & 1u) b_.back() |= (uint8_t)(0x80u >> (bits_ & 7));
            bits_++;
        }
    }
    void bytes(const std::vector<uint8_t>& v) { for (uint8_t x : v) put(x, 8); }
    std::vector<uint8_t>& data() { return b_; }
private:
    std::vector<uint8_t> b_;
    size_t bits_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    uint64_t get(int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; i++) {
            if (pos_ >= n_ * 8) { ok_ = false; return 0; }
            v = (v << 1) | ((d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u);
            pos_++;
        }
        return v;
    }
    bool ok() const { return ok_; }
    size_t bytePos() const { return pos_ >> 3; }
private:
    const uint8_t* d_;
    size_t n_, pos_ = 0;
    bool ok_ = true;
};

void appendCrc32(std::vector<uint8_t>& v) {
    const uint32_t c = crc32(v.data(), v.size());
    for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(c >> (8 * i)));
}
bool crc32Ok(const uint8_t* d, size_t n) {   // n bytes of data followed by their CRC
    const uint32_t c = crc32(d, n);
    return d[n] == (uint8_t)(c >> 24) && d[n + 1] == (uint8_t)(c >> 16) && d[n + 2] == (uint8_t)(c >> 8) && d[n + 3] == (uint8_t)c;
}
void setLen16(std::vector<uint8_t>& v, size_t at, size_t len) { v[at] = (uint8_t)(len >> 8); v[at + 1] = (uint8_t)len; }

} // namespace

// ---------------------------------------------------------------- clause 6

std::vector<uint8_t> smctBytes(const Smct& t) {
    BitWriter w;
    w.put(kTableSmct, 8); w.put(0, 16);
    w.put((uint64_t)t.segNo, 4); w.put((uint64_t)t.segCount, 4); w.put((uint64_t)t.update, 4); w.put(0x3F, 6);
    w.put(t.frames.size(), 6);
    for (const auto& f : t.frames) {
        w.put((uint64_t)f.smfId, 6); w.put(f.hier, 1); w.put(f.high, 1); w.put((uint64_t)f.txMode, 4); w.put(f.services.size(), 4);
        for (uint16_t s : f.services) w.put(s, 16);
        w.put(0xFFFF, 16);
    }
    std::vector<uint8_t> v = w.data();
    setLen16(v, 1, v.size());            // all fields but the CRC
    appendCrc32(v);
    return v;
}

std::vector<uint8_t> nitBytes(const Nit& t) {
    BitWriter w;
    w.put(kTableNit, 8); w.put(0, 16);
    w.put((uint64_t)t.segNo, 4); w.put((uint64_t)t.segCount, 4); w.put((uint64_t)t.update, 4); w.put(0xF, 4);
    if (t.segNo == 0) {
        for (int i = 0; i < 3; i++) w.put(i < (int)t.country.size() ? (uint8_t)t.country[(size_t)i] : ' ', 8);
        w.put(t.networkId & 0xFFFFFFFFFull, 36);
        w.put(t.freqs.size(), 12);
        for (uint32_t f : t.freqs) w.put(f, 32);
        w.put(t.name.size(), 8);
        w.bytes(t.name);
    }
    w.put(t.neighbours.size(), 6); w.put(3, 2);
    for (const auto& n : t.neighbours) {
        w.put(n.id & 0xFFFFFFFFFull, 36); w.put(n.freqs.size(), 4);
        for (uint32_t f : n.freqs) w.put(f, 32);
        w.put(0xFFFF, 16);
    }
    std::vector<uint8_t> v = w.data();
    setLen16(v, 1, v.size());
    appendCrc32(v);
    return v;
}

std::vector<uint8_t> controlFrameBytes(const std::vector<std::vector<uint8_t>>& tables) {
    BitWriter w;
    const size_t hdr = 2 + 2 * tables.size();     // 10 + 6 + 16 N bits
    w.put(hdr, 10); w.put(tables.size(), 6);
    for (const auto& t : tables) w.put(t.size(), 16);
    std::vector<uint8_t> v = w.data();
    v.push_back(crc8(v.data(), v.size()));
    for (const auto& t : tables) v.insert(v.end(), t.begin(), t.end());
    return v;
}

bool parseSmct(const uint8_t* d, size_t n, Smct& out) {
    if (n < 10 || d[0] != kTableSmct) return false;
    const size_t len = ((size_t)d[1] << 8) | d[2];
    if (len < 6 || len + 4 > n || !crc32Ok(d, len)) return false;
    BitReader r(d, len);
    r.get(24);
    out = Smct();
    out.segNo = (int)r.get(4); out.segCount = (int)r.get(4); out.update = (int)r.get(4); r.get(6);
    const int m1 = (int)r.get(6);
    for (int i = 0; i < m1 && r.ok(); i++) {
        SmctEntry e;
        e.smfId = (int)r.get(6); e.hier = r.get(1); e.high = r.get(1); e.txMode = (int)r.get(4);
        const int m2 = (int)r.get(4);
        for (int j = 0; j < m2; j++) e.services.push_back((uint16_t)r.get(16));
        r.get(16);
        if (r.ok()) out.frames.push_back(e);
    }
    return r.ok();
}

bool parseNit(const uint8_t* d, size_t n, Nit& out) {
    if (n < 9 || d[0] != kTableNit) return false;
    const size_t len = ((size_t)d[1] << 8) | d[2];
    if (len < 5 || len + 4 > n || !crc32Ok(d, len)) return false;
    BitReader r(d, len);
    r.get(24);
    out = Nit();
    out.segNo = (int)r.get(4); out.segCount = (int)r.get(4); out.update = (int)r.get(4); r.get(4);
    if (out.segNo == 0) {
        out.country.clear();
        for (int i = 0; i < 3; i++) out.country.push_back((char)r.get(8));
        out.networkId = r.get(36);
        const int n1 = (int)r.get(12);
        for (int i = 0; i < n1 && r.ok(); i++) out.freqs.push_back((uint32_t)r.get(32));
        const int n2 = (int)r.get(8);
        for (int i = 0; i < n2 && r.ok(); i++) out.name.push_back((uint8_t)r.get(8));
    }
    const int n3 = (int)r.get(6);
    r.get(2);
    for (int i = 0; i < n3 && r.ok(); i++) {
        NitNeighbour nb;
        nb.id = r.get(36);
        const int n4 = (int)r.get(4);
        for (int j = 0; j < n4; j++) nb.freqs.push_back((uint32_t)r.get(32));
        r.get(16);
        if (r.ok()) out.neighbours.push_back(nb);
    }
    return r.ok();
}

bool parseControlFrame(const uint8_t* d, size_t n, ControlFrame& out) {
    out = ControlFrame();
    if (n < 3) return false;
    BitReader r(d, n);
    const size_t hdr = (size_t)r.get(10);
    const int count = (int)r.get(6);
    if (hdr != 2 + 2 * (size_t)count || hdr + 1 > n) return false;
    if (crc8(d, hdr) != d[hdr]) return false;
    for (int i = 0; i < count; i++) out.tableLens.push_back((int)r.get(16));
    out.headerOk = true;
    size_t at = hdr + 1;
    for (int len : out.tableLens) {
        if (at + (size_t)len > n || len < 1) { out.tablesBad++; break; }
        const uint8_t* t = d + at;
        if (t[0] == kTableSmct) {
            Smct s;
            if (parseSmct(t, (size_t)len, s)) { out.smct = s; out.haveSmct = true; out.tablesOk++; } else out.tablesBad++;
        } else if (t[0] == kTableNit) {
            Nit s;
            if (parseNit(t, (size_t)len, s)) { out.nit = s; out.haveNit = true; out.tablesOk++; } else out.tablesBad++;
        } else out.otherTables.push_back(t[0]);
        at += (size_t)len;
    }
    return true;
}

// ---------------------------------------------------------------- clause 7

std::vector<uint8_t> subFrameBytes(const MuxSubFrame& s) {
    const bool audio = s.hasAudio, data = s.hasData, ext = s.hasExt && audio;
    // the sections first: their lengths go into the header
    std::vector<uint8_t> aSec, dSec;
    if (audio) {
        BitWriter w;
        w.put(s.audio.size(), 8);
        for (const auto& u : s.audio) { w.put(u.data.size(), 16); w.put((uint64_t)u.stream, 3); w.put(0x1F, 5); w.put((uint64_t)u.relTime, 16); }
        aSec = w.data();
        appendCrc32(aSec);
        for (const auto& u : s.audio) aSec.insert(aSec.end(), u.data.begin(), u.data.end());
    }
    if (data) {
        BitWriter w;
        w.put(s.data.size(), 8);
        for (const auto& u : s.data) { w.put((uint64_t)u.type, 8); w.put(u.data.size(), 16); }
        dSec = w.data();
        appendCrc32(dSec);
        for (const auto& u : s.data) dSec.insert(dSec.end(), u.data.begin(), u.data.end());
    }
    BitWriter w;
    w.put(0, 8);
    w.put(s.hasStartTime, 1); w.put(audio, 1); w.put(data, 1); w.put(ext, 1); w.put(1, 1); w.put(7, 3);
    if (s.hasStartTime) w.put(s.startTime, 32);
    if (audio) { w.put(aSec.size(), 21); w.put(s.streams.size(), 3); }
    if (data) { w.put(dSec.size(), 21); w.put(7, 3); }
    if (ext)
        for (const auto& st : s.streams) {
            const bool rate = st.rate100 >= 0, sr = st.sampleRateCode >= 0, desc = st.language.size() == 3;
            w.put((uint64_t)st.algo, 4); w.put(rate, 1); w.put(sr, 1); w.put(desc, 1); w.put((uint64_t)st.channelsCode, 3); w.put(0x3F, 6);
            if (rate) { w.put((uint64_t)st.rate100, 14); w.put(3, 2); }
            if (sr) { w.put(0xF, 4); w.put((uint64_t)st.sampleRateCode, 4); }
            if (desc) for (char c : st.language) w.put((uint8_t)c, 8);
        }
    std::vector<uint8_t> v = w.data();
    v[0] = (uint8_t)v.size();          // header length with the extension, without the CRC
    appendCrc32(v);
    v.insert(v.end(), aSec.begin(), aSec.end());
    v.insert(v.end(), dSec.begin(), dSec.end());
    return v;
}

std::vector<uint8_t> serviceFrameBytes(ServiceFrameHeader h, const std::vector<std::vector<uint8_t>>& subs, size_t capacity) {
    h.subLens.clear();
    for (const auto& s : subs) h.subLens.push_back((int)s.size());
    BitWriter w;
    w.put(0, 8); w.put((uint64_t)h.version, 4); w.put((uint64_t)h.emergency, 2); w.put(3, 2);
    w.put((uint64_t)h.smfId, 6); w.put(0x3F, 6);
    w.put((uint64_t)h.nitUpdate, 4); w.put((uint64_t)h.smctUpdate, 4); w.put((uint64_t)h.esgUpdate, 4); w.put(0xF, 4);
    w.put(subs.size(), 4);
    for (int len : h.subLens) w.put((uint64_t)len, 24);
    if (h.emergency == 2) w.put(0xFFFFFFFFu, 32);
    std::vector<uint8_t> v = w.data();
    v[0] = (uint8_t)v.size();
    appendCrc32(v);
    for (const auto& s : subs) v.insert(v.end(), s.begin(), s.end());
    if (v.size() < capacity) v.resize(capacity, 0xFF);
    return v;
}

namespace {

// block mode (7.3.4): the payloads of the data blocks of one unit, joined
std::vector<uint8_t> unblock(const uint8_t* d, size_t n, int* unitType) {
    std::vector<uint8_t> out;
    size_t at = 0;
    while (at + 4 <= n && d[at] == 0x55) {
        BitReader r(d + at, n - at);
        r.get(8); r.get(1); r.get(1);
        const int type = (int)r.get(2);
        const size_t len = (size_t)r.get(12);
        size_t hdr = 4;
        if (type == 2) { const int ut = (int)r.get(8); if (unitType) *unitType = ut; hdr = 5; }
        if (at + hdr + len > n || crc8(d + at, hdr - 1) != d[at + hdr - 1]) break;
        out.insert(out.end(), d + at + hdr, d + at + hdr + len);
        at += hdr + len;
    }
    return out;
}

bool parseSubFrame(const uint8_t* d, size_t n, ParsedSubFrame& out) {
    if (n < 6) return false;
    const size_t hl = d[0];
    if (hl < 2 || hl + 4 > n || !crc32Ok(d, hl)) return false;
    BitReader r(d, hl);
    r.get(8);
    MuxSubFrame& s = out.sf;
    s.hasStartTime = r.get(1); s.hasAudio = r.get(1); s.hasData = r.get(1); s.hasExt = r.get(1); s.mode1 = r.get(1); r.get(3);
    size_t aLen = 0, dLen = 0;
    int nStreams = 0;
    if (s.hasStartTime) s.startTime = (uint32_t)r.get(32);
    if (s.hasAudio) { aLen = (size_t)r.get(21); nStreams = (int)r.get(3); }
    if (s.hasData) { dLen = (size_t)r.get(21); r.get(3); }
    if (s.hasExt && s.hasAudio)
        for (int j = 0; j < nStreams; j++) {
            AudioStreamDesc st;
            st.algo = (int)r.get(4);
            const bool rate = r.get(1), sr = r.get(1), desc = r.get(1);
            st.channelsCode = (int)r.get(3); r.get(6);
            if (rate) { st.rate100 = (int)r.get(14); r.get(2); }
            if (sr) { r.get(4); st.sampleRateCode = (int)r.get(4); }
            if (desc) for (int i = 0; i < 3; i++) st.language.push_back((char)r.get(8));
            s.streams.push_back(st);
        }
    else for (int j = 0; j < nStreams; j++) s.streams.push_back(AudioStreamDesc());
    if (!r.ok()) return false;
    out.headerOk = true;
    size_t at = hl + 4;
    if (s.hasAudio) {
        if (at + aLen > n || aLen < 5) return true;
        const uint8_t* a = d + at;
        const int units = a[0];
        const size_t hdr = 1 + 5 * (size_t)units;
        if (hdr + 4 <= aLen && crc32Ok(a, hdr)) {
            BitReader q(a, hdr);
            q.get(8);
            size_t u = hdr + 4;
            out.audioOk = true;
            for (int i = 0; i < units; i++) {
                AudioUnit au;
                const size_t len = (size_t)q.get(16);
                au.stream = (int)q.get(3); q.get(5); au.relTime = (int)q.get(16);
                if (u + len > aLen) { out.audioOk = false; break; }
                au.data.assign(a + u, a + u + len);
                if (!s.mode1) au.data = unblock(a + u, len, nullptr);
                u += len;
                s.audio.push_back(std::move(au));
            }
        }
        at += aLen;
    }
    if (s.hasData) {
        if (at + dLen > n || dLen < 5) return true;
        const uint8_t* a = d + at;
        const int units = a[0];
        const size_t hdr = 1 + 3 * (size_t)units;
        if (hdr + 4 <= dLen && crc32Ok(a, hdr)) {
            BitReader q(a, hdr);
            q.get(8);
            size_t u = hdr + 4;
            out.dataOk = true;
            for (int i = 0; i < units; i++) {
                DataUnit du;
                du.type = (int)q.get(8);
                const size_t len = (size_t)q.get(16);
                if (u + len > dLen) { out.dataOk = false; break; }
                du.data.assign(a + u, a + u + len);
                if (!s.mode1) du.data = unblock(a + u, len, nullptr);
                u += len;
                s.data.push_back(std::move(du));
            }
        }
    }
    return true;
}

} // namespace

bool parseServiceFrame(const uint8_t* d, size_t n, ServiceFrame& out) {
    out = ServiceFrame();
    if (n < 10) return false;
    const size_t hl = d[0];
    if (hl < 6 || hl + 4 > n || !crc32Ok(d, hl)) return false;
    BitReader r(d, hl);
    r.get(8);
    ServiceFrameHeader& h = out.h;
    h.version = (int)r.get(4); h.emergency = (int)r.get(2); r.get(2);
    h.smfId = (int)r.get(6); r.get(6);
    h.nitUpdate = (int)r.get(4); h.smctUpdate = (int)r.get(4); h.esgUpdate = (int)r.get(4); r.get(4);
    const int count = (int)r.get(4);
    for (int i = 0; i < count; i++) h.subLens.push_back((int)r.get(24));
    if (!r.ok()) return false;
    out.headerOk = true;
    size_t at = hl + 4;
    for (int len : h.subLens) {
        ParsedSubFrame p;
        p.len = len;
        if (at + (size_t)len > n) { out.subs.push_back(p); break; }
        parseSubFrame(d + at, (size_t)len, p);
        out.subs.push_back(std::move(p));
        at += (size_t)len;
    }
    return true;
}

// ---------------------------------------------------------------- helpers

const char* dataUnitTypeText(int t) {
    if (t == 0) return "ESG data";
    if (t == 1) return "ESG programme notice";
    if (t == 64) return "emergency broadcast";
    if (t == 160) return "data broadcast";
    if (t >= 161 && t <= 169) return "data broadcast (reserved)";
    if (t == 255) return "system test";
    return "reserved";
}

int sampleRateHz(int code) {
    static const int r[16] = {0, 0, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0, 0, 0, 0};
    return code >= 0 && code < 16 ? r[code] : 0;
}

const char* channelsText(int code) { return code == 1 ? "mono" : code == 2 ? "2 channels" : code == 3 ? "5.1" : "-"; }

std::string printableText(const std::vector<uint8_t>& d) {
    size_t i = 0;
    int printable = 0;
    while (i < d.size()) {
        const uint8_t c = d[i];
        int extra = 0;
        if (c < 0x80) { if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') return {}; if (c == 0x7F) return {}; printable++; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return {};
        for (int k = 1; k <= extra; k++) if (i + (size_t)k >= d.size() || (d[i + (size_t)k] & 0xC0) != 0x80) return {};
        if (extra) printable++;
        i += 1 + (size_t)extra;
    }
    if (!printable) return {};
    return std::string(d.begin(), d.end());
}

std::string nameText(const std::vector<uint8_t>& d) {
    std::string s;
    for (uint8_t c : d) {
        if (c >= 0x20 && c < 0x7F) s.push_back((char)c);
        else { char b[8]; snprintf(b, sizeof b, "\\x%02X", c); s += b; }
    }
    return s;
}

} // namespace dect2::cdr
