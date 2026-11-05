// FengYun geostationary formats (fygeo_proto.h).
#include "dect2/fygeo/fygeo_proto.h"
#include "dect2/common/endian.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "openjpeg.h"

namespace dect2::fygeo {

namespace {
inline uint32_t be16(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe16(p); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe32(p); }
inline uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
inline void put16(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putBe16(v, x); }
inline void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x >> 16); put16(v, x & 0xFFFF); }
inline void put64(std::vector<uint8_t>& v, uint64_t x) { put32(v, (uint32_t)(x >> 32)); put32(v, (uint32_t)x); }
std::string text(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
    return s;
}
float beFloat(const uint8_t* p) {
    const uint32_t u = be32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
void putFloat(std::vector<uint8_t>& v, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    put32(v, u);
}
} // namespace

// ---------------------------------------------------------------- FY-4 headers
bool parseFy4Headers(const uint8_t* p, size_t n, Fy4Headers& h) {
    h = Fy4Headers();
    if (n < 16 || p[0] != 0 || be16(p + 1) != 16) return false;
    h.fileType = p[3];
    h.headerLength = be32(p + 4);
    h.dataBits = be64(p + 8);
    if (h.headerLength < 16 || h.headerLength > n) return false;
    size_t i = 16;
    while (i + 3 <= h.headerLength) {
        const int type = p[i];
        const size_t len = be16(p + i + 1);
        if (len < 3 || i + len > h.headerLength) return false;
        const uint8_t* r = p + i + 3;      // the record's body: type and length are not in it
        const size_t rl = len - 3;
        switch (type) {
        case 1:
            if (rl >= 29) {
                h.hasImage = true;
                h.satName = text(r, 9);
                h.instName = text(r + 9, 7);
                h.bpp = r[16];
                h.columns = (int)be16(r + 17);
                h.lines = (int)be16(r + 19);
                h.compression = r[21];
                h.channel = r[22];
                h.totalSeg = r[23];
                h.segNo = r[24];
                h.segPos = r[25];
                h.segLine = (int)be16(r + 26);
                h.algo = r[28] >> 7;
                h.lossless = (r[28] >> 6) & 1;
                h.level = r[28] & 63;
            }
            break;
        case 2:
            if (rl >= 48) {
                h.hasNav = true;
                h.projection = text(r, 32);
                h.cfac = beFloat(r + 32); h.lfac = beFloat(r + 36); h.coff = beFloat(r + 40); h.loff = beFloat(r + 44);
            }
            break;
        case 4: h.annotation = text(r, rl); break;
        case 5: if (rl >= 7) { h.hasTime = true; h.days = (int)be16(r + 1); h.msOfDay = be32(r + 3); } break;
        case 7: if (rl >= 4) { h.hasKey = true; h.key = be32(r); } break;
        default: break;
        }
        i += len;
    }
    h.ok = true;
    return true;
}

