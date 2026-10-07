// DRM signalling and message formats: FAC, SDC data entities, text message, AAC audio super frame.
#include "dect2/drm_msg.h"
#include "dect2/drm_fec.h"
#include <algorithm>
#include <cstring>

namespace dect2 { namespace drm {

namespace {
struct BitWriter {
    uint8_t* p; int n = 0;
    explicit BitWriter(uint8_t* b) : p(b) {}
    void put(uint32_t v, int bits) { for (int i = bits - 1; i >= 0; i--) p[n++] = (uint8_t)((v >> i) & 1); }
};
struct BitReader {
    const uint8_t* p; int n = 0;
    explicit BitReader(const uint8_t* b) : p(b) {}
    uint32_t get(int bits) { uint32_t v = 0; for (int i = 0; i < bits; i++) v = (v << 1) | (p[n++] & 1u); return v; }
};
} // namespace

// ---------------------------------------------------------------- FAC

int facInfoBits(bool modeE) { return modeE ? 116 : 72; }

void facBuild(const FacInfo& f, uint8_t* bits) {
    BitWriter w(bits);
    w.put(f.enhancement ? 1 : 0, 1); w.put((uint32_t)f.identity, 2); w.put(f.modeE ? 1 : 0, 1);
    w.put((uint32_t)f.occupancy, 3); w.put((uint32_t)f.interleaver, 1); w.put((uint32_t)f.mscMode, 2); w.put((uint32_t)f.sdcMode, 1);
    w.put((uint32_t)f.numServicesCode, 4); w.put((uint32_t)f.reconfig, 3); w.put((uint32_t)f.toggle, 1); w.put(0, 1);
    const int sets = f.modeE ? 2 : 1;
    for (int i = 0; i < sets; i++) {
        const FacService& s = f.svc[i];
        w.put(s.id, 24); w.put((uint32_t)s.shortId, 2); w.put(s.audioCa ? 1 : 0, 1); w.put((uint32_t)s.language, 4);
        w.put(s.data ? 1 : 0, 1); w.put((uint32_t)s.descriptor, 5); w.put(s.dataCa ? 1 : 0, 1); w.put(0, 6);
    }
    // the CRC covers 64 bits (mode E: 108 bits and 4 zero bits that are not transmitted)
    uint8_t tmp[120];
    std::memcpy(tmp, bits, (size_t)w.n);
    int cn = w.n;
    if (f.modeE) { for (int i = 0; i < 4; i++) tmp[cn++] = 0; }
    w.put(crc8(tmp, (size_t)cn), 8);
}

bool facParse(const uint8_t* bits, bool modeE, FacInfo& f) {
    const int total = facInfoBits(modeE);
    uint8_t tmp[120];
    const int body = total - 8;
    std::memcpy(tmp, bits, (size_t)body);
    int cn = body;
    if (modeE) { for (int i = 0; i < 4; i++) tmp[cn++] = 0; }
    BitReader tail(bits + body);
    if (crc8(tmp, (size_t)cn) != tail.get(8)) return false;
    BitReader r(bits);
    f = FacInfo();
    f.enhancement = r.get(1) != 0; f.identity = (int)r.get(2); f.modeE = r.get(1) != 0;
    f.occupancy = (int)r.get(3); f.interleaver = (int)r.get(1); f.mscMode = (int)r.get(2); f.sdcMode = (int)r.get(1);
    f.numServicesCode = (int)r.get(4); f.reconfig = (int)r.get(3); f.toggle = (int)r.get(1); r.get(1);
    if (f.modeE != modeE) return false;
    f.nSvc = modeE ? 2 : 1;
    for (int i = 0; i < f.nSvc; i++) {
        FacService& s = f.svc[i];
        s.id = r.get(24); s.shortId = (int)r.get(2); s.audioCa = r.get(1) != 0; s.language = (int)r.get(4);
        s.data = r.get(1) != 0; s.descriptor = (int)r.get(5); s.dataCa = r.get(1) != 0; r.get(6);
    }
    return true;
}

int facAudioServices(int c) {
    static const int a[16] = {4, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, -1, 3, 3, -1, 0};
    return a[c & 15];
}
int facDataServices(int c) {
    static const int d[16] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, -1, 0, 1, -1, 4};
    return d[c & 15];
}

const char* languageName(int c) {
    static const char* n[16] = {"", "Arabic", "Bengali", "Chinese (Mandarin)", "Dutch", "English", "French", "German", "Hindi", "Japanese", "Javanese", "Korean",
                                "Portuguese", "Russian", "Spanish", "Other"};
    return n[c & 15];
}
const char* programmeTypeName(int c) {
    static const char* n[32] = {"", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture", "Science", "Varied", "Pop music",
                                "Rock music", "Easy listening", "Light classical", "Serious classical", "Other music", "Weather", "Finance", "Children's",
                                "Social affairs", "Religion", "Phone-in", "Travel", "Leisure", "Jazz", "Country music", "National music", "Oldies", "Folk music",
                                "Documentary", "Alarm", "Skip"};
    return n[c & 31];
}
const char* mscModeName(bool modeE, int m) {
    if (!modeE) return m == 0 ? "64-QAM" : m == 3 ? "16-QAM" : "reserved";
    return m == 0 ? "16-QAM" : m == 3 ? "4-QAM" : "reserved";
}
const char* occupancyName(bool modeE, int o) {
    static const char* a[6] = {"4.5 kHz", "5 kHz", "9 kHz", "10 kHz", "18 kHz", "20 kHz"};
    if (modeE) return o == 0 ? "100 kHz" : "reserved";
    return o < 6 ? a[o] : "reserved";
}

// ---------------------------------------------------------------- SDC

int SdcAudio::rateHz() const {
    if (coding == 3) { static const int r[8] = {0, 0, 16000, 19200, 24000, 32000, 38400, 48000}; return r[rateCode & 7]; }
    static const int r[8] = {0, 12000, 0, 24000, 0, 48000, 0, 0};
    return r[rateCode & 7];
}

void sdcPutEntity(std::vector<uint8_t>& field, int type, int version, const std::vector<uint8_t>& body) {
    // header: length of the body in whole bytes after the first 4 bits (7 bits), version (1), type (4); then the first nibble of the body
    const int len = body.empty() ? 0 : (int)body.size() - 1;
    field.push_back((uint8_t)((len << 1) | (version & 1)));
    field.push_back((uint8_t)(((type & 15) << 4) | (body.empty() ? 0 : (body[0] >> 4) & 15)));
    // the nibble is carried in the second byte; the following body bytes are whole bytes
    for (size_t i = 1; i < body.size(); i++) field.push_back(body[i]);
}

std::vector<uint8_t> sdcEntityMux(const SdcMux& m) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)((((m.protA & 3) << 2) | (m.protB & 3)) << 4));
    for (int i = 0; i < m.nStreams; i++) {
        const SdcStream& s = m.stream[i];
        b.push_back((uint8_t)(s.lenA >> 4));
        b.push_back((uint8_t)(((s.lenA & 15) << 4) | ((s.lenB >> 8) & 15)));
        b.push_back((uint8_t)(s.lenB & 255));
    }
    return b;
}
std::vector<uint8_t> sdcEntityAudio(const SdcAudio& a) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)((((a.shortId & 3) << 2) | (a.streamId & 3)) << 4));
    b.push_back((uint8_t)(((a.coding & 3) << 6) | ((a.sbr & 1) << 5) | ((a.mode & 3) << 3) | (a.rateCode & 7)));
    b.push_back((uint8_t)(((a.text ? 1 : 0) << 7) | ((a.enhancement ? 1 : 0) << 6) | ((a.mps & 7) << 3)));
    for (uint8_t c : a.config) b.push_back(c);
    return b;
}
std::vector<uint8_t> sdcEntityLabel(int shortId, const std::string& text) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)((shortId & 3) << 6));
    for (char c : text) b.push_back((uint8_t)c);
    return b;
}
std::vector<uint8_t> sdcEntityLang(int shortId, const std::string& lang, const std::string& country) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)((shortId & 3) << 6));
    for (int i = 0; i < 3; i++) b.push_back((uint8_t)(i < (int)lang.size() ? lang[(size_t)i] : '-'));
    for (int i = 0; i < 2; i++) b.push_back((uint8_t)(i < (int)country.size() ? country[(size_t)i] : '-'));
    return b;
}
std::vector<uint8_t> sdcEntityTime(const SdcTime& t) {
    uint8_t bits[40];
    BitWriter w(bits);
    w.put((uint32_t)t.mjd, 17); w.put((uint32_t)t.hour, 5); w.put((uint32_t)t.minute, 6);
    if (t.hasOffset) { w.put(0, 2); w.put(t.offsetHalfHours < 0 ? 1 : 0, 1); w.put((uint32_t)std::abs(t.offsetHalfHours), 5); }
    // the first 4 bits are the nibble of the entity header; the rest are whole bytes
    std::vector<uint8_t> b(1 + (size_t)(w.n - 4) / 8);
    b[0] = (uint8_t)((bits[0] << 7) | (bits[1] << 6) | (bits[2] << 5) | (bits[3] << 4));
    for (int i = 4; i < w.n; i++) b[1 + (size_t)(i - 4) / 8] |= (uint8_t)(bits[i] << (7 - (i - 4) % 8));
    return b;
}

