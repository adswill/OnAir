// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/source/bluefile.h"
#include "dect2/common/u8file.h"
#include "dect2/dsp/sample_convert.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace dect2 {
namespace blue {
namespace {

uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

// Fixed-width reads in a chosen byte order, independent of the host (the bytes are composed explicitly).
uint64_t getU(const uint8_t* p, int n, bool big) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[big ? n - 1 - i : i] << (8 * i);
    return v;
}
void putU(uint8_t* p, uint64_t v, int n, bool big) {
    for (int i = 0; i < n; i++) p[big ? n - 1 - i : i] = (uint8_t)(v >> (8 * i));
}
double getF64(const uint8_t* p, bool big) { uint64_t u = getU(p, 8, big); double d; memcpy(&d, &u, 8); return d; }
float getF32(const uint8_t* p, bool big) { uint32_t u = (uint32_t)getU(p, 4, big); float f; memcpy(&f, &u, 4); return f; }
void putF64(uint8_t* p, double d, bool big) { uint64_t u; memcpy(&u, &d, 8); putU(p, u, 8, big); }
void putF32(uint8_t* p, float f, bool big) { uint32_t u; memcpy(&u, &f, 4); putU(p, u, 4, big); }
int seek64(FILE* f, uint64_t o) {
#ifdef _WIN32
    return _fseeki64(f, (long long)o, SEEK_SET);
#else
    return fseeko(f, (off_t)o, SEEK_SET);
#endif
}

bool okDouble(double d) { return std::isfinite(d) && d >= 0 && d < 9.0e15; }

std::string trimText(const uint8_t* p, size_t n) {
    size_t e = 0;
    while (e < n && p[e]) e++;
    while (e > 0 && (p[e - 1] == ' ' || p[e - 1] == '\r' || p[e - 1] == '\n')) e--;
    return std::string((const char*)p, e);
}

std::string numText(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.12g", v);
    return b;
}

// One scalar of a keyword value as text; false for kinds that are not numbers.
bool kwNumber(char t, const uint8_t* p, size_t avail, bool big, double& v) {
    switch (t) {
    case 'B': if (avail < 1) return false; v = (int8_t)p[0]; return true;
    case 'O': if (avail < 1) return false; v = p[0]; return true;
    case 'I': if (avail < 2) return false; v = (int16_t)getU(p, 2, big); return true;
    case 'L': if (avail < 4) return false; v = (int32_t)getU(p, 4, big); return true;
    case 'X': if (avail < 8) return false; v = (double)(int64_t)getU(p, 8, big); return true;
    case 'F': if (avail < 4) return false; v = getF32(p, big); return true;
    case 'D': if (avail < 8) return false; v = getF64(p, big); return true;
    default: return false;
    }
}

std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

void parseKeywords(const uint8_t* e, size_t n, bool big, std::vector<std::pair<std::string, std::string>>& out) {
    size_t o = 0;
    int guard = 0;
    while (o + 8 <= n && guard++ < 100000) {
        const uint32_t lkey = (uint32_t)getU(e + o, 4, big);
        const int lextra = (int16_t)getU(e + o + 4, 2, big);
        const int ltag = (int8_t)e[o + 6];
        const char type = (char)e[o + 7];
        if (lkey < 8 || lkey > n - o) break;                       // damaged record: stop, keep what was read
        const int64_t vlen = (int64_t)lkey - lextra;
        if (lextra < 8 || vlen < 0 || ltag < 0 || 8 + vlen + ltag > (int64_t)lkey) { o += lkey; continue; }
        const std::string tag = trimText(e + o + 8 + vlen, (size_t)ltag);
        std::string val;
        if (type == 'A') val = trimText(e + o + 8, (size_t)vlen);
        else { double d; if (kwNumber(type, e + o + 8, (size_t)vlen, big, d)) val = numText(d); else { o += lkey; continue; } }
        if (!tag.empty()) out.emplace_back(tag, val);
        o += lkey;
    }
}

bool keywordNumber(const Info& i, std::initializer_list<const char*> names, double& v) {
    for (const char* nm : names)
        for (auto& kv : i.keywords)
            if (upper(kv.first) == nm) { char* end = nullptr; const double d = strtod(kv.second.c_str(), &end); if (end != kv.second.c_str() && std::isfinite(d)) { v = d; return true; } }
    return false;
}

FILE* openRb(const std::string& p) { return u8fopen(p, "rb"); }

uint64_t fileSizeOf(const std::string& p) {
    std::error_code ec;
    const uint64_t s = std::filesystem::file_size(u8file(p), ec);
    return ec ? 0 : s;
}

} // namespace