std::vector<uint8_t> buildFy4Headers(const Fy4Headers& h) {
    std::vector<uint8_t> s;
    auto rec = [&](int type, const std::vector<uint8_t>& body) { s.push_back((uint8_t)type); put16(s, (uint32_t)(body.size() + 3)); s.insert(s.end(), body.begin(), body.end()); };
    auto str = [](const std::string& t, size_t pad = 0) { std::vector<uint8_t> b(t.begin(), t.end()); if (pad) b.resize(pad, ' '); return b; };
    if (h.hasImage) {
        std::vector<uint8_t> b = str(h.satName, 9);
        const std::vector<uint8_t> in = str(h.instName, 7);
        b.insert(b.end(), in.begin(), in.end());
        b.push_back((uint8_t)h.bpp);
        put16(b, (uint32_t)h.columns); put16(b, (uint32_t)h.lines);
        b.push_back((uint8_t)h.compression);
        b.push_back((uint8_t)h.channel); b.push_back((uint8_t)h.totalSeg); b.push_back((uint8_t)h.segNo); b.push_back((uint8_t)h.segPos);
        put16(b, (uint32_t)h.segLine);
        b.push_back((uint8_t)((h.algo & 1) << 7 | (h.lossless & 1) << 6 | (h.level & 63)));
        rec(1, b);
    }
    if (h.hasNav) {
        std::vector<uint8_t> b = str(h.projection, 32);
        putFloat(b, h.cfac); putFloat(b, h.lfac); putFloat(b, h.coff); putFloat(b, h.loff);
        rec(2, b);
    }
    if (!h.annotation.empty()) rec(4, str(h.annotation));
    if (h.hasTime) { std::vector<uint8_t> b{0x40}; put16(b, (uint32_t)h.days); put32(b, h.msOfDay); rec(5, b); }
    if (h.hasKey) { std::vector<uint8_t> b; put32(b, h.key); rec(7, b); }
    std::vector<uint8_t> out{0};
    put16(out, 16);
    out.push_back((uint8_t)h.fileType);
    put32(out, (uint32_t)(16 + s.size()));
    put64(out, h.dataBits);
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

double fy4HeaderTime(const Fy4Headers& h) {
    if (!h.hasTime) return 0;
    return ((double)h.days - 4383.0) * 86400.0 + h.msOfDay / 1000.0;
}

Fy4Name parseFy4Name(const std::string& a) {
    Fy4Name r;
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        const size_t k = a.find('_', from);
        if (k == std::string::npos) { parts.push_back(a.substr(from)); break; }
        parts.push_back(a.substr(from, k - from));
        from = k + 1;
    }
    if (parts.size() != 13 || parts[0].size() != 5 || parts[0].compare(0, 3, "FY4") != 0) return r;
    int y, mo, d, hh, mi, ss;
    if (sscanf(parts[9].c_str(), "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &hh, &mi, &ss) != 6) return r;
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss;
#ifdef _WIN32
    r.startSec = (double)_mkgmtime(&t);
#else
    r.startSec = (double)timegm(&t);
#endif
    r.sat = std::string("FY-4") + parts[0][3];
    for (char& c : parts[1]) if (c == '-') c = ' ';
    while (!parts[1].empty() && parts[1].back() == ' ') parts[1].pop_back();
    r.instrument = parts[1];
    r.region = parts[3];
    r.channel = parts[7];
    r.group = r.sat + "/" + r.region + "/" + parts[9];
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- JPEG 2000
namespace {
struct MemStream {
    const uint8_t* rd = nullptr;
    size_t size = 0, pos = 0;
    std::vector<uint8_t>* wr = nullptr;
};
OPJ_SIZE_T memRead(void* buf, OPJ_SIZE_T n, void* u) {
    auto* m = static_cast<MemStream*>(u);
    if (m->pos >= m->size) return (OPJ_SIZE_T)-1;
    const size_t k = std::min<size_t>(n, m->size - m->pos);
    memcpy(buf, m->rd + m->pos, k);
    m->pos += k;
    return k;
}
OPJ_OFF_T memSkip(OPJ_OFF_T n, void* u) {
    auto* m = static_cast<MemStream*>(u);
    const int64_t np = std::max<int64_t>(0, std::min<int64_t>((int64_t)m->size, (int64_t)m->pos + n));
    const OPJ_OFF_T d = (OPJ_OFF_T)(np - (int64_t)m->pos);
    m->pos = (size_t)np;
    return d;
}
OPJ_BOOL memSeek(OPJ_OFF_T p, void* u) {
    auto* m = static_cast<MemStream*>(u);
    if (p < 0 || (size_t)p > m->size) return OPJ_FALSE;
    m->pos = (size_t)p;
    return OPJ_TRUE;
}
OPJ_SIZE_T memWrite(void* buf, OPJ_SIZE_T n, void* u) {
    auto* m = static_cast<MemStream*>(u);
    const size_t end = m->pos + n;
    if (m->wr->size() < end) m->wr->resize(end);
    memcpy(m->wr->data() + m->pos, buf, n);
    m->pos = end;
    return n;
}
void quiet(const char*, void*) {}
} // namespace

bool j2kDecode(const uint8_t* d, size_t n, int& w, int& h, int& bpp, std::vector<uint16_t>& px) {
    w = h = bpp = 0;
    px.clear();
    if (n < 12) return false;
    MemStream ms;
    ms.rd = d; ms.size = n;
    opj_stream_t* st = opj_stream_default_create(OPJ_TRUE);
    if (!st) return false;
    opj_stream_set_user_data(st, &ms, nullptr);
    opj_stream_set_user_data_length(st, n);
    opj_stream_set_read_function(st, memRead);
    opj_stream_set_skip_function(st, memSkip);
    opj_stream_set_seek_function(st, memSeek);
    const bool jp2 = n >= 12 && d[4] == 'j' && d[5] == 'P';
    opj_codec_t* c = opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
    bool ok = false;
    opj_image_t* img = nullptr;
    if (c) {
        opj_set_info_handler(c, quiet, nullptr); opj_set_warning_handler(c, quiet, nullptr); opj_set_error_handler(c, quiet, nullptr);
        opj_dparameters_t par;
        opj_set_default_decoder_parameters(&par);
        if (opj_setup_decoder(c, &par) && opj_read_header(st, c, &img) && img && img->numcomps >= 1 && opj_decode(c, st, img)) {
            const opj_image_comp_t& cc = img->comps[0];
            if (cc.w > 0 && cc.h > 0 && (uint64_t)cc.w * cc.h <= 200ull * 1000 * 1000 && cc.data) {
                w = (int)cc.w; h = (int)cc.h; bpp = (int)cc.prec;
                px.resize((size_t)w * h);
                const int off = cc.sgnd ? (1 << (cc.prec - 1)) : 0;
                const int mx = (1 << std::min<int>(16, (int)cc.prec)) - 1;
                for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)std::max(0, std::min(mx, cc.data[i] + off));
                ok = true;
            }
        }
    }
    if (img) opj_image_destroy(img);
    if (c) opj_destroy_codec(c);
    opj_stream_destroy(st);
    return ok;
}

std::vector<uint8_t> j2kEncode(const uint16_t* px, int w, int h, int bpp, int quality) {
    std::vector<uint8_t> out;
    if (w <= 0 || h <= 0 || bpp < 1 || bpp > 16) return out;
    opj_image_cmptparm_t cp;
    memset(&cp, 0, sizeof cp);
    cp.dx = cp.dy = 1; cp.w = (OPJ_UINT32)w; cp.h = (OPJ_UINT32)h; cp.prec = (OPJ_UINT32)bpp; cp.sgnd = 0;
    opj_image_t* img = opj_image_create(1, &cp, OPJ_CLRSPC_GRAY);
    if (!img) return out;
    img->x0 = img->y0 = 0; img->x1 = (OPJ_UINT32)w; img->y1 = (OPJ_UINT32)h;
    for (size_t i = 0; i < (size_t)w * h; i++) img->comps[0].data[i] = px[i];
    opj_cparameters_t par;
    opj_set_default_encoder_parameters(&par);
    par.tcp_numlayers = 1;
    par.cp_disto_alloc = 1;
    par.tcp_rates[0] = quality > 0 ? (float)quality : 0.f;
    par.irreversible = quality > 0 ? 1 : 0;
    par.numresolution = std::max(1, std::min(6, 1 + (int)std::floor(std::log2((double)std::max(1, std::min(w, h) / 8)))));
    par.tcp_mct = 0;
    opj_codec_t* c = opj_create_compress(OPJ_CODEC_J2K);
    MemStream ms;
    ms.wr = &out;
    opj_stream_t* st = opj_stream_default_create(OPJ_FALSE);
    bool ok = false;
    if (c && st) {
        opj_set_info_handler(c, quiet, nullptr); opj_set_warning_handler(c, quiet, nullptr); opj_set_error_handler(c, quiet, nullptr);
        opj_stream_set_user_data(st, &ms, nullptr);
        opj_stream_set_write_function(st, memWrite);
        opj_stream_set_skip_function(st, memSkip);
        opj_stream_set_seek_function(st, memSeek);
        ms.size = 0x7fffffff;
        ok = opj_setup_encoder(c, &par, img) && opj_start_compress(c, img, st) && opj_encode(c, st) && opj_end_compress(c, st);
    }
    if (st) opj_stream_destroy(st);
    if (c) opj_destroy_codec(c);
    opj_image_destroy(img);
    if (!ok) out.clear();
    return out;
}

// ---------------------------------------------------------------- the CADU in the transport stream
void caduToTs(const uint8_t* cadu, int pid, uint8_t& cc, std::vector<uint8_t>& out) {
    std::vector<uint8_t> pay(40, 0);
    pay[0] = 0x45;                 // an IPv4 header and a UDP header's worth of bytes in front (their content is not looked at)
    pay[9] = 17;
    pay.insert(pay.end(), cadu, cadu + kCaduBytes);
    size_t pos = 0;
    bool first = true;
    while (pos < pay.size()) {
        uint8_t t[188];
        memset(t, 0xFF, sizeof t);
        t[0] = 0x47;
        t[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        t[2] = (uint8_t)pid;
        t[3] = (uint8_t)(0x10 | (cc & 15));
        cc = (uint8_t)((cc + 1) & 15);
        size_t o = 4;
        if (first) t[o++] = 0;     // pointer field
        const size_t k = std::min(pay.size() - pos, sizeof t - o);
        memcpy(t + o, pay.data() + pos, k);
        pos += k;
        first = false;
        out.insert(out.end(), t, t + 188);
    }
}

size_t TsCaduExtractor::push(const uint8_t* ts, size_t nPackets, const std::function<void(const uint8_t*, int)>& cb) {
    size_t n = 0;
    for (size_t i = 0; i < nPackets; i++) {
        const uint8_t* t = ts + 188 * i;
        packets++;
        if (t[0] != 0x47) { tsErrors++; continue; }
        if (t[1] & 0x80) { tsErrors++; continue; }
        const int pid = (t[1] & 0x1F) << 8 | t[2];
        if (pid == 0x1FFF) continue;
        const int afc = (t[3] >> 4) & 3;
        if (afc == 2 || afc == 0) continue;
        size_t off = 4;
        if (afc == 3) off += 1 + t[4];
        if (off >= 188) continue;
        Pid& p = pid_[(size_t)pid];
        p.count++;
        auto emit = [&]() {
            if (p.cur.size() < 40 + kCaduBytes) return;
            const size_t lim = std::min<size_t>(p.cur.size() - kCaduBytes, 128);
            for (size_t k = 0; k <= lim; k++)
                if (be32(p.cur.data() + k) == kAsm) { caduCount++; n++; cb(p.cur.data() + k, pid); break; }
        };
        if (t[1] & 0x40) {
            const size_t ptr = t[off];
            if (off + 1 + ptr > 188) { p.active = false; continue; }
            if (p.active) {
                p.cur.insert(p.cur.end(), t + off + 1, t + off + 1 + ptr);
                emit();
            }
            p.cur.assign(t + off + 1 + ptr, t + 188);
            p.active = true;
        } else if (p.active) {
            p.cur.insert(p.cur.end(), t + off, t + 188);
            if (p.cur.size() > 8192) p.active = false;
        }
    }
    return n;
}

std::vector<std::pair<int, uint64_t>> TsCaduExtractor::pids() const {
    std::vector<std::pair<int, uint64_t>> v;
    for (size_t i = 0; i < pid_.size(); i++) if (pid_[i].count) v.emplace_back((int)i, pid_[i].count);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return v;
}

// ---------------------------------------------------------------- S-VISSR
namespace {
inline uint32_t bitsAt(const uint8_t* f, size_t bit, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, bit++) v = v << 1 | ((f[bit >> 3] >> (7 - (bit & 7))) & 1);
    return v;
}
inline void setBitsAt(uint8_t* f, size_t bit, int n, uint32_t v) {
    for (int i = n - 1; i >= 0; i--, bit++) {
        const uint8_t m = (uint8_t)(0x80 >> (bit & 7));
        if ((v >> i) & 1) f[bit >> 3] |= m; else f[bit >> 3] &= (uint8_t)~m;
    }
}
constexpr size_t kIrBlockBits = 20408, kVisBlockBits = 57060, kIr4Bit = 329874;
constexpr size_t kVisBase = kIrBlockBits * 4;
constexpr int kSpareFirst = 2114, kSpareLast = 2292;

std::vector<uint8_t> pnBits(size_t from, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    uint64_t sh = kSvissrPnSeed;
    for (size_t i = 0; i < from + n; i++) {
        const uint8_t b = (uint8_t)(((sh >> 14) ^ (sh >> 13)) & 1);
        sh = sh << 1 | b;
        if (i >= from) out.push_back(b);
    }
    return out;
}
} // namespace

const std::vector<uint8_t>& svissrPnBytes() {
    static const std::vector<uint8_t> t = [] {
        const std::vector<uint8_t> bits = pnBits(kSvissrPreamble, kSvissrFrameBits);
        std::vector<uint8_t> v((size_t)kSvissrFrameBytes);
        for (size_t i = 0; i < v.size(); i++) {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++) b = (uint8_t)(b << 1 | bits[i * 8 + (size_t)k]);
            v[i] = b;
        }
        return v;
    }();
    return t;
}

void svissrDescramble(uint8_t* f) {
    const auto& t = svissrPnBytes();
    for (int i = 0; i < kSvissrFrameBytes; i++) f[i] = (i & 1) ? (uint8_t)(0xFF - (t[(size_t)i] ^ f[i])) : (uint8_t)(t[(size_t)i] ^ f[i]);
}
void svissrScramble(uint8_t* f) {
    const auto& t = svissrPnBytes();
    for (int i = 0; i < kSvissrFrameBytes; i++) f[i] = (i & 1) ? (uint8_t)((0xFF - f[i]) ^ t[(size_t)i]) : (uint8_t)(t[(size_t)i] ^ f[i]);
}
std::vector<uint8_t> svissrPreambleBits() { return pnBits(0, kSvissrPreamble); }

int svissrLineCounter(const uint8_t* f) { return (f[67] << 8 | f[68]) & 0x0FFF; }

void svissrUnpack(const uint8_t* f, uint8_t* ir1, uint8_t* ir2, uint8_t* ir3, uint8_t* ir4, uint8_t* vis4) {
    uint8_t* ir[3] = {ir1, ir2, ir3};
    for (int k = 0; k < 3; k++) {
        const uint8_t* s = f + (kIrBlockBits / 8) * (size_t)(k + 1) + 2;
        for (int i = 0; i < kSvissrIrWidth; i++) ir[k][i] = (uint8_t)(255 - s[i]);
    }
    for (int i = 0; i < kSvissrIrWidth; i++) ir4[i] = (uint8_t)((1023 - bitsAt(f, kIr4Bit + (size_t)i * 10, 10)) >> 2);
    for (int r = 0; r < 4; r++) {
        const size_t b0 = kVisBase + kVisBlockBits * (size_t)r + 24;
        uint8_t* d = vis4 + (size_t)r * kSvissrVisWidth;
        for (int i = 0; i < kSvissrVisWidth; i++) d[i] = (uint8_t)(bitsAt(f, b0 + (size_t)i * 6, 6) << 2 | 2);
    }
}

void svissrPack(uint8_t* f, int counter, const uint8_t* ir1, const uint8_t* ir2, const uint8_t* ir3, const uint8_t* ir4, const uint8_t* vis4) {
    memset(f, 0, kSvissrFrameBytes);
    f[3] = 1;
    f[67] = (uint8_t)(counter >> 8); f[68] = (uint8_t)counter;
    f[193] = (uint8_t)(counter % 25);
    const uint8_t* ir[3] = {ir1, ir2, ir3};
    for (int k = 0; k < 3; k++) {
        uint8_t* d = f + (kIrBlockBits / 8) * (size_t)(k + 1) + 2;
        for (int i = 0; i < kSvissrIrWidth; i++) d[i] = (uint8_t)(255 - ir[k][i]);
    }
    for (int i = 0; i < kSvissrIrWidth; i++) setBitsAt(f, kIr4Bit + (size_t)i * 10, 10, (uint32_t)std::max(0, 1023 - ((int)ir4[i] << 2 | 2)));
    for (int r = 0; r < 4; r++) {
        const size_t b0 = kVisBase + kVisBlockBits * (size_t)r + 24;
        const uint8_t* s = vis4 + (size_t)r * kSvissrVisWidth;
        for (int i = 0; i < kSvissrVisWidth; i++) setBitsAt(f, b0 + (size_t)i * 6, 6, (uint32_t)(s[i] >> 2));
    }
}

int svissrSpareErrors(const uint8_t* f, int* total) {
    int e = 0;
    for (int i = kSpareFirst; i <= kSpareLast; i++) for (int b = 0; b < 8; b++) e += (f[i] >> b) & 1;
    if (total) *total = (kSpareLast - kSpareFirst + 1) * 8;
    return e;
}

} // namespace dect2::fygeo