int sdcParse(const uint8_t* d, int bytes, SdcInfo& out) {
    int pos = 0, count = 0;
    while (pos + 2 <= bytes) {
        const int len = d[pos] >> 1, ver = d[pos] & 1, type = d[pos + 1] >> 4, nib = d[pos + 1] & 15;
        if (len == 0 && ver == 0 && type == 0 && nib == 0) break;            // padding
        if (pos + 2 + len > bytes) break;
        const uint8_t* b = d + pos + 2;
        count++;
        if (type == 0 && len % 3 == 0 && len >= 3 && len <= 12) {
            SdcMux m;
            m.present = true; m.protA = (nib >> 2) & 3; m.protB = nib & 3; m.nStreams = len / 3;
            for (int i = 0; i < m.nStreams; i++) {
                m.stream[i].lenA = (b[i * 3] << 4) | (b[i * 3 + 1] >> 4);
                m.stream[i].lenB = ((b[i * 3 + 1] & 15) << 8) | b[i * 3 + 2];
            }
            (ver ? out.muxNext : out.mux) = m;
        } else if (type == 1) {
            const int id = (nib >> 2) & 3;
            SdcLabel& l = out.label[id];
            l.present = true; l.shortId = id; l.textControl = 0; l.text.clear();
            int start = 0;
            if (len > 0 && b[0] >= 1 && b[0] <= 15) { l.textControl = b[0]; start = 1; }
            for (int i = start; i < len; i++) l.text.push_back((char)b[i]);
        } else if (type == 5 && len >= 1 && !ver) {
            SdcApp& a = out.app[(nib >> 2) & 3];
            a.present = true; a.shortId = (nib >> 2) & 3; a.streamId = nib & 3; a.packet = (b[0] & 0x80) != 0;
            a.domain = a.packet ? (b[0] & 7) : (b[0] & 7);
        } else if (type == 8 && len >= 3) {
            uint8_t bits[40];
            BitWriter w(bits);
            w.put((uint32_t)nib, 4);
            for (int i = 0; i < len && i < 4; i++) w.put(b[i], 8);
            BitReader r(bits);
            SdcTime& t = out.time;
            t.present = true; t.mjd = (int)r.get(17); t.hour = (int)r.get(5); t.minute = (int)r.get(6);
            t.hasOffset = len >= 4;
            if (t.hasOffset) { r.get(2); const int sense = (int)r.get(1); const int v = (int)r.get(5); t.offsetHalfHours = sense ? -v : v; }
        } else if (type == 9 && len >= 2 && !ver) {
            SdcAudio a;
            a.present = true; a.shortId = (nib >> 2) & 3; a.streamId = nib & 3;
            a.coding = b[0] >> 6; a.sbr = (b[0] >> 5) & 1; a.mode = (b[0] >> 3) & 3; a.rateCode = b[0] & 7;
            a.text = (b[1] & 0x80) != 0; a.enhancement = (b[1] & 0x40) != 0; a.mps = (b[1] >> 3) & 7;
            if (a.coding == 3) a.sbr = 0;
            a.config.assign(b + 2, b + len);
            out.audio[a.shortId] = a;
        } else if (type == 12 && len >= 5 && !ver) {
            SdcLang& l = out.lang[(nib >> 2) & 3];
            l.present = true; l.shortId = (nib >> 2) & 3;
            l.language.assign((const char*)b, 3); l.country.assign((const char*)b + 3, 2);
        } else {
            out.otherTypes.push_back(type);
        }
        pos += 2 + len;
    }
    out.entities = count;
    return count;
}