bool isBlueExtension(const std::string& path) {
    const size_t d = path.find_last_of('.');
    if (d == std::string::npos) return false;
    std::string e = path.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e == ".tmp" || e == ".prm" || e == ".blue" || e == ".bluefile";
}

bool hasMagic(const std::string& path) {
    FILE* f = openRb(path);
    if (!f) return false;
    char m[4] = {};
    const bool ok = fread(m, 1, 4, f) == 4 && !memcmp(m, "BLUE", 4);
    fclose(f);
    return ok;
}

bool parse(const uint8_t* h, size_t hl, const uint8_t* ext, size_t extLen, uint64_t fileSize, Info& out, std::string& err) {
    Info r;
    if (hl < 512) { err = "BLUE header is shorter than 512 bytes"; return false; }
    if (memcmp(h, "BLUE", 4)) { err = "not an X-Midas BLUE file (no BLUE tag)"; return false; }
    auto rep = [&](const uint8_t* p, bool& big) {
        if (!memcmp(p, "EEEI", 4)) { big = false; return true; }
        if (!memcmp(p, "IEEE", 4)) { big = true; return true; }
        return false;
    };
    if (!rep(h + 4, r.headBig)) { err = "BLUE header byte order (head_rep) is neither EEEI nor IEEE"; return false; }
    if (!rep(h + 8, r.dataBig)) { err = "BLUE data byte order (data_rep) is neither EEEI nor IEEE"; return false; }
    const bool hb = r.headBig;
    if ((int32_t)getU(h + 12, 4, hb) != 0) { err = "BLUE file with a detached header is not supported"; return false; }
    const uint32_t extStart = (uint32_t)getU(h + 24, 4, hb);
    const uint32_t extSize = (uint32_t)getU(h + 28, 4, hb);
    const double dStart = getF64(h + 32, hb), dSize = getF64(h + 40, hb);
    r.type = (int32_t)getU(h + 48, 4, hb);
    if (r.type != 1000 && r.type != 2000) { err = "BLUE file type " + std::to_string(r.type) + " is not supported (1000 and 2000 are)"; return false; }
    r.spa = (char)toupper(h[52]); r.kind = (char)toupper(h[53]);
    if (r.spa != 'S' && r.spa != 'C') { err = std::string("BLUE data format ") + (char)h[52] + (char)h[53] + " is not scalar or complex samples"; return false; }
    switch (r.kind) {
    case 'B': case 'O': r.bytesPerScalar = 1; break;
    case 'I': r.bytesPerScalar = 2; break;
    case 'L': case 'F': r.bytesPerScalar = 4; break;
    case 'X': case 'D': r.bytesPerScalar = 8; break;
    default: err = std::string("BLUE sample type '") + (char)h[53] + "' is not supported (B, O, I, L, X, F, D are)"; return false;
    }
    r.complex = r.spa == 'C';
    r.timecode = getF64(h + 56, hb);
    if (!okDouble(dStart) || dStart < 512) { err = "BLUE data_start is not valid"; return false; }
    r.dataStart = (uint64_t)dStart;
    if (fileSize && r.dataStart > fileSize) { err = "BLUE data_start is past the end of the file"; return false; }
    uint64_t avail = fileSize > r.dataStart ? fileSize - r.dataStart : 0;
    uint64_t bytes = 0;
    if (okDouble(dSize) && dSize > 0) bytes = (uint64_t)dSize;
    else { bytes = avail; r.sizeFromFile = true; }
    if (fileSize && bytes > avail) { bytes = avail; r.truncated = !r.sizeFromFile; }
    const size_t ab = r.atomBytes();
    r.samples = bytes / ab;
    r.dataBytes = r.samples * ab;
    r.xstart = getF64(h + 256, hb);
    r.xdelta = getF64(h + 264, hb);
    r.xunits = (int32_t)getU(h + 272, 4, hb);
    if (r.type == 2000) r.subsize = (int32_t)getU(h + 276, 4, hb);
    (void)extStart;
    if (ext && extLen && extSize) parseKeywords(ext, std::min<size_t>(extLen, extSize), hb, r.keywords);
    // the sample rate: 1/xdelta when the axis is time (xunits 1) or not described (0); a frequency axis (3) is a spectrum, not a rate
    if (std::isfinite(r.xdelta) && r.xdelta > 0 && (r.xunits == 1 || r.xunits == 0)) r.sampleRate = 1.0 / r.xdelta;
    double v;
    if (r.sampleRate <= 0 && keywordNumber(r, {"SAMPLE_RATE", "SAMPLERATE", "SAMPLE_RATE_HZ", "SRATE", "FS"}, v) && v > 0) r.sampleRate = v;
    if (keywordNumber(r, {"RF_FREQ", "RF_FREQUENCY", "RFFREQ", "CENTER_FREQ", "CENTER_FREQUENCY", "CENTERFREQ", "CENTRE_FREQ", "COL_FREQ", "COLLECT_FREQ", "CHAN_FREQ", "FREQ"}, v) && v > 0) r.centerHz = v;
    for (auto& kv : r.keywords) { const std::string k = upper(kv.first); if ((k == "DESCRIPTION" || k == "COMMENT") && r.description.empty()) r.description = kv.second; }
    out = r;
    return true;
}

