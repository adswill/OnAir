// See sd3_proto.h. Formats from SatDump (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/sd3/sd3_proto.h"
#include "dect2/ccsds/ccsds_tm.h"
#include "dect2/image/j2k.h"
#include <algorithm>
#include <cstring>
#include <map>

namespace dect2::sd3 {

int infoBytes(const Sd3Link& l) {
    const int bytes = (l.frameBits + 7) / 8;
    if (l.rsI <= 0) return bytes - 4;
    const int e = l.rsType == 239 ? 8 : 16;
    const int n = (bytes - 4) / l.rsI;
    return l.rsI * (n - 2 * e);
}

static void applyRandomiser(std::vector<uint8_t>& f) {
    if (f.size() <= 4) return;
    const auto r = ccsds::randomiser(f.size() - 4);
    for (size_t i = 4; i < f.size(); i++) f[i] ^= r[i - 4];
}

static int rsWordLength(const Sd3Link& l, size_t bytes) {
    const int n = (int)(bytes - 4) / l.rsI;
    return std::min(n, 255);
}

bool decodeFrame(const Sd3Link& l, std::vector<uint8_t>& f, int& corrected) {
    corrected = 0;
    if (l.derand && !l.derandAfterRs) applyRandomiser(f);
    bool ok = true;
    if (l.rsI > 0) {
        const int e = l.rsType == 239 ? 8 : 16;
        const int n = rsWordLength(l, f.size());
        ccsds::Rs rs(e, n);
        std::vector<uint8_t> w((size_t)n);
        for (int c = 0; c < l.rsI; c++) {
            for (int j = 0; j < n; j++) w[(size_t)j] = f[4 + (size_t)c + (size_t)j * (size_t)l.rsI];
            const int fixed = rs.decode(w.data(), l.dual);
            if (fixed < 0) { ok = false; continue; }
            corrected += fixed;
            for (int j = 0; j < n; j++) f[4 + (size_t)c + (size_t)j * (size_t)l.rsI] = w[(size_t)j];
        }
    }
    if (l.derand && l.derandAfterRs) applyRandomiser(f);
    return ok;
}

void encodeFrame(const Sd3Link& l, std::vector<uint8_t>& f) {
    const size_t bytes = (size_t)(l.frameBits + 7) / 8;
    f.resize(bytes, 0);
    if (l.rsI > 0) {
        if (l.derandAfterRs && l.derand) applyRandomiser(f);       // the check symbols then cover the randomised data
        const int e = l.rsType == 239 ? 8 : 16;
        const int n = rsWordLength(l, bytes);
        ccsds::Rs rs(e, n);
        std::vector<uint8_t> w((size_t)n);
        for (int c = 0; c < l.rsI; c++) {
            for (int j = 0; j < n - 2 * e; j++) w[(size_t)j] = f[4 + (size_t)c + (size_t)j * (size_t)l.rsI];
            rs.encode(w.data(), l.dual);
            for (int j = 0; j < n; j++) f[4 + (size_t)c + (size_t)j * (size_t)l.rsI] = w[(size_t)j];
        }
        if (l.derand && !l.derandAfterRs) applyRandomiser(f);
    } else if (l.derand) applyRandomiser(f);
    for (int i = 0; i < 4; i++) f[(size_t)i] = (uint8_t)(l.sync >> (24 - 8 * i));
}

void frameIds(const uint8_t* f, size_t n, int& scid, int& vcid) {
    scid = vcid = -1;
    if (n < 10) return;
    const int ver = f[4] >> 6;
    if (ver == 1) { scid = (f[4] & 0x3F) << 2 | f[5] >> 6; vcid = f[5] & 0x3F; }
    else if (ver == 0) { scid = (f[4] & 0x3F) << 4 | f[5] >> 4; vcid = (f[5] >> 1) & 7; }
}

uint32_t crc32Hdlc(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

uint16_t crc16Ccitt(const uint8_t* p, size_t n, uint16_t c) {
    for (size_t i = 0; i < n; i++) {
        c = (uint16_t)(c ^ (uint16_t)(p[i] << 8));
        for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

static uint16_t crc16b(const uint8_t* p, size_t n, uint16_t poly, uint16_t c) {
    for (size_t i = 0; i < n; i++) {
        c = (uint16_t)(c ^ (uint16_t)(p[i] << 8));
        for (int k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ poly : c << 1);
    }
    return c;
}

void AosDemux::parse(size_t limit) {
    size_t at = 0;
    for (;;) {
        if (at >= limit || buf_.size() - at < 6) break;
        const uint8_t* h = &buf_[at];
        if ((h[0] >> 5) != 0) { have_ = false; buf_.clear(); return; }     // not a version-0 space packet: resync at the next pointer
        const bool sec = (h[0] >> 3) & 1;
        const size_t total = 6 + (size_t)(((int)h[4] << 8 | h[5]) + 1) + ((secExtends_ && sec) ? 8 : 0);
        if (buf_.size() - at < total) break;
        if (limit != SIZE_MAX && at + total > limit) break;       // it would run over the next packet start the frame announces: junk
        Packet p;
        p.apid = ((h[0] & 7) << 8) | h[1];
        p.seqFlag = h[2] >> 6;
        p.seq = ((h[2] & 0x3F) << 8) | h[3];
        p.secHdr = sec;
        p.raw.assign(h, h + total);
        if (p.apid != 2047 && onPacket) onPacket(p);
        at += total;
    }
    if (limit != SIZE_MAX && at < limit && limit <= buf_.size()) { dropped++; at = limit; }   // padding or a broken packet: restart where the frame says
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)at);
}

void AosDemux::push(const uint8_t* c) {
    const uint32_t count = (uint32_t)c[6] << 16 | (uint32_t)c[7] << 8 | c[8];
    if (lastCount_ >= 0 && count != (((uint32_t)lastCount_ + 1) & 0xFFFFFF)) { dropped++; have_ = false; buf_.clear(); }
    lastCount_ = count;
    const int fhp = (c[10] & 7) << 8 | c[11];
    const uint8_t* d = c + 12;
    if (have_) {
        const size_t boundary = fhp < 2047 && fhp < mpdu_ ? buf_.size() + (size_t)fhp : SIZE_MAX;
        buf_.insert(buf_.end(), d, d + mpdu_);
        parse(boundary);
        parse(SIZE_MAX);
        return;
    }
    if (fhp >= 2047 || fhp >= mpdu_) return;
    buf_.assign(d + fhp, d + mpdu_);
    have_ = true;
    parse(SIZE_MAX);
}

void buildAosFrame(int scid, int vcid, uint32_t counter, int mpdu, const std::vector<uint8_t>& stream, size_t& pos, int caduBytes, uint8_t* c, const std::vector<size_t>& starts) {
    std::memset(c, 0, (size_t)caduBytes);
    c[0] = 0x1A; c[1] = 0xCF; c[2] = 0xFC; c[3] = 0x1D;
    c[4] = (uint8_t)(0x40 | (scid >> 2));
    c[5] = (uint8_t)((scid & 3) << 6 | vcid);
    c[6] = (uint8_t)(counter >> 16); c[7] = (uint8_t)(counter >> 8); c[8] = (uint8_t)counter;
    int fhp = 2047;
    auto it = std::lower_bound(starts.begin(), starts.end(), pos);
    if (it != starts.end() && *it < pos + (size_t)mpdu) fhp = (int)(*it - pos);
    c[10] = (uint8_t)(fhp >> 8); c[11] = (uint8_t)fhp;
    const size_t n = std::min((size_t)mpdu, stream.size() > pos ? stream.size() - pos : 0);
    if (n) std::memcpy(c + 12, &stream[pos], n);
    pos += (size_t)mpdu;
}

void CaduStream::push(const uint8_t* p, size_t n, const std::function<void(const uint8_t*)>& out) {
    buf_.insert(buf_.end(), p, p + n);
    size_t at = 0;
    while (buf_.size() - at >= (size_t)n_ + 4) {
        if (buf_[at] == 0x1A && buf_[at + 1] == 0xCF && buf_[at + 2] == 0xFC && buf_[at + 3] == 0x1D && buf_[at + (size_t)n_] == 0x1A && buf_[at + (size_t)n_ + 1] == 0xCF) {
            out(&buf_[at]);
            at += (size_t)n_;
        } else if (buf_[at] == 0x1A && buf_[at + 1] == 0xCF && buf_[at + 2] == 0xFC && buf_[at + 3] == 0x1D && buf_.size() - at < (size_t)n_ + 4) break;
        else at++;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)at);
}

// ---- GRB ------------------------------------------------------------------------------------------------------------------

static const struct { int base, zone; } kAbiBases[] = {{0x90, 0}, {0xB0, 1}, {0xD0, 2}, {0xF0, 3}, {0x110, 0}, {0x130, 1}, {0x150, 2}, {0x170, 3}, {0x190, 0}, {0x1A0, 1}};
static const double kAbiRes[16] = {1, 0.5, 1, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

int abiChannelOfApid(int apid, int& zone) {
    for (const auto& b : kAbiBases)
        if (apid >= b.base && apid < b.base + 16) { zone = b.zone; return apid - b.base + 1; }
    return 0;
}

std::string abiProductName(int apid) {
    static const char* z[] = {"Full disk", "CONUS", "Mesoscale 1", "Mesoscale 2"};
    int zone = 0;
    const int ch = abiChannelOfApid(apid, zone);
    if (ch) return "ABI " + std::string(z[zone]) + " channel " + std::to_string(ch);
    if (apid >= 0x486 && apid <= 0x48B) { static const char* s[] = {"Fe094", "Fe132", "Fe171", "Fe195", "Fe284", "Fe304"}; return std::string("SUVI ") + s[apid - 0x486]; }
    if (apid == 0x301) return "GLM events";
    if (apid == 0x302) return "GLM flashes";
    if (apid == 0x303) return "GLM groups";
    if (apid == 0x300) return "GLM metadata";
    if (apid == 0x580) return "GRB information";
    if (apid >= 0x80 && apid < 0x1A0) return "ABI metadata";
    if (apid >= 0x480 && apid <= 0x485) return "SUVI metadata";
    return "";
}

struct GrbCanvas {
    int f = 1, w = 0, h = 0;
    std::vector<uint8_t> px;
};

struct GrbDecoder::Impl {
    AosDemux vc5{kGrbMpdu}, vc6{kGrbMpdu};
    struct Wip { bool active = false; bool valid = true; std::vector<uint8_t> sec, data; };
    std::map<int, Wip> wip[2];
    std::map<int, GrbCanvas> canvas;
};

GrbDecoder::GrbDecoder() : p_(std::make_unique<Impl>()) {}
GrbDecoder::~GrbDecoder() = default;

void GrbDecoder::reset() {
    p_->vc5.reset(); p_->vc6.reset();
    p_->wip[0].clear(); p_->wip[1].clear();
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void GrbDecoder::pushCadu(const uint8_t* c) {
    const int vcid = c[5] & 0x3F;
    if (vcid != 5 && vcid != 6) return;
    const int pol = vcid == 6 ? 1 : 0;
    AosDemux& dem = pol ? p_->vc6 : p_->vc5;

    auto finish = [this](int apid, Impl::Wip& w) {
        if (w.sec.size() < 8) return;
        const int variant = ((w.sec[6] & 7) << 2) | (w.sec[7] >> 6);
        payloads++;
        std::vector<uint8_t>& d = w.data;
        const std::string name = abiProductName(apid);
        if (variant == 2 || variant == 3) {
            if (d.size() < 34 || d[0] != 1) return;              // 1: JPEG 2000 (the others are not used on GRB)
            const uint32_t row = (uint32_t)d[11] << 16 | (uint32_t)d[12] << 8 | d[13];
            const uint32_t lx = be32(&d[14]), ly = be32(&d[18]), bh = be32(&d[22]), bw = be32(&d[26]), dqf = be32(&d[30]);
            const size_t size = std::min<size_t>(dqf, d.size() - 34);
            j2k::Image im = j2k::decode(&d[34], size);
            if (!im.ok) { j2kBad++; lastError = im.error; return; }
            blocks++;
            int zone = 0;
            const int ch = abiChannelOfApid(apid, zone);
            GrbCanvas& cv = p_->canvas[apid];
            if (cv.px.empty()) {
                int fw, fh;
                if (ch) {
                    const double res = kAbiRes[ch - 1];
                    const int base = zone == 0 ? 10848 : zone == 1 ? 5000 : 1000;
                    const int baseH = zone == 0 ? 10848 : zone == 1 ? 3000 : 1000;
                    fw = (int)(base / res); fh = (int)(baseH / res);
                    cv.f = std::max(1, (int)(4.0 / res + 0.5));
                } else { fw = (int)bw; fh = (int)std::max<uint32_t>(bh, (uint32_t)im.height); cv.f = 1; }
                cv.w = fw / cv.f; cv.h = fh / cv.f;
                if (cv.w <= 0 || cv.h <= 0 || cv.w > 30000 || cv.h > 30000) { cv = GrbCanvas{}; return; }
                cv.px.assign((size_t)cv.w * (size_t)cv.h, 0);
            }
            const int shift = std::max(0, im.precision - 8);
            const int y0 = (int)(ly + row);
            for (int y = 0; y < im.height; y++) {
                const int fy = y0 + y;
                if (fy < 0 || fy % cv.f) continue;
                const int ry = fy / cv.f;
                if (ry >= cv.h) break;
                uint8_t* out = &cv.px[(size_t)ry * (size_t)cv.w];
                for (int x = 0; x < im.width; x++) {
                    const int fx = (int)lx + x;
                    if (fx < 0 || fx % cv.f) continue;
                    const int rx = fx / cv.f;
                    if (rx >= cv.w) break;
                    out[rx] = (uint8_t)(std::min<int>(im.pix[(size_t)y * (size_t)im.width + (size_t)x] >> shift, 255));
                }
                if (onLine) onLine(apid, cv.w, cv.h, ry, out);
            }
            if (onProduct && !name.empty()) onProduct(apid, name);
        } else if (variant == 0) {
            if (d.size() < 21) return;
            const std::vector<uint8_t> body(d.begin() + 21, d.end());
            char nm[64];
            snprintf(nm, sizeof nm, "%03X_%u.%s", apid, be32(&d[1]), apid >= 0x301 && apid <= 0x303 ? "bin" : "xml");
            if (onFile) onFile(nm, body);
            if (onProduct && !name.empty()) onProduct(apid, name);
        }
    };

    dem.onPacket = [this, pol, &finish](const Packet& p) {
        packets++;
        if (onPacket) onPacket(p);
        if (p.raw.size() < 6 + 8 + 4) return;
        const bool crcOk = crc32Hdlc(p.raw.data(), p.raw.size() - 4) == be32(&p.raw[p.raw.size() - 4]);
        auto& map = p_->wip[pol];
        Impl::Wip& w = map[p.apid];
        const uint8_t* payload = p.raw.data() + 6;
        const size_t plen = p.raw.size() - 6;
        if (p.seqFlag == 1 || p.seqFlag == 3) {
            if (w.active && w.valid) finish(p.apid, w);
            w = Impl::Wip{};
            if (!crcOk) { crcBad++; return; }
            w.sec.assign(payload, payload + 8);
            w.data.assign(payload + 8, payload + plen - 4);
            w.active = true;
            if (p.seqFlag == 3) { finish(p.apid, w); w = Impl::Wip{}; }
        } else {
            if (!crcOk) { crcBad++; w.active = false; w.valid = false; return; }
            if (w.active) w.data.insert(w.data.end(), payload + 8, payload + plen - 4);
            if (p.seqFlag == 2 && w.active) { if (w.valid) finish(p.apid, w); w.active = false; }
        }
    };
    dem.push(c);
}

static void pushBe32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

static std::vector<std::vector<uint8_t>> grbSplit(int apid, uint16_t& seq, int variant, const std::vector<uint8_t>& payload) {
    std::vector<std::vector<uint8_t>> out;
    size_t at = 0;
    const size_t chunk = 7000;
    bool first = true;
    do {
        const size_t n = std::min(chunk, payload.size() - at);
        const bool last = at + n == payload.size();
        std::vector<uint8_t> raw(6 + 8);
        const int flag = first ? (last ? 3 : 1) : (last ? 2 : 0);
        raw[0] = (uint8_t)(0x08 | apid >> 8); raw[1] = (uint8_t)apid;
        raw[2] = (uint8_t)(flag << 6 | (seq >> 8 & 0x3F)); raw[3] = (uint8_t)seq;
        seq = (seq + 1) & 0x3FFF;
        raw[6 + 6] = (uint8_t)((variant >> 2) & 7); raw[6 + 7] = (uint8_t)((variant & 3) << 6);
        raw.insert(raw.end(), payload.begin() + (std::ptrdiff_t)at, payload.begin() + (std::ptrdiff_t)(at + n));
        const size_t total = raw.size() + 4;
        const size_t len = total - 7;
        raw[4] = (uint8_t)(len >> 8); raw[5] = (uint8_t)len;
        pushBe32(raw, crc32Hdlc(raw.data(), raw.size()));
        out.push_back(std::move(raw));
        at += n;
        first = false;
    } while (at < payload.size());
    return out;
}

std::vector<std::vector<uint8_t>> grbImagePackets(int apid, uint16_t& seq, uint32_t seconds, int compression, const std::vector<uint8_t>& data, uint32_t lx, uint32_t ly, uint32_t row, uint32_t h, uint32_t w) {
    std::vector<uint8_t> p;
    p.push_back((uint8_t)compression);
    pushBe32(p, seconds); pushBe32(p, 0);
    p.push_back(0); p.push_back(0);                     // block sequence
    p.push_back((uint8_t)(row >> 16)); p.push_back((uint8_t)(row >> 8)); p.push_back((uint8_t)row);
    pushBe32(p, lx); pushBe32(p, ly); pushBe32(p, h); pushBe32(p, w); pushBe32(p, (uint32_t)data.size());
    p.insert(p.end(), data.begin(), data.end());
    return grbSplit(apid, seq, 2, p);
}

std::vector<std::vector<uint8_t>> grbGenericPackets(int apid, uint16_t& seq, uint32_t seconds, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p(21, 0);
    p[1] = (uint8_t)(seconds >> 24); p[2] = (uint8_t)(seconds >> 16); p[3] = (uint8_t)(seconds >> 8); p[4] = (uint8_t)seconds;
    p.insert(p.end(), data.begin(), data.end());
    return grbSplit(apid, seq, 0, p);
}

// ---- GK-2A UHRIT ---------------------------------------------------------------------------------------------------------

struct UhritDecoder::Impl {
    std::map<int, AosDemux> dem;
    std::map<int, std::map<int, std::vector<uint8_t>>> wip;
};

UhritDecoder::UhritDecoder() : p_(std::make_unique<Impl>()) {}
UhritDecoder::~UhritDecoder() = default;
void UhritDecoder::reset() { p_->dem.clear(); p_->wip.clear(); }

void UhritDecoder::pushCadu(const uint8_t* c) {
    const uint16_t have = (uint16_t)(c[2046] << 8 | c[2047]);
    if (crc16Ccitt(c + 4, 2042, 0xFFFF) != have) { frameCrcBad++; return; }
    const int vc = c[5] & 0x3F;
    if (vc == 63) return;
    auto it = p_->dem.find(vc);
    if (it == p_->dem.end()) it = p_->dem.emplace(std::piecewise_construct, std::forward_as_tuple(vc), std::forward_as_tuple(kGrbMpdu, true)).first;
    auto finish = [this, vc](int apid, std::vector<uint8_t>& f) {
        if (f.size() >= 10) {
            files++;
            std::vector<uint8_t> file(f.begin() + 10, f.end());
            if (onFile) onFile(file, vc, apid);
        }
        f.clear();
    };
    it->second.onPacket = [this, vc, &finish](const Packet& p) {
        packets++;
        if (onPacket) onPacket(p);
        if (p.raw.size() < 6 + 4) return;
        auto& f = p_->wip[vc][p.apid];
        const uint32_t sent = be32(&p.raw[p.raw.size() - 4]);
        if (crc32Hdlc(p.raw.data(), p.raw.size() - 4) != sent) { crcBad++; f.clear(); return; }
        const size_t plen = p.raw.size() - 6;
        if (p.seqFlag == 1 || p.seqFlag == 3) {
            if (!f.empty()) finish(p.apid, f);
            if (plen >= 12) f.insert(f.end(), p.raw.begin() + 6 + 8, p.raw.end() - 4);
            if (p.seqFlag == 3) finish(p.apid, f);
        } else if (p.seqFlag == 0 || p.seqFlag == 2) {
            if (!f.empty() && plen >= 12) f.insert(f.end(), p.raw.begin() + 6 + 8, p.raw.end() - 4);
            if (p.seqFlag == 2 && !f.empty()) finish(p.apid, f);
        }
    };
    it->second.push(c);
}

std::string xritAnnotation(const std::vector<uint8_t>& f) {
    if (f.size() < 16 || f[0] != 0) return "";
    uint32_t total = be32(&f[4]);
    if (total > f.size()) return "";
    size_t at = 0;
    while (at + 3 <= total) {
        const int type = f[at];
        const int len = f[at + 1] << 8 | f[at + 2];
        if (len < 3 || at + (size_t)len > total) return "";
        if (type == 4) return std::string((const char*)&f[at + 3], (size_t)len - 3);
        at += (size_t)len;
    }
    return "";
}

void buildUhritCadu(int scid, int vcid, uint32_t counter, const std::vector<uint8_t>& stream, size_t& pos, uint8_t c[kUhritCadu], const std::vector<size_t>& starts) {
    buildAosFrame(scid, vcid, counter, kGrbMpdu, stream, pos, kUhritCadu, c, starts);
    const uint16_t crc = crc16Ccitt(c + 4, 2042, 0xFFFF);
    c[2046] = (uint8_t)(crc >> 8); c[2047] = (uint8_t)crc;
}

// ---- GVAR -----------------------------------------------------------------------------------------------------------------

void gvarScramble(uint8_t* f) {
    static const std::vector<uint8_t> tab = [] {
        std::vector<uint8_t> t((size_t)kGvarBytes);
        uint16_t sh = 0b101001110110101;
        uint8_t cur = 0; int have = 0; int oo = 0;
        for (int i = 0; i < 262288; i++) {
            const uint16_t out = ((sh >> 14) ^ (sh >> 7)) & 1;
            sh = (uint16_t)(sh << 1 | out);
            if (i >= 10032) {
                cur = (uint8_t)(cur << 1 | out);
                if (++have == 8) { t[(size_t)oo++] = cur; have = 0; }
            }
        }
        return t;
    }();
    for (int i = 0; i < kGvarBytes - 8; i++) f[8 + i] = (uint8_t)((i % 2 ? (tab[(size_t)i] ^ f[8 + i]) ^ 0xFF : tab[(size_t)i] ^ f[8 + i]));
}

static uint16_t gvarCrc(const uint8_t* d, int n) { return crc16b(d, (size_t)n, 0x1021, 0xFFFF); }

GvarBlock gvarHeader(const uint8_t* f) {
    uint8_t hdr[3][30];
    for (int c = 0; c < 3; c++) { std::memcpy(hdr[c], f + 8 + 30 * c, 30); hdr[c][0] &= 0x0F; }
    uint8_t use[30];
    bool crc = false;
    for (int c = 0; c < 3 && !crc; c++) {
        uint8_t t[30];
        std::memcpy(t, hdr[c], 30);
        t[28] = (uint8_t)~t[28]; t[29] = (uint8_t)~t[29];
        if (gvarCrc(t, 30) == 0) { std::memcpy(use, hdr[c], 30); crc = true; }
    }
    if (!crc) for (int i = 0; i < 30; i++) use[i] = (uint8_t)((hdr[0][i] & hdr[1][i]) | (hdr[0][i] & hdr[2][i]) | (hdr[1][i] & hdr[2][i]));
    GvarBlock b;
    b.id = use[0];
    b.wordSize = use[1];
    b.wordCount = use[2] << 8 | use[3];
    b.count = use[12] << 8 | use[13];
    b.crcOk = crc;
    return b;
}

static void unpackWords(const uint8_t* d, int n, uint16_t* w) {
    int pos = 0;
    for (int i = 0; i + 4 <= n; i += 4) {
        w[i] = (uint16_t)(d[pos] << 2 | d[pos + 1] >> 6);
        w[i + 1] = (uint16_t)((d[pos + 1] % 64) << 4 | d[pos + 2] >> 4);
        w[i + 2] = (uint16_t)((d[pos + 2] % 16) << 6 | d[pos + 3] >> 2);
        w[i + 3] = (uint16_t)((d[pos + 3] % 4) << 8 | d[pos + 4]);
        pos += 5;
    }
}

static void packWords(const uint16_t* w, int n, uint8_t* d) {
    int pos = 0;
    for (int i = 0; i + 4 <= n; i += 4) {
        d[pos] = (uint8_t)(w[i] >> 2);
        d[pos + 1] = (uint8_t)(w[i] << 6 | w[i + 1] >> 4);
        d[pos + 2] = (uint8_t)(w[i + 1] << 4 | w[i + 2] >> 6);
        d[pos + 3] = (uint8_t)(w[i + 2] << 2 | w[i + 3] >> 8);
        d[pos + 4] = (uint8_t)w[i + 3];
        pos += 5;
    }
}

int gvarCounter(const uint8_t* f) {
    uint16_t w[16];
    unpackWords(f + 8 + 30 * 3, 16, w);
    const int c = ((w[5] << 10) | w[6]) & 0x7FF;
    return c > kGvarLines ? -1 : c;
}

int gvarIrWords(const uint8_t* f) {
    uint16_t w[16];
    unpackWords(f + 8 + 30 * 3, 16, w);
    int n = ((w[11] << 10) | w[12]) & 0x1FFF;
    if (n > 6565 || n < kGvarIrWidth) n = 6530;
    return n;
}

void gvarVisPixels(const uint8_t* f, uint16_t* px) {
    static thread_local std::vector<uint16_t> w;
    w.resize(kGvarVisWidth + 16);
    uint8_t sh[5];
    int pos = 116;
    for (int i = 0; i < kGvarVisWidth + 10; i += 4) {
        for (int k = 0; k < 5; k++) sh[k] = (uint8_t)(f[pos + k] << 6 | f[pos + k + 1] >> 2);
        w[(size_t)i] = (uint16_t)(sh[0] << 2 | sh[1] >> 6);
        w[(size_t)i + 1] = (uint16_t)((sh[1] % 64) << 4 | sh[2] >> 4);
        w[(size_t)i + 2] = (uint16_t)((sh[2] % 16) << 6 | sh[3] >> 2);
        w[(size_t)i + 3] = (uint16_t)((sh[3] % 4) << 8 | sh[4]);
        pos += 5;
    }
    for (int i = 0; i < kGvarVisWidth; i++) px[i] = w[(size_t)i + 1];
}

void gvarIrPixels(const uint8_t* f, int words, uint16_t* a0, uint16_t* a1, uint16_t* b0, uint16_t* b1) {
    static thread_local std::vector<uint16_t> w;
    w.resize(5252 * 4 + 8);
    unpackWords(f + 8 + 30 * 3, 5252 * 4, w.data());
    for (int i = 0; i < kGvarIrWidth; i++) {
        a0[i] = w[(size_t)(16 + words * 0 + i)];
        a1[i] = w[(size_t)(16 + words * 1 + i)];
        b0[i] = w[(size_t)(16 + words * 2 + i)];
        b1[i] = i >= 5206 - 60 ? b0[i] : w[(size_t)(16 + words * 3 + i)];
    }
}

void gvarBuildFrame(int blockId, int counter, const std::function<uint16_t(int, int, int)>& pixel, int scid, uint8_t* f) {
    std::memset(f, 0, (size_t)kGvarBytes);
    for (int i = 0; i < 8; i++) f[i] = (uint8_t)(kGvarSync >> (56 - 8 * i));
    uint8_t h[30] = {};
    h[0] = (uint8_t)blockId; h[1] = 10; h[2] = 0x28; h[3] = 0x42; h[8] = 1;
    const uint16_t crc = gvarCrc(h, 28);
    h[28] = (uint8_t)~(crc >> 8); h[29] = (uint8_t)~crc;
    for (int c = 0; c < 3; c++) std::memcpy(f + 8 + 30 * c, h, 30);
    const int kWords = 6530;
    uint16_t doc[16] = {};
    doc[0] = (uint16_t)scid;
    doc[5] = (uint16_t)(counter >> 10); doc[6] = (uint16_t)(counter & 0x3FF);
    doc[11] = (uint16_t)(kWords >> 10); doc[12] = (uint16_t)(kWords & 0x3FF);
    if (blockId == 1 || blockId == 2) {
        std::vector<uint16_t> w(5252 * 4 + 8, 0);
        std::copy(doc, doc + 16, w.begin());
        const int c0 = (blockId - 1) * 2;                  // the two channels of this block
        for (int i = 0; i < kGvarIrWidth; i++) {
            w[(size_t)(16 + kWords * 0 + i)] = pixel(c0, counter * 2, i);
            w[(size_t)(16 + kWords * 1 + i)] = pixel(c0, counter * 2 + 1, i);
            w[(size_t)(16 + kWords * 2 + i)] = pixel(c0 + 1, counter * 2, i);
            if (i < 5206 - 60) w[(size_t)(16 + kWords * 3 + i)] = pixel(c0 + 1, counter * 2 + 1, i);
        }
        packWords(w.data(), 5252 * 4, f + 8 + 30 * 3);
    } else if (blockId >= 3 && blockId <= 10) {
        packWords(doc, 16, f + 8 + 30 * 3);
        std::vector<uint16_t> w(kGvarVisWidth + 16, 0);
        const int y = counter * 8 + (blockId - 3);
        for (int i = 0; i < kGvarVisWidth; i++) w[(size_t)i + 1] = pixel(4, y, i);
        // 116 bytes and 6 bits in: the words are packed from there
        std::vector<uint8_t> p(((size_t)kGvarVisWidth + 16) * 10 / 8 + 8, 0);
        packWords(w.data(), kGvarVisWidth + 10, p.data());
        for (size_t i = 0; i + 1 < p.size(); i++) {
            if (116 + i + 1 >= (size_t)kGvarBytes) break;
            f[116 + i] = (uint8_t)((f[116 + i] & 0xFC) | p[i] >> 6);
            f[116 + i + 1] = (uint8_t)(p[i] << 2);
        }
    }
    gvarScramble(f);
}

// ---- cubesats -------------------------------------------------------------------------------------------------------------

static const std::vector<uint8_t>& pn9() {
    static const std::vector<uint8_t> tab = [] {
        std::vector<uint8_t> t(1024, 0);
        uint16_t b = 0x1FF;
        for (int i = 0; i < 8192; i++) {
            const uint8_t bit = b & 1;
            const uint8_t x = (b & 1) ^ ((b >> 5) & 1);
            b = x ? (b >> 1) | 0x0100 : b >> 1;
            t[(size_t)i >> 3] |= (uint8_t)(bit << (i & 7));
        }
        return t;
    }();
    return tab;
}

bool geoscanFrame(uint8_t* d) {
    const auto& t = pn9();
    for (int i = 0; i < kGeoscanData; i++) d[i] ^= t[(size_t)i];
    const uint16_t a = crc16b(d, kGeoscanData - 2, 0x8005, 0xFFFF);
    return a == (uint16_t)(d[kGeoscanData - 2] << 8 | d[kGeoscanData - 1]);
}

void geoscanBuild(const uint8_t* payload, uint8_t* out) {
    std::memcpy(out, payload, kGeoscanData - 2);
    const uint16_t a = crc16b(out, kGeoscanData - 2, 0x8005, 0xFFFF);
    out[kGeoscanData - 2] = (uint8_t)(a >> 8); out[kGeoscanData - 1] = (uint8_t)a;
    const auto& t = pn9();
    for (int i = 0; i < kGeoscanData; i++) out[i] ^= t[(size_t)i];
}

static const uint8_t kSi4462[64] = {0x87, 0xb8, 0x59, 0xb7, 0xa1, 0xcc, 0x24, 0x57, 0x5e, 0x4b, 0x9c, 0x0e, 0xe9, 0xea, 0x50, 0x2a,
                                    0xbe, 0xb4, 0x1b, 0xb6, 0xb0, 0x5d, 0xf1, 0xe6, 0x9a, 0xe3, 0x45, 0xfd, 0x2c, 0x53, 0x18, 0x0c,
                                    0xca, 0xc9, 0xfb, 0x49, 0x37, 0xe5, 0xa8, 0x51, 0x3b, 0x2f, 0x61, 0xaa, 0x72, 0x18, 0x84, 0x02,
                                    0x23, 0x23, 0xab, 0x63, 0x89, 0x51, 0xb3, 0xe7, 0x8b, 0x72, 0x90, 0x4c, 0xe8, 0xfb, 0xc1, 0xff};

bool lucky7Frame(uint8_t* b) {
    for (int i = 0; i < 37; i++) b[2 + i] ^= kSi4462[i];
    const uint16_t a = crc16b(b + 2, 35, 0x8005, 0xFFFF);
    return a == (uint16_t)(b[2 + 35] << 8 | b[2 + 36]);
}

void lucky7Build(const uint8_t* payload, uint8_t* out) {
    out[0] = 0x2D; out[1] = 0xD4;
    std::memcpy(out + 2, payload, 35);
    const uint16_t a = crc16b(out + 2, 35, 0x8005, 0xFFFF);
    out[37] = (uint8_t)(a >> 8); out[38] = (uint8_t)a;
    for (int i = 0; i < 37; i++) out[2 + i] ^= kSi4462[i];
}

const char* lucky7Kind(const uint8_t* p) {
    if (p[1] == 0x00) return "telemetry";
    if ((p[1] & 0xC0) == 0xC0) return "picture";
    if ((p[1] & 0x20) == 0x20) return "dosimetry";
    return "other";
}

void SpinoDeframer::push(int bit) {
    sr_ = sr_ << 1 | (uint32_t)bit;
    if (in_) {
        by_ = (uint8_t)(by_ << 1 | bit);
        if (++bit_ == 8) { f_.push_back(by_); bit_ = 0; }
        if (f_.size() >= 18) {
            const int size = f_[17] << 8 | f_[16];
            if (size > 4096) { in_ = false; f_.clear(); bad++; return; }
            if ((int)f_.size() >= size + 16) {
                const uint16_t crc = (uint16_t)(f_[f_.size() - 1] << 8 | f_[f_.size() - 2]);
                if (crc == crc16Ccitt(f_.data(), f_.size() - 2, 0)) { if (onFrame) onFrame(f_); }
                else bad++;
                in_ = false;
                f_.clear();
            }
        }
        return;
    }
    if (sr_ == 0x2EFC9827u) { in_ = true; bit_ = 0; f_.clear(); }
}

std::vector<uint8_t> spinoBuild(const std::vector<uint8_t>& head16, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f = {0x2E, 0xFC, 0x98, 0x27};
    std::vector<uint8_t> h = head16;
    h.resize(16, 0);
    const size_t size = h.size() + 2 + body.size() + 2 - 16;
    std::vector<uint8_t> frame = h;
    frame.push_back((uint8_t)size); frame.push_back((uint8_t)(size >> 8));
    frame.insert(frame.end(), body.begin(), body.end());
    const uint16_t c = crc16Ccitt(frame.data(), frame.size(), 0);
    frame.push_back((uint8_t)c); frame.push_back((uint8_t)(c >> 8));
    f.insert(f.end(), frame.begin(), frame.end());
    return f;
}

} // namespace dect2::sd3