void sdcBuildBits(int afsIndex, const std::vector<uint8_t>& field, int fieldBytes, std::vector<uint8_t>& bits) {
    bits.clear();
    std::vector<uint8_t> crcIn;
    for (int i = 3; i >= 0; i--) { bits.push_back((uint8_t)((afsIndex >> i) & 1)); }
    for (int i = 0; i < 4; i++) crcIn.push_back(0);
    for (int i = 3; i >= 0; i--) crcIn.push_back((uint8_t)((afsIndex >> i) & 1));
    for (int i = 0; i < fieldBytes; i++) {
        const uint8_t v = i < (int)field.size() ? field[(size_t)i] : 0;
        for (int j = 7; j >= 0; j--) { bits.push_back((v >> j) & 1); crcIn.push_back((v >> j) & 1); }
    }
    const uint32_t c = crc16(crcIn.data(), crcIn.size());
    for (int j = 15; j >= 0; j--) bits.push_back((uint8_t)((c >> j) & 1));
}

bool sdcCheckBits(const uint8_t* bits, int fieldBytes, int& afsIndex) {
    std::vector<uint8_t> crcIn;
    for (int i = 0; i < 4; i++) crcIn.push_back(0);
    afsIndex = 0;
    for (int i = 0; i < 4; i++) { crcIn.push_back(bits[i] & 1); afsIndex = (afsIndex << 1) | (bits[i] & 1); }
    crcIn.insert(crcIn.end(), bits + 4, bits + 4 + (size_t)fieldBytes * 8);
    uint32_t rx = 0;
    for (int j = 0; j < 16; j++) rx = (rx << 1) | (bits[4 + fieldBytes * 8 + j] & 1u);
    return crc16(crcIn.data(), crcIn.size()) == rx;
}