bool probe(const std::string& path, Info& out, std::string& err) {
    FILE* f = openRb(path);
    if (!f) { err = "cannot open " + path; return false; }
    uint8_t h[512];
    const size_t g = fread(h, 1, 512, f);
    if (g < 512) { fclose(f); err = "file is too short to be a BLUE file: " + path; return false; }
    const uint64_t size = fileSizeOf(path);
    std::vector<uint8_t> ext;
    // the extended header is optional: read it only when it lies inside the file and is of a sane size
    bool hb = !memcmp(h + 4, "IEEE", 4);
    const uint64_t es = getU(h + 24, 4, hb) * 512ull, en = getU(h + 28, 4, hb);
    if (es && en && en <= (4u << 20) && es + en <= size) {
        ext.resize((size_t)en);
        if (seek64(f, es) != 0 || fread(ext.data(), 1, ext.size(), f) != ext.size()) ext.clear();
    }
    fclose(f);
    return parse(h, 512, ext.empty() ? nullptr : ext.data(), ext.size(), size, out, err);
}

// ------------------------------------------------------------------ reader

bool Reader::open(const std::string& path, std::string& err) {
    close();
    if (!probe(path, info_, err)) return false;
    f_ = openRb(path);
    if (!f_) { err = "cannot open " + path; return false; }
    rewind();
    return true;
}
void Reader::close() { if (f_) { fclose(f_); f_ = nullptr; } }
void Reader::rewind() {
    if (!f_) return;
    seek64(f_, info_.dataStart);
    left_ = info_.samples;
}

size_t Reader::read(cf32* dst, size_t n) {
    if (!f_ || !left_ || !n) return 0;
    const size_t want = (size_t)std::min<uint64_t>(n, left_);
    const size_t ab = info_.atomBytes(), bs = (size_t)info_.bytesPerScalar;
    raw_.resize(want * ab);
    const size_t got = fread(raw_.data(), ab, want, f_);
    left_ = got < want ? 0 : left_ - got;
    const bool big = info_.dataBig;
    auto scalar = [&](const uint8_t* p) -> float {
        switch (info_.kind) {
        case 'B': return (int8_t)p[0] / 128.0f;
        case 'O': return ((int)p[0] - 128) / 128.0f;
        case 'I': return (int16_t)getU(p, 2, big) / 32768.0f;
        case 'L': return (float)((int32_t)getU(p, 4, big) / 2147483648.0);
        case 'X': return (float)((int64_t)getU(p, 8, big) / 9223372036854775808.0);
        case 'F': return getF32(p, big);
        default: return (float)getF64(p, big);
        }
    };
    // the common complex formats in their own loops (the same arithmetic: x / 128 and x / 32768 are exact as multiplications)
    if (info_.complex && !big && ab == 2 * bs && info_.kind == 'B') { convertCs8(reinterpret_cast<const int8_t*>(raw_.data()), got, dst); return got; }
    if (info_.complex && !big && ab == 2 * bs && info_.kind == 'F') { std::memcpy(dst, raw_.data(), got * sizeof(cf32)); return got; }
    if (info_.complex && !big && ab == 2 * bs && info_.kind == 'I') {
        s16_.resize(got * 2);
        std::memcpy(s16_.data(), raw_.data(), got * 4);
        convertCs16(s16_.data(), got, dst, 1.0f / 32768);
        return got;
    }
    for (size_t i = 0; i < got; i++) {
        const uint8_t* p = &raw_[i * ab];
        dst[i] = info_.complex ? cf32(scalar(p), scalar(p + bs)) : cf32(scalar(p), 0.f);
    }
    return got;
}

