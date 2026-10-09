// HD Radio Layer 2 and the data services (see hdr_l2.h). The decoding logic is adapted from nrsc5 (GPL-3.0,
// github.com/theori-io/nrsc5): the transfer frame and PDU parsing from src/frame.c, the SIS messages from src/pids.c, the ID3, SIG and LOT
// handling from src/output.c. Nothing here touches the HDC audio codec: the audio packets are only counted.
#include "dect2/hdr_l2.h"
#include "dect2/hdr_fec.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace dect2 {

const char* hdrProgramTypeName(int t) {
    static const char* const names[] = {"None", "News", "Information", "Sports", "Talk", "Rock", "Classic rock", "Adult hits", "Soft rock", "Top 40",
                                        "Country", "Oldies", "Soft", "Nostalgia", "Jazz", "Classical", "Rhythm and blues", "Soft rhythm and blues",
                                        "Foreign language", "Religious music", "Religious talk", "Personality", "Public", "College", "Spanish talk",
                                        "Spanish music", "Hip hop", "", "", "Weather", "Emergency test", "Emergency"};
    if (t >= 0 && t < 32) return names[t];
    if (t == 65) return "Traffic";
    if (t == 76) return "Special reading services";
    return "";
}

// the data service types of the SIS and the SIG (the values nrsc5.h lists)
const char* hdrDataTypeName(int t) {
    switch (t) {
    case 0: return "non-specific"; case 1: return "News"; case 3: return "Sports"; case 29: return "Weather"; case 31: return "Emergency";
    case 65: return "Traffic"; case 66: return "Image maps"; case 80: return "Text"; case 256: return "Advertising"; case 257: return "Financial";
    case 258: return "Stock ticker"; case 259: return "Navigation"; case 260: return "Program guide"; case 261: return "Audio";
    case 262: return "Private data network"; case 263: return "Service maintenance"; case 264: return "HD Radio system services";
    case 265: return "Audio-related data"; case 511: return "Special tests";
    default: return "";
    }
}