int dateToMjd(int y, int m, int d) {
    const int a = (14 - m) / 12;
    const int yy = y + 4800 - a, mm = m + 12 * a - 3;
    const int jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    return jdn - 2400001;
}
void mjdToDate(int mjd, int& y, int& m, int& d) {
    const int j = mjd + 2400001 + 32044;
    const int g = j / 146097, dg = j % 146097;
    const int c = (dg / 36524 + 1) * 3 / 4, dc = dg - c * 36524;
    const int b = dc / 1461, db = dc % 1461;
    const int a = (db / 365 + 1) * 3 / 4, da = db - a * 365;
    const int yy = g * 400 + c * 100 + b * 4 + a, mm = (da * 5 + 308) / 153 - 2, dd = da - (mm + 4) * 153 / 5 + 122;
    y = yy - 4800 + (mm + 2) / 12; m = (mm + 2) % 12 + 1; d = dd + 1;
}

// ---------------------------------------------------------------- text message

void TextDecoder::reset() { *this = TextDecoder(); }

bool TextDecoder::feed(const uint8_t* four) {
    changed_ = false;
    const bool marker = four[0] == 0xFF && four[1] == 0xFF && four[2] == 0xFF && four[3] == 0xFF;
    if (marker) { seg_.clear(); need_ = -1; return false; }
    if (need_ == 0) return false;                       // idle: no segment in progress (all zero units)
    seg_.insert(seg_.end(), four, four + 4);
    if (need_ < 0 && seg_.size() >= 2) {
        const bool command = (seg_[0] & 0x10) != 0;
        const int n = command ? 0 : (seg_[0] & 15) + 1;
        need_ = 2 + n + 2;
    }
    if (need_ > 0 && (int)seg_.size() >= need_) { finishSegment(); need_ = 0; seg_.clear(); }
    else if (seg_.size() > 40) { need_ = 0; seg_.clear(); bad_++; }
    return changed_;
}