// ------------------------------------------------------------------ writer

bool Writer::open(const std::string& path, const WriteSpec& s, std::string& err) {
    std::string e;
    close(e);
    if (!strchr("BILFD", s.kind)) { err = "BLUE writer: unsupported sample type"; return false; }
    if (!(s.sampleRate > 0)) { err = "BLUE writer: the sample rate is not known"; return false; }
    try {
        const std::filesystem::path p = u8file(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    } catch (const std::exception& ex) { err = std::string("cannot create the folder: ") + ex.what(); return false; }
    f_ = u8fopen(path, "wb");
    if (!f_) { err = "cannot create " + path; return false; }
    s_ = s; samples_ = 0;
    uint8_t h[512] = {};
    memcpy(h, "BLUE", 4);
    memcpy(h + 4, s.bigEndian ? "IEEE" : "EEEI", 4);
    memcpy(h + 8, s.bigEndian ? "IEEE" : "EEEI", 4);
    putF64(h + 32, 512, s.bigEndian);
    putU(h + 48, 1000, 4, s.bigEndian);
    h[52] = (uint8_t)(s.complex ? 'C' : 'S'); h[53] = (uint8_t)s.kind;
    putF64(h + 56, s.timecode, s.bigEndian);
    putF64(h + 256, 0.0, s.bigEndian);
    putF64(h + 264, 1.0 / s.sampleRate, s.bigEndian);
    putU(h + 272, 1, 4, s.bigEndian);   // xunits 1 = time in seconds
    if (fwrite(h, 1, 512, f_) != 512) { err = "writing failed: " + path; fclose(f_); f_ = nullptr; return false; }
    return true;
}

bool Writer::write(const cf32* x, size_t n, std::string& err) {
    if (!f_) { err = "BLUE writer is not open"; return false; }
    const bool big = s_.bigEndian;
    const int bs = s_.kind == 'B' ? 1 : s_.kind == 'I' ? 2 : (s_.kind == 'L' || s_.kind == 'F') ? 4 : 8;
    const int per = s_.complex ? 2 : 1;
    buf_.resize(n * per * bs);
    uint8_t* p = buf_.data();
    auto put = [&](float v) {
        switch (s_.kind) {
        case 'B': *p = (uint8_t)(int8_t)std::max(-127L, std::min(127L, lrintf(v * 127.f))); break;
        case 'I': putU(p, (uint16_t)(int16_t)std::max(-32767L, std::min(32767L, lrintf(v * 32767.f))), 2, big); break;
        case 'L': putU(p, (uint32_t)(int32_t)std::max(-2147483647.0, std::min(2147483647.0, std::nearbyint((double)v * 2147483647.0))), 4, big); break;
        case 'F': putF32(p, v, big); break;
        default: putF64(p, (double)v, big); break;
        }
        p += bs;
    };
    for (size_t i = 0; i < n; i++) { put(x[i].real()); if (per == 2) put(x[i].imag()); }
    if (fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) { err = "writing failed (disk full?)"; return false; }
    samples_ += n;
    return true;
}

bool Writer::writeBytes(const void* p, size_t bytes, std::string& err) {
    if (!f_) { err = "BLUE writer is not open"; return false; }
    const size_t bs = s_.kind == 'B' ? 1 : s_.kind == 'I' ? 2 : (s_.kind == 'L' || s_.kind == 'F') ? 4 : 8;
    const size_t atom = bs * (s_.complex ? 2 : 1);
    if (fwrite(p, 1, bytes, f_) != bytes) { err = "writing failed (disk full?)"; return false; }
    samples_ += bytes / atom;
    return true;
}

bool Writer::close(std::string& err) {
    if (!f_) return true;
    FILE* f = f_;
    f_ = nullptr;
    const bool big = s_.bigEndian;
    const int bs = s_.kind == 'B' ? 1 : s_.kind == 'I' ? 2 : (s_.kind == 'L' || s_.kind == 'F') ? 4 : 8;
    const uint64_t dataBytes = samples_ * (uint64_t)bs * (s_.complex ? 2 : 1);
    // keywords: RF_FREQ (when known), SAMPLE_RATE and DESCRIPTION; each record is padded to a multiple of 8 bytes
    std::vector<uint8_t> ext;
    auto addKw = [&](const char* tag, char type, const uint8_t* val, size_t vlen) {
        const size_t ltag = strlen(tag);
        size_t total = 8 + vlen + ltag;
        total = (total + 7) & ~(size_t)7;
        const size_t o = ext.size();
        ext.resize(o + total, 0);
        putU(&ext[o], total, 4, big);
        putU(&ext[o + 4], (uint16_t)(total - vlen), 2, big);   // lextra: everything but the value
        ext[o + 6] = (uint8_t)ltag;
        ext[o + 7] = (uint8_t)type;
        memcpy(&ext[o + 8], val, vlen);
        memcpy(&ext[o + 8 + vlen], tag, ltag);
    };
    uint8_t d8[8];
    if (s_.centerHz > 0) { putF64(d8, s_.centerHz, big); addKw("RF_FREQ", 'D', d8, 8); }
    putF64(d8, s_.sampleRate, big); addKw("SAMPLE_RATE", 'D', d8, 8);
    if (!s_.description.empty()) { const std::string d = s_.description.substr(0, 200); addKw("DESCRIPTION", 'A', (const uint8_t*)d.data(), d.size()); }
    bool ok = true;
    // the data is padded to a 512-byte block, then the extended header
    const uint64_t padded = (512 + dataBytes + 511) / 512 * 512;
    if (seek64(f, 512 + dataBytes) != 0) ok = false;
    if (ok) { std::vector<uint8_t> z((size_t)(padded - 512 - dataBytes), 0); if (!z.empty() && fwrite(z.data(), 1, z.size(), f) != z.size()) ok = false; }
    if (ok && fwrite(ext.data(), 1, ext.size(), f) != ext.size()) ok = false;
    if (ok) {
        uint8_t u[4]; uint8_t sz[8];
        putU(u, padded / 512, 4, big);
        seek64(f, 24); ok = fwrite(u, 1, 4, f) == 4;
        putU(u, ext.size(), 4, big);
        seek64(f, 28); ok = ok && fwrite(u, 1, 4, f) == 4;
        putF64(sz, (double)dataBytes, big);
        seek64(f, 40); ok = ok && fwrite(sz, 1, 8, f) == 8;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) err = "writing the BLUE header failed";
    return ok;
}

// ------------------------------------------------------------------ conversion

bool convertToCf32(const std::string& src, const std::string& dstPath, std::string& err) {
    Reader r;
    if (!r.open(src, err)) return false;
    FILE* o = u8fopen(dstPath, "wb");
    if (!o) { err = "cannot create " + dstPath; return false; }
    std::vector<cf32> buf(1 << 16);
    bool ok = true;
    for (size_t n; ok && (n = r.read(buf.data(), buf.size())) > 0;) {
        if (std::endian::native != std::endian::little) for (auto& c : buf) { float v[2] = {c.real(), c.imag()}; uint32_t u[2]; memcpy(u, v, 8); u[0] = bswap32(u[0]); u[1] = bswap32(u[1]); memcpy(v, u, 8); c = cf32(v[0], v[1]); }
        ok = fwrite(buf.data(), sizeof(cf32), n, o) == n;
    }
    if (fclose(o) != 0) ok = false;
    if (!ok) { err = "writing " + dstPath + " failed (disk full?)"; std::error_code ec; std::filesystem::remove(u8file(dstPath), ec); }
    return ok;
}

} // namespace blue
} // namespace dect2