namespace hdr {

const char* mimeName(uint32_t m) {
    switch (m) {
    case kMimePrimaryImage: return "album art (primary image)";
    case kMimeStationLogo: return "station logo";
    case kMimeHdc: return "HDC audio";
    case kMimeText: return "text";
    case kMimeJpeg: return "JPEG";
    case kMimePng: return "PNG";
    case 0x2D42AC3E: return "NAVTEQ traffic";
    case 0x82F03DFC: return "HERE TPEG traffic";
    case 0xB7F03DFC: return "HERE traffic images";
    case 0xEECB55B6: return "HD TMC traffic";
    case 0xB39EBEB2: case 0x4EB03469: case 0x52103469: return "TTN TPEG traffic";
    case kMimeTtnTraffic: return "TTN traffic";
    case kMimeTtnWeather: return "TTN weather";
    default: return "";
    }
}

// ---------------------------------------------------------------- transfer frames (nrsc5 frame.c frame_push())

bool frameGeom(int len, FrameGeom& g) {
    switch (len) {
    case 146176: g = {len, 146176 - 30000, 1248, 24}; return true;   // FM P1
    case 4608: g = {len, 120, 184, 24}; return true;                  // FM P3 (MP3, MP11)
    case 2304: g = {len, 120, 88, 24}; return true;                   // FM P3 (MP2)
    case 3750: g = {len, 120, 160, 22}; return true;                  // AM P1
    case 24000: g = {len, 120, 992, 24}; return true;                 // AM P3 (MA1)
    case 30000: g = {len, 120, 1240, 24}; return true;                // AM P3 (MA3)
    default: return false;
    }
}

int pduBytes(int len) {
    FrameGeom g;
    if (!frameGeom(len, g)) return 0;
    return (len - g.pciLen) / 8;
}

namespace {
// the bit of the L1 stream that carries bit i of the frame (the bytes are sent least significant bit first)
inline int l1Index(int i, int len) {
    const int bs = (i >> 3) << 3;
    const int bl = std::min(8, len - bs);
    return bs + bl - 1 - (i & 7);
}
}

void packTransfer(const std::vector<uint8_t>& pdu, uint32_t pci, int len, uint8_t* bits) {
    FrameGeom g;
    if (!frameGeom(len, g)) return;
    int h = 0;
    size_t bit = 0;
    for (int i = 0; i < len; i++) {
        uint8_t v;
        if (i >= g.start && ((i - g.start) % g.offset) == 0 && h < g.pciLen) { v = (uint8_t)((pci >> (23 - h)) & 1); h++; }
        else { v = bit / 8 < pdu.size() ? (uint8_t)((pdu[bit / 8] >> (7 - (bit & 7))) & 1) : 0; bit++; }
        bits[l1Index(i, len)] = v;
    }
}

void unpackTransfer(const uint8_t* bits, int len, std::vector<uint8_t>& pdu, uint32_t& pci) {
    FrameGeom g;
    pci = 0;
    pdu.clear();
    if (!frameGeom(len, g)) return;
    pdu.reserve((size_t)(len / 8));
    int h = 0, j = 0;
    unsigned val = 0;
    for (int i = 0; i < len; i++) {
        const unsigned b = bits[l1Index(i, len)] & 1;
        if (i >= g.start && ((i - g.start) % g.offset) == 0 && h < g.pciLen) { pci |= b << (23 - h); h++; }
        else {
            val |= b << (7 - j);
            if (++j == 8) { pdu.push_back((uint8_t)val); val = 0; j = 0; }
        }
    }
}

int matchPci(uint32_t pci, int pciLen, uint32_t& match) {
    static const uint32_t cw[8] = {kPciAudio, kPciAudioOpp, kPciAudioFixed, kPciAudioFixedOpp, kPciFixed, 0x8D338D, 0xD8D338, 0x634CE3};
    for (uint32_t c : cw) {
        const int s = __builtin_popcount((pci ^ c) >> (24 - pciLen));
        if (s <= 4) { match = c; return s; }
    }
    return -1;
}

// ---------------------------------------------------------------- encoder pieces

void hdlcAppend(std::vector<uint8_t>& out, const std::vector<uint8_t>& frame) {
    for (uint8_t b : frame) {
        if (b == 0x7E || b == 0x7D) { out.push_back(0x7D); out.push_back((uint8_t)(b ^ 0x20)); }
        else out.push_back(b);
    }
    out.push_back(0x7E);
}

std::vector<uint8_t> aasFrame(uint16_t port, uint16_t seq, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f;
    f.reserve(payload.size() + 7);
    f.push_back(0x21);
    f.push_back((uint8_t)(port & 0xFF)); f.push_back((uint8_t)(port >> 8));
    f.push_back((uint8_t)(seq & 0xFF)); f.push_back((uint8_t)(seq >> 8));
    f.insert(f.end(), payload.begin(), payload.end());
    appendFcs(f);
    return f;
}

std::vector<uint8_t> id3Tag(const std::string& title, const std::string& artist, const std::string& album, const std::string& genre, int xhdrLot, uint32_t xhdrMime) {
    std::vector<uint8_t> fr;
    auto frame = [&](const char* id, const std::vector<uint8_t>& data) {
        fr.insert(fr.end(), id, id + 4);
        const uint32_t n = (uint32_t)data.size();
        fr.push_back((uint8_t)(n >> 24)); fr.push_back((uint8_t)(n >> 16)); fr.push_back((uint8_t)(n >> 8)); fr.push_back((uint8_t)n);
        fr.push_back(0); fr.push_back(0);
        fr.insert(fr.end(), data.begin(), data.end());
    };
    auto text = [&](const char* id, const std::string& s) {
        if (s.empty()) return;
        std::vector<uint8_t> d{0};                  // ISO-8859-1
        d.insert(d.end(), s.begin(), s.end());
        frame(id, d);
    };
    text("TIT2", title);
    text("TPE1", artist);
    text("TALB", album);
    text("TCON", genre);
    if (xhdrLot >= 0) {
        std::vector<uint8_t> d = {(uint8_t)xhdrMime, (uint8_t)(xhdrMime >> 8), (uint8_t)(xhdrMime >> 16), (uint8_t)(xhdrMime >> 24), 0, 2,
                                  (uint8_t)(xhdrLot & 0xFF), (uint8_t)(xhdrLot >> 8)};
        frame("XHDR", d);
    }
    std::vector<uint8_t> t = {'I', 'D', '3', 3, 0, 0};
    const uint32_t n = (uint32_t)fr.size();
    t.push_back((uint8_t)((n >> 21) & 0x7F)); t.push_back((uint8_t)((n >> 14) & 0x7F)); t.push_back((uint8_t)((n >> 7) & 0x7F)); t.push_back((uint8_t)(n & 0x7F));
    t.insert(t.end(), fr.begin(), fr.end());
    return t;
}

std::vector<uint8_t> sigTable(const std::vector<SigService>& services) {
    std::vector<uint8_t> o;
    for (const SigService& s : services) {
        o.push_back(s.audio ? 0x40 : 0x41);
        o.push_back((uint8_t)(s.number & 0xFF)); o.push_back((uint8_t)(s.number >> 8)); o.push_back(0);
        if (!s.name.empty()) {
            o.push_back(0x69);
            o.push_back((uint8_t)(s.name.size() + 2));
            o.push_back(0);
            o.insert(o.end(), s.name.begin(), s.name.end());
        }
        for (const SigService::Comp& c : s.comps) {
            if (c.audio) {
                o.push_back(0x66); o.push_back(12);
                o.push_back((uint8_t)c.id); o.push_back((uint8_t)c.port); o.push_back((uint8_t)c.type);
                o.push_back(0); o.push_back(0); o.push_back(0); o.push_back(0);
            } else {
                o.push_back(0x67); o.push_back(13);
                o.push_back((uint8_t)c.id); o.push_back((uint8_t)(c.port & 0xFF)); o.push_back((uint8_t)(c.port >> 8));
                o.push_back((uint8_t)(c.sdType & 0xFF)); o.push_back((uint8_t)(c.sdType >> 8)); o.push_back((uint8_t)c.type);
                o.push_back(0); o.push_back(0);
            }
            o.push_back((uint8_t)c.mime); o.push_back((uint8_t)(c.mime >> 8)); o.push_back((uint8_t)(c.mime >> 16)); o.push_back((uint8_t)(c.mime >> 24));
        }
    }
    return o;
}

std::vector<std::vector<uint8_t>> lotFragments(const LotFile& f, int repeat) {
    std::vector<std::vector<uint8_t>> out;
    const size_t nf = std::max<size_t>(1, (f.bytes.size() + 255) / 256);
    for (size_t i = 0; i < nf; i++) {
        std::vector<uint8_t> p;
        const bool head = i == 0;
        const size_t name = std::min<size_t>(f.name.size(), 200);
        p.push_back((uint8_t)(8 + (head ? 16 + name : 0)));
        p.push_back((uint8_t)repeat);
        p.push_back((uint8_t)(f.lot & 0xFF)); p.push_back((uint8_t)(f.lot >> 8));
        p.push_back((uint8_t)i); p.push_back((uint8_t)(i >> 8)); p.push_back((uint8_t)(i >> 16)); p.push_back((uint8_t)(i >> 24));
        if (head) {
            p.push_back(1); p.push_back(0); p.push_back(0); p.push_back(0);      // version 1
            const int year = 2030, mon = 1, mday = 1, hour = 0, min = 0;        // expiry, UTC
            p.push_back((uint8_t)(((hour & 3) << 6) | min));
            p.push_back((uint8_t)((mday << 3) | (hour >> 2)));
            p.push_back((uint8_t)(((year & 0xF) << 4) | mon));
            p.push_back((uint8_t)(year >> 4));
            const uint32_t sz = (uint32_t)f.bytes.size();
            p.push_back((uint8_t)sz); p.push_back((uint8_t)(sz >> 8)); p.push_back((uint8_t)(sz >> 16)); p.push_back((uint8_t)(sz >> 24));
            p.push_back((uint8_t)f.mime); p.push_back((uint8_t)(f.mime >> 8)); p.push_back((uint8_t)(f.mime >> 16)); p.push_back((uint8_t)(f.mime >> 24));
            p.insert(p.end(), f.name.begin(), f.name.begin() + (long)name);
        }
        const size_t a = i * 256, b = std::min(f.bytes.size(), a + 256);
        p.insert(p.end(), f.bytes.begin() + (long)a, f.bytes.begin() + (long)b);
        out.push_back(std::move(p));
    }
    return out;
}

namespace {
int locBits(int codec, int stream) {
    switch (codec) {
    case 0: return 16;
    case 1: case 2: case 3: return stream == 0 ? 12 : 16;
    case 10: case 13: return 12;
    default: return 16;
    }
}
}

int audioPduHeaderBytes(const AudioPduSpec& s) {
    const int nop = (int)s.packetBytes.size();
    return 14 + (locBits(s.codecMode, s.streamId) * nop + 4) / 8 + 3;
}

std::vector<uint8_t> audioPdu(const AudioPduSpec& s, std::vector<uint8_t>& psd) {
    const int nop = (int)s.packetBytes.size();
    const int lcb = locBits(s.codecMode, s.streamId);
    const int hdr = audioPduHeaderBytes(s);
    const int la = hdr + s.psdRoom - 1;                 // last byte of the PSD area
    std::vector<uint8_t> o((size_t)hdr, 0);
    o[8] = (uint8_t)((s.codecMode & 0xF) | ((s.streamId & 3) << 4) | ((s.pduSeq & 3) << 6));
    o[9] = (uint8_t)(((s.pduSeq >> 2) & 1) | (0 << 1) | (0 << 3));     // blend control 0, digital gain 0
    o[10] = (uint8_t)(0 | ((s.latency & 3) << 6));
    o[11] = (uint8_t)(((s.latency >> 2) & 1) | (0 << 1) | (0 << 2) | ((s.seq & 0x1F) << 3));   // whole packets only (pfirst = plast = 0)
    o[12] = (uint8_t)(((s.seq >> 5) & 1) | ((nop & 0x3F) << 1) | 0x80);    // with a header expansion
    o[13] = (uint8_t)la;
    // packet locators: the offset of the last byte (the CRC) of every packet
    std::vector<int> loc;
    int end = la;
    for (int b : s.packetBytes) { end += b + 1; loc.push_back(end); }
    uint8_t* L = &o[14];
    for (int j = 0; j < nop; j++) {
        const int v = loc[(size_t)j];
        if (lcb == 16) { L[2 * j] = (uint8_t)(v & 0xFF); L[2 * j + 1] = (uint8_t)(v >> 8); }
        else if (j % 2 == 0) { L[j / 2 * 3] = (uint8_t)(v & 0xFF); L[j / 2 * 3 + 1] = (uint8_t)((L[j / 2 * 3 + 1] & 0xF0) | ((v >> 8) & 0xF)); }
        else { L[j / 2 * 3 + 1] = (uint8_t)((L[j / 2 * 3 + 1] & 0x0F) | ((v & 0xF) << 4)); L[j / 2 * 3 + 2] = (uint8_t)(v >> 4); }
    }
    // header expansion: program number, then access and program type
    uint8_t* H = &o[(size_t)hdr - 3];
    H[0] = (uint8_t)(0x80 | 0x10 | ((s.program & 7) << 1));
    H[1] = (uint8_t)(0x80 | 0x20 | ((s.access & 1) << 3) | ((s.progType >> 7) & 1));
    H[2] = (uint8_t)(s.progType & 0x7F);
    // the PSD stream, padded with flags
    const size_t take = std::min(psd.size(), (size_t)s.psdRoom);
    o.insert(o.end(), psd.begin(), psd.begin() + (long)take);
    psd.erase(psd.begin(), psd.begin() + (long)take);
    o.insert(o.end(), (size_t)s.psdRoom - take, 0x7E);
    for (int b : s.packetBytes) {
        const size_t st = o.size();
        o.insert(o.end(), (size_t)b, s.fill);
        o.push_back(crc8(&o[st], (size_t)b));
    }
    while (o.size() < 96) o.push_back(0);              // never happens with real packets: the RS word needs 96 bytes
    rsEncodeHeader(o.data());
    return o;
}

void fixedBlock(std::vector<uint8_t>& hdlc, std::vector<uint8_t>& out) {
    static const uint8_t bbm[4] = {0x7D, 0x3A, 0xE2, 0x42};
    out.insert(out.end(), bbm, bbm + 4);
    const size_t n = std::min<size_t>(255, hdlc.size());
    out.insert(out.end(), hdlc.begin(), hdlc.begin() + (long)n);
    hdlc.erase(hdlc.begin(), hdlc.begin() + (long)n);
    for (size_t i = n; i < 255; i++) out.push_back(0x7E);
}

void fixedRegion(const FixedSpec& f, std::vector<uint8_t>& sub, std::vector<uint8_t>& out) {
    // subchannel bytes, then the CCC (channel configuration) and the sync byte that gives its width
    const size_t take = std::min(sub.size(), (size_t)f.subLen);
    out.insert(out.end(), sub.begin(), sub.begin() + (long)take);
    sub.erase(sub.begin(), sub.begin() + (long)take);
    out.insert(out.end(), (size_t)f.subLen - take, 0x7E);
    std::vector<uint8_t> ccc = {0x00, 0x00, 0x00, (uint8_t)(f.subLen & 0xFF), (uint8_t)(f.subLen >> 8)};
    appendFcs(ccc);
    std::vector<uint8_t> w = {0x7E};
    hdlcAppend(w, ccc);
    int width = std::max(f.cccWidth, (int)w.size() + 1);
    width = std::min(30, (width + 1) & ~1);
    while ((int)w.size() < width) w.push_back(0x7E);
    w.resize((size_t)width);
    out.insert(out.end(), w.begin(), w.end());
    const int n = width / 2;
    out.push_back((uint8_t)((n << 4) | n));
}

// ---------------------------------------------------------------- SIS encoder (the reverse of nrsc5 pids.c)

namespace {
const char kChars5[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ ?-*$ ";
void putBits(std::vector<uint8_t>& v, unsigned x, int n) { for (int i = n - 1; i >= 0; i--) v.push_back((uint8_t)((x >> i) & 1)); }
int char5(char c) {
    for (int i = 0; i < 31; i++) if (kChars5[i] == c) return i;
    return 26;
}
}

SisEncoder::SisEncoder(const SisConfig& c) {
    auto add = [&](int id, std::vector<uint8_t> b) { msgs_.push_back({id, std::move(b)}); };
    {   // station id
        std::vector<uint8_t> b;
        for (int i = 0; i < 2; i++) putBits(b, (unsigned)char5(i < (int)c.country.size() ? (char)std::toupper((unsigned char)c.country[(size_t)i]) : ' '), 5);
        putBits(b, 0, 3);
        putBits(b, (unsigned)c.facilityId & 0x7FFFF, 19);
        add(0, b);
    }
    {   // short name (call sign)
        std::vector<uint8_t> b;
        for (int i = 0; i < 4; i++) putBits(b, (unsigned)char5(i < (int)c.callSign.size() ? c.callSign[(size_t)i] : ' '), 5);
        putBits(b, c.fmSuffix ? 1 : 0, 2);
        add(1, b);
    }
    if (!c.name.empty()) {   // universal short station name: up to two frames of 6 characters
        const std::string n = c.name.substr(0, 12);
        const int frames = n.size() > 6 ? 2 : 1;
        for (int f = 0; f < frames; f++) {
            std::vector<uint8_t> b;
            putBits(b, (unsigned)f, 4); putBits(b, 0, 1);
            if (f == 0) { putBits(b, 0, 3); putBits(b, c.fmSuffix ? 1 : 0, 1); putBits(b, (unsigned)(frames - 1), 1); }
            else putBits(b, 0, 5);
            for (int j = 0; j < 6; j++) { const size_t k = (size_t)(f * 6 + j); putBits(b, k < n.size() ? (uint8_t)n[k] : 0, 8); }
            add(8, b);
        }
    }
    if (!c.slogan.empty()) {   // slogan: 5 characters in the first frame, 6 in the others
        const std::string s = c.slogan.substr(0, 95);
        const int len = (int)s.size(), frames = (len + 6) / 6;
        for (int f = 0; f < frames; f++) {
            std::vector<uint8_t> b;
            putBits(b, (unsigned)f, 4); putBits(b, 1, 1);
            int first;
            if (f == 0) { putBits(b, 0, 3); putBits(b, 0, 3); putBits(b, (unsigned)len, 7); first = 0; }
            else { putBits(b, 0, 5); first = f * 6 - 1; }
            const int cnt = f == 0 ? 5 : 6;
            for (int j = 0; j < cnt; j++) { const int k = first + j; putBits(b, k < len ? (uint8_t)s[(size_t)k] : 0, 8); }
            add(8, b);
        }
    }
    if (!c.longName.empty()) {   // long station name: 7-bit characters, 7 per frame, up to 8 frames
        const std::string s = c.longName.substr(0, 56);
        const int frames = std::max(1, ((int)s.size() + 6) / 7);
        for (int f = 0; f < frames; f++) {
            std::vector<uint8_t> b;
            putBits(b, (unsigned)(frames - 1), 3); putBits(b, (unsigned)f, 3);
            for (int j = 0; j < 7; j++) { const size_t k = (size_t)(f * 7 + j); putBits(b, k < s.size() ? (uint8_t)s[k] & 0x7F : 0, 7); }
            putBits(b, 0, 3);   // sequence
            add(2, b);
        }
    }
    if (c.location) {
        const int lat = (int)std::lround(c.lat * 8192), lon = (int)std::lround(c.lon * 8192);
        std::vector<uint8_t> b;
        putBits(b, 1, 1); putBits(b, (unsigned)lat & 0x3FFFFF, 22); putBits(b, (unsigned)(c.altM >> 8) & 0xF, 4);
        add(4, b);
        b.clear();
        putBits(b, 0, 1); putBits(b, (unsigned)lon & 0x3FFFFF, 22); putBits(b, (unsigned)(c.altM >> 4) & 0xF, 4);
        add(4, b);
    }
    for (const auto& a : c.audio) {
        std::vector<uint8_t> b;
        putBits(b, 0, 2); putBits(b, (unsigned)a.access, 1); putBits(b, (unsigned)a.program, 6); putBits(b, (unsigned)a.type, 8);
        putBits(b, 0, 5); putBits(b, (unsigned)a.soundExp, 5);
        add(6, b);
    }
    for (const auto& d : c.data) {
        std::vector<uint8_t> b;
        putBits(b, 1, 2); putBits(b, (unsigned)d.access, 1); putBits(b, (unsigned)d.type, 9); putBits(b, 0, 3); putBits(b, (unsigned)d.mime, 12);
        add(6, b);
    }
    if (!c.message.empty()) {   // station message: 4 characters in frame 0, 6 in the others
        const std::string s = c.message.substr(0, 190);
        const int len = (int)s.size();
        unsigned sum = 0;
        for (char ch : s) sum += (uint8_t)ch;
        const unsigned ck = (((sum >> 8) & 0x7F) + (sum & 0xFF)) & 0x7F;
        const int frames = (len + 7) / 6;
        for (int f = 0; f < frames; f++) {
            std::vector<uint8_t> b;
            putBits(b, (unsigned)f, 5); putBits(b, 0, 2);
            int first, cnt;
            if (f == 0) { putBits(b, 0, 1); putBits(b, 0, 3); putBits(b, (unsigned)len, 8); putBits(b, ck, 7); first = 0; cnt = 4; }
            else { putBits(b, 0, 3); first = f * 6 - 2; cnt = 6; }
            for (int j = 0; j < cnt; j++) { const int k = first + j; putBits(b, k < len ? (uint8_t)s[(size_t)k] : 0, 8); }
            add(5, b);
        }
    }
}

void SisEncoder::next(uint8_t* bits) {
    memset(bits, 0, 80);
    if (msgs_.empty()) { putCrc12(bits); return; }
    const Msg& a = msgs_[pos_ % msgs_.size()];
    const Msg& b = msgs_[(pos_ + 1) % msgs_.size()];
    const bool two = msgs_.size() > 1 && a.bits.size() + b.bits.size() <= 54;
    int off = 2;                       // bit 0: SIS frame, bit 1: one or two messages
    bits[0] = 0;
    bits[1] = two ? 1 : 0;
    auto put = [&](const Msg& m) {
        for (int i = 3; i >= 0; i--) bits[off++] = (uint8_t)((m.id >> i) & 1);
        for (uint8_t x : m.bits) bits[off++] = x;
    };
    put(a);
    if (two) put(b);
    pos_ += two ? 2 : 1;
    putCrc12(bits);
}

// ---------------------------------------------------------------- decoder state

struct L2Decoder::Sis {
    std::string country;
    int facility = -1;
    std::string shortName;
    char longName[8 * 7 + 1] = {};
    bool longHave[8] = {};
    int longSeq = -1;
    bool longDone = false;
    double lat = NAN, lon = NAN;
    int alt = 0;
    char msg[256 + 8] = {};
    bool msgHave[32] = {};
    int msgSeq = -1, msgLen = 0, msgEnc = 0, msgCk = 0;
    bool msgDone = false;
    char uname[16] = {};
    bool unameHave[16] = {};
    int unameLen = -1, unameEnc = 0, unameAppend = 0;
    bool unameDone = false;
    char slogan[128] = {};
    bool sloganHave[16] = {};
    int sloganLen = -1, sloganEnc = 0;
    bool sloganDone = false;
    char alert[512] = {};
    bool alertHave[64] = {};
    int alertSeq = -1, alertLen = -1, alertEnc = 0, alertCrc = 0, alertCnt = 0;
    bool alertDone = false;
    int alertTimeout = 0;
    struct Asd { int access = -1, type = -1, sound = -1; } asd[64];
    struct Dsd { int access, type, mime; };
    std::vector<Dsd> dsd;
};

struct L2Decoder::Prog {
    HdrProgram p;
    std::vector<uint8_t> psd;
    bool psdOpen = false;
    double rate[3] = {0, 0, 0};       // audio bytes per second on P1, P3 and P4 (moving average)
    bool rateInit[3] = {false, false, false};
    uint64_t bytesNow[3] = {0, 0, 0}; // in the transfer frame being parsed
};

struct L2Decoder::Fixed {
    int syncWidth = 0, syncCount = 0;
    std::vector<uint8_t> ccc;
    bool cccOpen = false;
    bool ready = false;
    struct Sub { int length = 0; std::vector<uint8_t> blocks, acc; bool open = false; } sub[4];
};

L2Decoder::L2Decoder() { reset(); }
L2Decoder::~L2Decoder() = default;

void L2Decoder::reset() {
    sis_ = std::make_unique<Sis>();
    progs_.clear();
    sigServices_.clear();
    sigRaw_.clear();
    lots_.clear();
    portCount_.clear();
    lotVersion_ = 0;
    for (int c = 0; c < 3; c++) pciOk_[c] = pciBad_[c] = 0;
    pdus_ = aasOk_ = aasBad_ = pidsOk_ = pidsBad_ = 0;
    lastPsd_ = lastData_ = -1e9;
    resetTransport();
}

void L2Decoder::resetTransport() {
    for (auto& f : fixed_) f = std::make_unique<Fixed>();
    for (auto& kv : progs_) { kv.second.psd.clear(); kv.second.psdOpen = false; }
}

L2Decoder::Prog& L2Decoder::prog(int n) {
    auto it = progs_.find(n);
    if (it == progs_.end()) { it = progs_.emplace(n, Prog()).first; it->second.p.number = n; }
    return it->second;
}

namespace {
std::string latin1(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (!c) break;
        if (c < 0x80) s += (char)c;
        else { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}
std::string ucs2(const uint8_t* p, size_t n) {
    bool le = true;
    size_t i = 0;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { i = 2; }
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { le = false; i = 2; }
    std::string s;
    for (; i + 1 < n; i += 2) {
        const unsigned c = le ? (p[i] | (p[i + 1] << 8)) : ((p[i] << 8) | p[i + 1]);
        if (!c) break;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}
std::string textEnc(int enc, const uint8_t* p, size_t n) { return enc == 1 ? ucs2(p, n) : latin1(p, n); }
std::string textEnc(int enc, const char* p, size_t n) { return textEnc(enc, reinterpret_cast<const uint8_t*>(p), n); }
size_t cstrLen(const char* s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

unsigned getBits(const uint8_t* bits, int& off, int n) {
    unsigned r = 0;
    for (int i = 0; i < n; i++) r = (r << 1) | bits[off++];
    return r;
}
int getSigned(const uint8_t* bits, int& off, int n) {
    const int r = (int)getBits(bits, off, n);
    return (r & (1 << (n - 1))) ? r - (1 << n) : r;
}

// nrsc5 pids.c crc7() over the alert text
int crc7(const char* a, int len) {
    const unsigned char poly = 0x09;
    unsigned char reg = 0x42;
    for (int bi = len - 1; bi >= 0; bi--) {
        for (int b = 6; b >= 0; b--) {
            unsigned char bit = ((unsigned char)a[bi] >> b) & 1;
            if (b == 0 && bi > 0) bit ^= ((unsigned char)a[bi - 1] >> 7);
            reg = (unsigned char)(reg << 1);
            reg ^= bit;
            if (reg & 0x80) reg ^= (0x80 | poly);
        }
    }
    for (int b = 6; b >= 0; b--) {
        reg = (unsigned char)(reg << 1);
        if (reg & 0x80) reg ^= (0x80 | poly);
    }
    return reg;
}
}

// ---------------------------------------------------------------- PIDS / SIS (nrsc5 pids.c)

void L2Decoder::pushPids(const uint8_t* bits) {
    uint8_t p[80];
    for (int i = 0; i < 80; i++) p[i] = bits[((i >> 3) << 3) + 7 - (i & 7)];
    if (crc12(p) != (unsigned)((p[68] << 11) | (p[69] << 10) | (p[70] << 9) | (p[71] << 8) | (p[72] << 7) | (p[73] << 6) | (p[74] << 5) | (p[75] << 4) |
                               (p[76] << 3) | (p[77] << 2) | (p[78] << 1) | p[79])) { pidsBad_++; return; }
    pidsOk_++;
    if (p[0] == 0) sisDecode(p + 1);
}

void L2Decoder::sisDecode(const uint8_t* bits) {
    static const int sizes[16] = {32, 22, 58, 32, 27, 58, 27, 22, 58, 58, 27, -1, -1, -1, -1, -1};
    Sis& S = *sis_;
    int off = 0;
    const int payloads = bits[0] + 1;
    off = 1;
    if (S.alertDone) S.alertTimeout++;
    for (int n = 0; n < payloads; n++) {
        if (off > 59) break;
        const int id = (int)getBits(bits, off, 4);
        const int sz = sizes[id];
        if (sz < 0 || off > 63 - sz) break;
        const uint8_t* b = bits + off;
        int o = 0;
        switch (id) {
        case 0: {   // station id
            char cc[3] = {kChars5[getBits(b, o, 5)], 0, 0};
            cc[1] = kChars5[getBits(b, o, 5)];
            o += 3;
            const int fac = (int)getBits(b, o, 19);
            if (S.country != cc || S.facility != fac) { S.country = cc; S.facility = fac; }
            break;
        }
        case 1: {   // short name
            std::string s;
            for (int j = 0; j < 4; j++) s += kChars5[getBits(b, o, 5)];
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (b[o] == 0 && b[o + 1] == 1) s += "-FM";
            if (s != S.shortName) { S.shortName = s; say("HD Radio: call sign " + s); }
            break;
        }
        case 2: {   // long name
            const int last = (int)getBits(b, o, 3), cur = (int)getBits(b, o, 3);
            int t = 55;
            const int seq = (int)getBits(b, t, 3);
            if (cur == 0 && seq != S.longSeq) { memset(S.longName, 0, sizeof S.longName); memset(S.longHave, 0, sizeof S.longHave); S.longSeq = seq; S.longDone = false; }
            for (int j = 0; j < 7; j++) S.longName[cur * 7 + j] = (char)getBits(b, o, 7);
            S.longHave[cur] = true;
            if (S.longSeq >= 0 && !S.longDone) {
                bool all = true;
                for (int j = 0; j <= last; j++) all = all && S.longHave[j];
                if (all) S.longDone = true;
            }
            break;
        }
        case 4: {   // location
            if (b[o++]) { S.lat = getSigned(b, o, 22) / 8192.0; S.alt = (S.alt & 0x0F0) | ((int)getBits(b, o, 4) << 8); }
            else { S.lon = getSigned(b, o, 22) / 8192.0; S.alt = (S.alt & 0xF00) | ((int)getBits(b, o, 4) << 4); }
            break;
        }
        case 5: {   // station message
            const int cur = (int)getBits(b, o, 5), seq = (int)getBits(b, o, 2);
            if (cur == 0) {
                if (seq != S.msgSeq) { memset(S.msg, 0, sizeof S.msg); memset(S.msgHave, 0, sizeof S.msgHave); S.msgSeq = seq; S.msgDone = false; }
                o += 1;   // priority
                S.msgEnc = (int)getBits(b, o, 3);
                S.msgLen = (int)getBits(b, o, 8);
                S.msgCk = (int)getBits(b, o, 7);
                for (int j = 0; j < 4; j++) S.msg[j] = (char)getBits(b, o, 8);
            } else {
                o += 3;
                for (int j = 0; j < 6; j++) S.msg[cur * 6 - 2 + j] = (char)getBits(b, o, 8);
            }
            S.msgHave[cur] = true;
            if (S.msgSeq >= 0 && !S.msgDone) {
                bool all = true;
                for (int j = 0; j < (S.msgLen + 7) / 6; j++) all = all && S.msgHave[j];
                if (all) {
                    unsigned sum = 0;
                    for (int j = 0; j < S.msgLen; j++) sum += (uint8_t)S.msg[j];
                    sum = (((sum >> 8) & 0x7F) + (sum & 0xFF)) & 0x7F;
                    if ((int)sum == S.msgCk) S.msgDone = true;
                }
            }
            break;
        }
        case 6: case 10: {   // service information
            const int cat = (int)getBits(b, o, 2);
            if (cat == 0) {
                const int acc = (int)getBits(b, o, 1), pn = (int)getBits(b, o, 6), type = (int)getBits(b, o, 8);
                o += 5;
                const int se = (int)getBits(b, o, 5);
                if (pn < 64) { S.asd[pn].access = acc; S.asd[pn].type = type; S.asd[pn].sound = se; }
            } else if (cat == 1) {
                const int acc = (int)getBits(b, o, 1), type = (int)getBits(b, o, 9);
                o += 3;
                const int mime = (int)getBits(b, o, 12);
                bool have = false;
                for (const auto& d : S.dsd) if (d.access == acc && d.type == type && d.mime == mime) have = true;
                if (!have && S.dsd.size() < 16) S.dsd.push_back({acc, type, mime});
            }
            break;
        }
        case 8: {   // universal short station name or slogan
            const int cur = (int)getBits(b, o, 4);
            if (b[o++] == 0) {
                if (cur >= 2) break;
                if (cur == 0) {
                    S.unameEnc = (int)getBits(b, o, 3);
                    S.unameAppend = b[o++];
                    S.unameLen = b[o++] + 1;
                    for (int j = 0; j < 6; j++) S.uname[j] = (char)getBits(b, o, 8);
                } else {
                    o += 5;
                    for (int j = 0; j < 6; j++) S.uname[cur * 6 + j] = (char)getBits(b, o, 8);
                }
                S.unameHave[cur] = true;
                if (S.unameLen >= 0 && !S.unameDone) {
                    bool all = true;
                    for (int j = 0; j < S.unameLen; j++) all = all && S.unameHave[j];
                    if (all) S.unameDone = true;
                }
            } else {
                if (cur == 0) {
                    S.sloganEnc = (int)getBits(b, o, 3);
                    o += 3;
                    S.sloganLen = (int)getBits(b, o, 7);
                    for (int j = 0; j < 5; j++) S.slogan[j] = (char)getBits(b, o, 8);
                } else {
                    o += 5;
                    for (int j = 0; j < 6; j++) if (cur * 6 - 1 + j < 127) S.slogan[cur * 6 - 1 + j] = (char)getBits(b, o, 8);
                }
                S.sloganHave[cur] = true;
                if (S.sloganLen >= 0 && !S.sloganDone) {
                    bool all = true;
                    for (int j = 0; j < (S.sloganLen + 6) / 6; j++) all = all && S.sloganHave[j];
                    if (all) S.sloganDone = true;
                }
            }
            break;
        }
        case 9: {   // emergency alert
            const int cur = (int)getBits(b, o, 6), seq = (int)getBits(b, o, 2);
            o += 2;
            S.alertTimeout = 0;
            if (cur == 0) {
                if (seq != S.alertSeq) { memset(S.alert, 0, sizeof S.alert); memset(S.alertHave, 0, sizeof S.alertHave); S.alertSeq = seq; S.alertDone = false; }
                S.alertEnc = (int)getBits(b, o, 3);
                S.alertLen = (int)getBits(b, o, 9);
                S.alertCrc = (int)getBits(b, o, 7);
                S.alertCnt = 1 + 2 * (int)getBits(b, o, 5);
                for (int j = 0; j < 3; j++) S.alert[j] = (char)getBits(b, o, 8);
            } else {
                for (int j = 0; j < 6; j++) if (cur * 6 - 3 + j < 500) S.alert[cur * 6 - 3 + j] = (char)getBits(b, o, 8);
            }
            S.alertHave[cur] = true;
            if (S.alertLen >= 0 && !S.alertDone) {
                bool all = true;
                for (int j = 0; j < (S.alertLen + 8) / 6 && j < 64; j++) all = all && S.alertHave[j];
                if (all && crc7(S.alert, S.alertLen) == S.alertCrc && S.alertCnt >= 7 && S.alertLen >= S.alertCnt) {
                    S.alertDone = true;
                    say("HD Radio: emergency alert received");
                }
            }
            break;
        }
        default: break;     // parameter messages and reserved ones
        }
        off += sz;
    }
    if (S.alertDone && S.alertTimeout >= 16) { S.alertDone = false; S.alertSeq = -1; S.alertLen = -1; memset(S.alertHave, 0, sizeof S.alertHave); }
}

// ---------------------------------------------------------------- transfer frames (nrsc5 frame.c)

void L2Decoder::pushTransfer(const uint8_t* bits, int len, int channel) {
    std::vector<uint8_t> pdu;
    uint32_t pci = 0;
    unpackTransfer(bits, len, pdu, pci);
    FrameGeom g;
    if (!frameGeom(len, g)) return;
    uint32_t m = 0;
    channel = chan(channel);
    if (matchPci(pci, g.pciLen, m) < 0) { pciBad_[channel]++; return; }
    pciOk_[channel]++;
    for (auto& kv : progs_) kv.second.bytesNow[channel] = 0;
    frameProcess(pdu, channel, m);
    // audio bit rate of each program on this channel, averaged over about 4 seconds
    const double dur = (len == 146176 || len == 24000 || len == 30000) ? 65536.0 / 44100.0 : 65536.0 / 44100.0 / 8.0;
    const double a = std::min(1.0, dur / 4.0);
    for (auto& kv : progs_) {
        Prog& P = kv.second;
        const double r = (double)P.bytesNow[channel] / dur;
        if (!P.rateInit[channel]) { if (P.bytesNow[channel]) { P.rate[channel] = r; P.rateInit[channel] = true; } }
        else P.rate[channel] += a * (r - P.rate[channel]);
        P.p.kbps = (P.rate[0] + P.rate[1] + P.rate[2]) * 8 / 1000;
    }
}

namespace {
bool hasAudio(uint32_t pci) { return pci == kPciAudio || pci == kPciAudioOpp || pci == kPciAudioFixed || pci == kPciAudioFixedOpp; }
bool hasFixed(uint32_t pci) { return pci == kPciAudioFixed || pci == kPciAudioFixedOpp || pci == kPciFixed; }
size_t unescape(std::vector<uint8_t>& d) {
    size_t o = 0;
    for (size_t i = 0; i < d.size(); i++) {
        if (d[i] == 0x7D && i + 1 < d.size()) d[o++] = (uint8_t)(d[++i] | 0x20);
        else d[o++] = d[i];
    }
    d.resize(o);
    return o;
}
}

void L2Decoder::frameProcess(std::vector<uint8_t>& buf, int channel, uint32_t pci) {
    size_t audioEnd = buf.size();
    if (hasFixed(pci)) audioEnd = fixedData(buf, channel);
    if (!hasAudio(pci)) return;
    size_t offset = 0;
    while (offset + 96 < audioEnd) {
        const size_t start = offset;
        uint8_t* h = &buf[offset];
        if (!rsDecodeHeader(h, nullptr)) return;
        const int codec = h[8] & 0xF, stream = (h[8] >> 4) & 3;
        const int blend = (h[9] >> 1) & 3;
        const int nop = (h[12] >> 1) & 0x3F, hef = h[12] >> 7, la = h[13];
        offset += 14;
        const int lcb = locBits(codec, stream);
        const size_t locBytes = (size_t)((lcb * nop + 4) / 8);
        if (start + (size_t)la + 1 < offset + locBytes || start + (size_t)la >= audioEnd) return;
        std::vector<int> loc((size_t)nop);
        for (int j = 0; j < nop; j++) {
            const uint8_t* L = &buf[offset];
            int v;
            if (lcb == 16) v = L[2 * j] | (L[2 * j + 1] << 8);
            else if (j % 2 == 0) v = ((L[j / 2 * 3 + 1] & 0xF) << 8) | L[j / 2 * 3];
            else v = (L[j / 2 * 3 + 2] << 4) | (L[j / 2 * 3 + 1] >> 4);
            loc[(size_t)j] = v;
            if (j == 0 && v <= la) return;
            if (j > 0 && v <= loc[(size_t)j - 1]) return;
            if (start + (size_t)v >= audioEnd) return;
        }
        offset += locBytes;
        if (stream >= 2) {
            if (nop == 0) return;
            offset = start + (size_t)loc[(size_t)nop - 1] + 1;
            continue;
        }
        int progNum = 0, access = 0, type = -1;
        if (hef) {   // header expansion fields (nrsc5 parse_hef()); every element's last byte says whether another follows
            const size_t end = audioEnd;
            size_t p = offset;
            bool more = true;
            while (more) {
                if (p >= end) { p = end; break; }
                const uint8_t c = buf[p];
                bool bad = false;
                switch ((c >> 4) & 7) {
                case 1:
                    progNum = (c >> 1) & 7;
                    if (c & 1) { if (p + 2 >= end) bad = true; else p += 2; }
                    break;
                case 2:
                    if (p + 1 >= end) { bad = true; break; }
                    access = (c >> 3) & 1;
                    type = (c & 1) << 7;
                    p++;
                    type |= buf[p] & 0x7F;
                    break;
                case 3:
                    if (p + ((c & 8) ? 4 : 3) >= end) bad = true; else p += (c & 8) ? 4 : 3;
                    break;
                case 4:
                    if (p + ((c & 8) ? 3 : 1) >= end) bad = true; else p += (c & 8) ? 3 : 1;
                    break;
                default: break;
                }
                if (bad) { p = end; break; }
                more = (buf[p++] & 0x80) != 0;
            }
            offset = p;
        }
        Prog& P = prog(progNum);
        pdus_++;
        P.p.onAir = true;
        if (stream == 0) {
            P.p.codecMode = codec;
            P.p.blend = blend;
            P.p.access = access;
            if (type >= 0) P.p.type = type;
        }
        // program service data and the opportunistic AAS packets, in an HDLC stream across the PDUs
        const size_t psdEnd = start + (size_t)la + 1;
        if (psdEnd > offset) hdlcFeed(P.psd, P.psdOpen, &buf[offset], psdEnd - offset, 100 + progNum, channel);
        offset = psdEnd;
        for (int j = 0; j < nop; j++) {
            const size_t last = start + (size_t)loc[(size_t)j];
            if (last < offset) return;
            const size_t cnt = last - offset;
            if (crc8(&buf[offset], cnt + 1) == 0) P.p.packetsOk++; else P.p.packetsBad++;
            P.bytesNow[channel] += cnt;
            offset = last + 1;
        }
    }
}

// kind: 100 + program for a PSD stream, 0..3 a fixed subchannel, -1 the CCC
void L2Decoder::hdlcFeed(std::vector<uint8_t>& acc, bool& open, const uint8_t* p, size_t n, int kind, int channel) {
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = p[i];
        if (b == 0x7E) {
            if (open) {
                if (kind < 0) cccFrame(acc, channel);
                else aasFrame(acc, kind, channel);
            }
            acc.clear();
            open = true;
        } else if (open) {
            if (acc.size() >= 8212) { open = false; acc.clear(); continue; }
            acc.push_back(b);
        }
    }
}

void L2Decoder::aasFrame(std::vector<uint8_t>& f, int, int) {
    const size_t n = unescape(f);
    if (n == 0) return;                                  // padding
    if (n < 7 || fcs16(f.data(), n) != kFcsGood) { aasBad_++; return; }   // stations abandon frames now and then
    aasOk_++;
    if (f[0] != 0x21) return;
    aasPacket((uint16_t)(f[1] | (f[2] << 8)), (uint16_t)(f[3] | (f[4] << 8)), &f[5], n - 7);
}

void L2Decoder::cccFrame(std::vector<uint8_t>& f, int channel) {
    Fixed& F = *fixed_[chan(channel)];
    const size_t n = unescape(f);
    if (n == 0 || F.ready) return;
    if (fcs16(f.data(), n) != kFcsGood) return;
    for (int i = 0; i < 4; i++) {
        F.sub[i].length = 0;
        if ((size_t)(5 + i * 4) <= n) {
            const int mode = f[(size_t)(1 + i * 4)] | (f[(size_t)(2 + i * 4)] << 8);
            const int len = f[(size_t)(3 + i * 4)] | (f[(size_t)(4 + i * 4)] << 8);
            if (mode == 0) { F.sub[i].length = len; F.sub[i].blocks.clear(); F.sub[i].open = false; }
            else say("HD Radio: fixed data subchannel mode " + std::to_string(mode) + " is not supported");
        }
    }
    F.ready = true;
}

// nrsc5 process_fixed_data(): from the end of the frame backwards, the sync byte, the CCC bytes and the subchannels
size_t L2Decoder::fixedData(std::vector<uint8_t>& buf, int channel) {
    Fixed& F = *fixed_[chan(channel)];
    if (buf.empty()) return 0;
    long p = (long)buf.size() - 1;
    auto widthOf = [](uint8_t b) -> int { if (b == 0) return 1; if ((b >> 4) == (b & 0xF)) return (b & 0xF) * 2; return 0; };
    if (F.syncCount < 2) {
        const int w = widthOf(buf[(size_t)p]);
        if (w > 0 && F.syncWidth == w) F.syncCount++;
        else F.syncCount = 0;
        F.syncWidth = w;
        if (F.syncCount < 2) return (size_t)p;
    }
    p -= F.syncWidth;
    if (p < 0) return 0;
    hdlcFeed(F.ccc, F.cccOpen, &buf[(size_t)p], (size_t)F.syncWidth, -1, channel);
    if (!F.ready) return (size_t)p;
    static const uint8_t bbm[4] = {0x7D, 0x3A, 0xE2, 0x42};
    for (int i = 3; i >= 0; i--) {
        Fixed::Sub& s = F.sub[i];
        if (s.length == 0) continue;
        p -= s.length;
        if (p < 0) return 0;
        for (int j = 0; j < s.length; j++) {
            s.blocks.push_back(buf[(size_t)p + (size_t)j]);
            if (s.blocks.size() == 4 && memcmp(s.blocks.data(), bbm, 4) != 0) s.blocks.erase(s.blocks.begin());   // not aligned yet: skip a byte
            if (s.blocks.size() == 255 + 4) {
                hdlcFeed(s.acc, s.open, &s.blocks[4], 255, i, channel);
                s.blocks.clear();
            }
        }
    }
    return (size_t)p;
}

// ---------------------------------------------------------------- AAS packets (nrsc5 output.c output_aas_push())

void L2Decoder::aasPacket(uint16_t port, uint16_t, const uint8_t* p, size_t n) {
    auto& pc = portCount_[port];
    pc.first++;
    pc.second += n;
    if (port == 0x5100 || (port >= 0x5201 && port <= 0x5207)) id3(port & 7, p, n);
    else if (port == 0x20) sig(p, n);
    else if (port >= 0x401 && port <= 0x50FF) {
        lastData_ = now_;
        for (const SigService& s : sigServices_)
            for (const SigService::Comp& c : s.comps)
                if (!c.audio && c.port == port && c.type == 3) { lot(port, p, n); return; }
    }
}

void L2Decoder::id3(int program, const uint8_t* buf, size_t len) {
    if (len < 10 || memcmp(buf, "ID3\x03\x00", 5) != 0 || buf[5]) return;
    const size_t total = (size_t)(((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14) | ((buf[8] & 0x7F) << 7) | (buf[9] & 0x7F)) + 10;
    if (total > len) return;
    Prog& P = prog(program);
    HdrProgram& H = P.p;
    size_t off = 10;
    auto text = [](const uint8_t* d, size_t n) { return n ? textEnc(d[0], d + 1, n - 1) : std::string(); };
    while (off + 10 <= total) {
        const uint8_t* tag = buf + off;
        const uint8_t* d = tag + 10;
        const size_t fl = ((size_t)tag[4] << 24) | ((size_t)tag[5] << 16) | ((size_t)tag[6] << 8) | tag[7];
        if (off + 10 + fl > total) break;
        if (!memcmp(tag, "TIT2", 4)) H.title = text(d, fl);
        else if (!memcmp(tag, "TPE1", 4)) H.artist = text(d, fl);
        else if (!memcmp(tag, "TALB", 4)) H.album = text(d, fl);
        else if (!memcmp(tag, "TCON", 4)) H.genre = text(d, fl);
        else if (!memcmp(tag, "UFID", 4)) {
            const uint8_t* z = (const uint8_t*)memchr(d, 0, fl);
            if (z) H.ufid = latin1(z + 1, (size_t)(d + fl - z - 1));
        } else if (!memcmp(tag, "COMM", 4) && fl >= 5) {
            const int enc = d[0];
            const uint8_t* e = d + fl;
            const uint8_t* z = d + 4;
            if (enc == 1) { while (z + 1 < e && (z[0] || z[1])) z += 2; z += 2; }
            else { while (z < e && *z) z++; z++; }
            if (z <= e) H.comment = textEnc(enc, z, (size_t)(e - z));
        } else if (!memcmp(tag, "COMR", 4) && fl >= 1) {
            // price, valid until (8 digits), contact URL, received as, seller, description
            const int enc = d[0];
            const uint8_t* e = d + fl;
            const uint8_t* q = d + 1;
            auto field = [&](int en) {
                const uint8_t* s = q;
                if (en == 1) { while (q + 1 < e && (q[0] || q[1])) q += 2; const std::string r = ucs2(s, (size_t)(q - s)); q = std::min(e, q + 2); return r; }
                while (q < e && *q) q++;
                const std::string r = latin1(s, (size_t)(q - s));
                q = std::min(e, q + 1);
                return r;
            };
            H.comPrice = field(0);
            if (q + 8 <= e) { H.comValid = std::string((const char*)q, 4) + "-" + std::string((const char*)q + 4, 2) + "-" + std::string((const char*)q + 6, 2); q += 8; }
            H.comUrl = field(0);
            if (q < e) q++;   // received as
            H.comSeller = field(enc);
            H.comDesc = field(enc);
        } else if (!memcmp(tag, "XHDR", 4) && fl >= 6) {
            H.xhdrMime = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
            const int param = d[4], ext = d[5];
            if (6u + (size_t)ext == fl) {
                if (param == 0 && ext == 2) H.xhdrLot = d[6] | (d[7] << 8);
                else if (param == 1 && ext == 0) H.xhdrLot = -1;
            }
        }
        off += 10 + fl;
    }
    if (H.psdCount == 0) say("HD Radio: HD" + std::to_string(program + 1) + " now playing: " + H.title + (H.artist.empty() ? "" : " / " + H.artist));
    H.psdCount++;
    lastPsd_ = now_;
}

// nrsc5 output.c parse_sig(): the station information guide
void L2Decoder::sig(const uint8_t* buf, size_t len) {
    lastData_ = now_;
    if (len == sigRaw_.size() && std::equal(buf, buf + len, sigRaw_.begin())) return;
    sigRaw_.assign(buf, buf + len);
    std::vector<SigService> svc;
    const uint8_t* p = buf;
    const uint8_t* e = buf + len;
    SigService* cur = nullptr;
    while (p < e) {
        const uint8_t type = *p++;
        if ((type & 0xF0) == 0x40) {
            if (p + 3 > e) break;
            SigService s;
            s.audio = type == 0x40;
            s.number = p[0] | (p[1] << 8);
            svc.push_back(s);
            cur = &svc.back();
            p += 3;
        } else if ((type & 0xF0) == 0x60) {
            if (p >= e) break;
            const int l = *p++;
            if (!cur || l < 1 || p + (l - 1) > e) break;
            if (type == 0x69 && l >= 2) cur->name = latin1(p + 1, (size_t)(l - 2));
            else if (type == 0x67 && l - 1 >= 12) cur->comps.push_back({false, p[0], p[1] | (p[2] << 8), p[5], p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t)p[11] << 24), p[3] | (p[4] << 8)});
            else if (type == 0x66 && l - 1 >= 11) cur->comps.push_back({true, p[0], p[1], p[2], p[7] | (p[8] << 8) | (p[9] << 16) | ((uint32_t)p[10] << 24), -1});
            p += l - 1;
        } else break;
    }
    sigServices_ = svc;
    for (const SigService& s : svc) {
        if (!s.audio) continue;
        for (const SigService::Comp& c : s.comps) {
            if (!c.audio || c.port < 0 || c.port > 7) continue;
            Prog& P = prog(c.port);
            P.p.inSig = true;
            if (!s.name.empty()) P.p.name = s.name;
            for (const SigService::Comp& d : s.comps) if (!d.audio && d.mime == kMimePrimaryImage) P.p.artPort = d.port;
        }
    }
    say("HD Radio: station information guide with " + std::to_string(svc.size()) + " services");
}

// nrsc5 output.c process_port(), the LOT case
void L2Decoder::lot(int port, const uint8_t* buf, size_t len) {
    if (len < 8) return;
    int hdrlen = buf[0];
    const int lotId = buf[2] | (buf[3] << 8);
    const uint32_t seq = buf[4] | (buf[5] << 8) | (buf[6] << 16) | ((uint32_t)buf[7] << 24);
    if (hdrlen < 8 || (size_t)hdrlen > len || seq >= 256) return;
    buf += 8; len -= 8; hdrlen -= 8;
    auto key = std::make_pair(port, lotId);
    auto it = lots_.find(key);
    if (it == lots_.end()) {
        if (lots_.size() >= 24) {   // forget the oldest
            auto old = lots_.begin();
            for (auto j = lots_.begin(); j != lots_.end(); ++j) if (j->second.info.timeSec < old->second.info.timeSec) old = j;
            lots_.erase(old);
        }
        it = lots_.emplace(key, Lot()).first;
        it->second.info.port = port;
        it->second.info.lot = lotId;
        it->second.frags.resize(256);
    }
    Lot& L = it->second;
    L.info.timeSec = now_;
    bool fresh = false;
    if (hdrlen > 0) {
        if (hdrlen < 16) return;
        const int year = (buf[7] << 4) | (buf[6] >> 4), mon = buf[6] & 0xF, mday = buf[5] >> 3, hour = ((buf[5] & 7) << 2) | (buf[4] >> 6), min = buf[4] & 0x3F;
        const uint32_t size = buf[8] | (buf[9] << 8) | (buf[10] << 16) | ((uint32_t)buf[11] << 24);
        const uint32_t mime = buf[12] | (buf[13] << 8) | (buf[14] << 16) | ((uint32_t)buf[15] << 24);
        const std::string name = latin1(buf + 16, (size_t)(hdrlen - 16));
        char ex[40];
        snprintf(ex, sizeof ex, "%04d-%02d-%02d %02d:%02d", year, mon, mday, hour, min);
        if (L.info.size && (name != L.info.name || size != L.info.size || mime != L.info.mime || ex != L.info.expires)) {
            for (auto& f : L.frags) f.clear();     // a new file under the same id
            L.info.have = 0; L.info.complete = false;
            fresh = true;
        }
        L.info.name = name; L.info.size = std::min<uint32_t>(size, 65536); L.info.mime = mime; L.info.expires = ex;
        buf += hdrlen; len -= (size_t)hdrlen;
    }
    if (len > 256) return;
    std::vector<uint8_t>& fr = L.frags[seq];
    if (fr.empty() && len) { fr.assign(buf, buf + len); L.info.have += (uint32_t)len; fresh = true; }
    if (fresh && L.info.size && !L.info.complete) {
        const uint32_t nf = (L.info.size + 255) / 256;
        bool all = true;
        for (uint32_t i = 0; i < nf && all; i++) all = !L.frags[i].empty();
        if (all) {
            L.bytes.clear();
            for (uint32_t i = 0; i < nf; i++) L.bytes.insert(L.bytes.end(), L.frags[i].begin(), L.frags[i].end());
            L.bytes.resize(L.info.size);
            L.info.complete = true;
            L.info.version = ++lotVersion_;
            char b[200];
            snprintf(b, sizeof b, "HD Radio: received %s (%u bytes, %s) on port %04X", L.info.name.c_str(), L.info.size, mimeName(L.info.mime), port);
            say(b);
        }
    }
}

bool L2Decoder::lotBytes(int port, int lotId, std::vector<uint8_t>& out) const {
    auto it = lots_.find(std::make_pair(port, lotId));
    if (it == lots_.end() || !it->second.info.complete) return false;
    out = it->second.bytes;
    return true;
}

void L2Decoder::fill(HdrTelemetry& t) const {
    const Sis& S = *sis_;
    t.callSign = S.shortName;
    t.countryCode = S.country;
    t.facilityId = S.facility;
    t.stationName = S.unameDone ? textEnc(S.unameEnc, S.uname, cstrLen(S.uname, 12)) + (S.unameAppend ? "-FM" : "") : std::string();
    t.longName = S.longDone ? latin1(reinterpret_cast<const uint8_t*>(S.longName), cstrLen(S.longName, 56)) : std::string();
    t.slogan = S.sloganDone ? textEnc(S.sloganEnc, S.slogan, (size_t)std::max(0, S.sloganLen)) : std::string();
    t.message = S.msgDone ? textEnc(S.msgEnc, S.msg, (size_t)S.msgLen) : std::string();
    t.alert = S.alertDone && S.alertLen > S.alertCnt ? textEnc(S.alertEnc, S.alert + S.alertCnt, (size_t)(S.alertLen - S.alertCnt)) : std::string();
    t.haveLocation = !std::isnan(S.lat) && !std::isnan(S.lon);
    t.latitude = t.haveLocation ? S.lat : 0;
    t.longitude = t.haveLocation ? S.lon : 0;
    t.altitudeM = S.alt;
    // programs: from the PDUs, the SIG and the SIS service descriptors
    std::map<int, HdrProgram> pm;
    for (const auto& kv : progs_) pm[kv.first] = kv.second.p;
    for (int i = 0; i < 8; i++) {
        if (S.asd[i].type < 0) continue;
        HdrProgram& p = pm[i];
        p.number = i;
        p.inSis = true;
        if (p.type < 0) p.type = S.asd[i].type;
        p.soundExp = S.asd[i].sound;
        p.access = S.asd[i].access;
    }
    t.programs.clear();
    for (const auto& kv : pm) if (kv.second.onAir || kv.second.inSis || kv.second.inSig) t.programs.push_back(kv.second);
    // data services
    t.dataServices.clear();
    for (const SigService& s : sigServices_) {
        int program = -1;
        if (s.audio) for (const SigService::Comp& c : s.comps) if (c.audio) program = c.port;
        for (const SigService::Comp& c : s.comps) {
            if (c.audio) continue;
            HdrDataService d;
            d.fromSig = true; d.service = s.number; d.name = s.name; d.program = program; d.port = c.port; d.aasType = c.type; d.mime = c.mime; d.sdType = c.sdType;
            auto pc = portCount_.find(c.port);
            if (pc != portCount_.end()) { d.packets = pc->second.first; d.bytes = pc->second.second; }
            t.dataServices.push_back(d);
        }
    }
    for (const auto& d : S.dsd) {
        HdrDataService x;
        x.fromSig = false; x.sdType = d.type; x.sisMime = d.mime; x.aasType = -1;
        x.name = "";
        t.dataServices.push_back(x);
    }
    t.lots.clear();
    for (const auto& kv : lots_) t.lots.push_back(kv.second.info);
    std::sort(t.lots.begin(), t.lots.end(), [](const HdrLotInfo& a, const HdrLotInfo& b) { return a.timeSec > b.timeSec; });
    t.lotVersion = lotVersion_;
    t.pidsOk = pidsOk_; t.pidsBad = pidsBad_;
    t.pduCount = pdus_;
    t.aasPackets = aasOk_; t.aasBad = aasBad_;
    t.psdSeen = now_ - lastPsd_ < 10;
    t.dataSeen = now_ - lastData_ < 10;
    t.dataValid = !S.shortName.empty() || !progs_.empty();
}

}} // namespace dect2::hdr