void TextDecoder::finishSegment() {
    const int n = need_ - 4;
    uint32_t rx = ((uint32_t)seg_[(size_t)(2 + n)] << 8) | seg_[(size_t)(3 + n)];
    if (crc16Bytes(seg_.data(), (size_t)(2 + n)) != rx) { bad_++; return; }
    ok_++;
    const bool toggle = (seg_[0] & 0x80) != 0, first = (seg_[0] & 0x40) != 0, last = (seg_[0] & 0x20) != 0, command = (seg_[0] & 0x10) != 0;
    if (command) {
        if ((seg_[0] & 15) == 1 && !text_.empty()) { text_.clear(); changed_ = true; }       // clear display
        return;
    }
    const int segnum = first ? 0 : (seg_[1] >> 4) & 7;
    if (!haveToggle_ || toggle != toggle_) {            // a different message: start again
        haveToggle_ = true; toggle_ = toggle; got_ = 0; last_ = -1;
        for (auto& p : parts_) p.clear();
    }
    parts_[segnum].assign((const char*)seg_.data() + 2, (size_t)n);
    got_ |= (uint8_t)(1u << segnum);
    if (last) last_ = segnum;
    if (last_ >= 0 && (got_ & ((1u << (last_ + 1)) - 1)) == ((1u << (last_ + 1)) - 1)) {
        std::string t;
        for (int i = 0; i <= last_; i++) t += parts_[i];
        if (t != text_) { text_ = t; changed_ = true; }
    }
}

void TextEncoder::set(const std::string& message) {
    stream_.clear(); pos_ = 0;
    if (message.empty()) return;
    toggle_ = !toggle_;
    const bool toggle = toggle_;
    const int nseg = (int)std::min<size_t>(8, (message.size() + 15) / 16);
    for (int s = 0; s < nseg; s++) {
        const size_t off = (size_t)s * 16;
        const std::string body = message.substr(off, 16);
        std::vector<uint8_t> seg;
        const bool first = s == 0, last = s == nseg - 1;
        seg.push_back((uint8_t)((toggle ? 0x80 : 0) | (first ? 0x40 : 0) | (last ? 0x20 : 0) | ((int)body.size() - 1)));
        seg.push_back(first ? 0xF0 : (uint8_t)((s & 7) << 4));
        for (char c : body) seg.push_back((uint8_t)c);
        const uint32_t c = crc16Bytes(seg.data(), seg.size());
        seg.push_back((uint8_t)(c >> 8)); seg.push_back((uint8_t)c);
        while (seg.size() % 4) seg.push_back(0);
        for (int i = 0; i < 4; i++) stream_.push_back(0xFF);
        stream_.insert(stream_.end(), seg.begin(), seg.end());
    }
}

void TextEncoder::next(uint8_t* four) {
    if (stream_.empty()) { std::memset(four, 0, 4); return; }
    for (int i = 0; i < 4; i++) four[i] = stream_[pos_ + (size_t)i];
    pos_ += 4;
    if (pos_ >= stream_.size()) pos_ = 0;
}

// ---------------------------------------------------------------- AAC audio super frame

int aacNumFrames(bool modeE, int rateHz) {
    if (!modeE) return rateHz == 12000 ? 5 : rateHz == 24000 ? 10 : 0;
    return rateHz == 24000 ? 5 : rateHz == 48000 ? 10 : 0;
}
int aacHeaderBytes(int n) { return ((n - 1) * 12 + (n == 10 ? 4 : 0) + 7) / 8; }

bool aacSuperFrameParse(const uint8_t* lf, int len, int lenA, int n, AacSuperFrame& out) {
    out = AacSuperFrame();
    out.numFrames = n;
    const int hdr = aacHeaderBytes(n);
    if (n < 1 || len < hdr + n) return false;
    int hp = 0;
    if (lenA > 0) {
        if (lenA < hdr + n || (lenA - hdr) % n != 0) return false;
        hp = (lenA - hdr) / n - 1;
    }
    out.hpBytes = hp;
    // frame borders: 12 bits each, bytes counted from the start of the audio frames; they only grow, so a wrapped value is recognised
    std::vector<int> border((size_t)n, 0);
    int prev = 0;
    bool ok = true;
    for (int i = 0; i < n - 1; i++) {
        const int bit = i * 12;
        int v = (lf[bit / 8] << 8 | lf[bit / 8 + 1]);
        v = (bit % 8 == 0) ? (v >> 4) & 0xFFF : v & 0xFFF;
        while (v < prev) v += 4096;
        border[(size_t)i] = v; prev = v;
    }
    const int payload = len - hdr - n;
    border[(size_t)n - 1] = payload;
    int start = 0;
    for (int i = 0; i < n; i++) {
        if (border[(size_t)i] < start || border[(size_t)i] > payload || border[(size_t)i] - start < hp) ok = false;
        start = border[(size_t)i];
    }
    out.headerOk = ok;
    if (!ok) return false;
    // higher protected part: for every frame its first hp bytes and then its CRC byte; lower protected part: the rest of the frames in order
    const uint8_t* hpPart = lf + hdr;
    const uint8_t* lpPart = lf + hdr + (size_t)n * (size_t)(hp + 1);
    const uint8_t* end = lf + len;
    start = 0;
    out.frames.resize((size_t)n);
    for (int i = 0; i < n; i++) {
        const int fl = border[(size_t)i] - start;
        start = border[(size_t)i];
        auto& fr = out.frames[(size_t)i];
        fr.assign(hpPart + (size_t)i * (size_t)(hp + 1), hpPart + (size_t)i * (size_t)(hp + 1) + (size_t)hp);
        out.crc.push_back(hpPart[(size_t)i * (size_t)(hp + 1) + (size_t)hp]);
        const int lpn = fl - hp;
        if (lpPart + lpn > end) { out.headerOk = false; return false; }
        fr.insert(fr.end(), lpPart, lpPart + lpn);
        lpPart += lpn;
    }
    return true;
}

bool aacSuperFrameBuild(const std::vector<std::vector<uint8_t>>& frames, const std::vector<uint8_t>& crc, int hp, int len, std::vector<uint8_t>& lf) {
    const int n = (int)frames.size();
    const int hdr = aacHeaderBytes(n);
    int total = 0;
    for (const auto& f : frames) { if ((int)f.size() < hp) return false; total += (int)f.size(); }
    if (hdr + n + total > len) return false;
    lf.assign((size_t)len, 0);
    uint8_t bits[160];
    int nb = 0;
    int acc = 0;
    for (int i = 0; i < n - 1; i++) {
        acc += (int)frames[(size_t)i].size();
        for (int j = 11; j >= 0; j--) bits[nb++] = (uint8_t)((acc >> j) & 1);
    }
    if (n == 10) for (int j = 0; j < 4; j++) bits[nb++] = 0;
    for (int i = 0; i < nb; i++) lf[(size_t)i / 8] |= (uint8_t)(bits[i] << (7 - i % 8));
    size_t hpPos = (size_t)hdr, lpPos = (size_t)hdr + (size_t)n * (size_t)(hp + 1);
    for (int i = 0; i < n; i++) {
        const auto& f = frames[(size_t)i];
        std::copy(f.begin(), f.begin() + hp, lf.begin() + (ptrdiff_t)hpPos); hpPos += (size_t)hp;
        lf[hpPos++] = i < (int)crc.size() ? crc[(size_t)i] : 0;
        std::copy(f.begin() + hp, f.end(), lf.begin() + (ptrdiff_t)lpPos); lpPos += f.size() - (size_t)hp;
    }
    return true;
}

}} // namespace dect2::drm
