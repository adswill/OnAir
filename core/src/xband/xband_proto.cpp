// See xband_proto.h. Link parameters from SatDump's pipelines (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/xband/xband_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2 {

namespace {
// name, family, MHz, sample rate, min rate, symbols, alpha, mod, fec, cadu, rsI, rsE, fill, nrzm, railDiff, derand, from, afterRs, invert, swap, mpdu, insert, tm, instruments, needs
const char* const kDish8 = "Needs a tracking X-band dish (1.2 m or more), a down-converter to L-band and a very fast radio";
const char* const kDish2 = "Needs a tracking S-band dish";
const char* const kDish1 = "Needs a tracking L-band dish";
const XbLink kLinks[kXbNumProfiles] = {
    {"xb_npp", "JPSS HRD (NPP, NOAA-20)", 530, 7812, 25e6, 18e6, 15e6, 0.5, kXbQpsk, kXbFecConv, 1024, 4, 16, 0, true, false, true, 4, false, false, false, 884, 0, false, "VIIRS, ATMS", kDish8},
    {"xb_jpss2", "JPSS HRD (NOAA-21)", 531, 7812, 40e6, 32e6, 25e6, 0.5, kXbOqpsk, kXbFecConv, 1279, 5, 16, 0, true, false, true, 4, false, false, false, 1094, 9, false, "VIIRS, ATMS", kDish8},
    {"xb_aqua", "EOS DB (Aqua, Aura)", 532, 8160, 15e6, 10e6, 7.5e6, 0.5, kXbOqpsk, kXbFecNone, 1024, 4, 16, 0, false, true, true, 4, false, false, false, 884, 0, false, "MODIS", kDish8},
    {"xb_terra", "EOS DB (Terra)", 533, 8212.5, 45e6, 30e6, 13.125e6, 0.5, kXbBpsk, kXbFecConv, 1024, 4, 16, 0, true, false, true, 10, true, false, true, 884, 0, false, "MODIS", kDish8},
    {"xb_metopd", "MetOp X-band dump", 534, 7800, 45e6, 40e6, 35e6, 0.5, kXbQpsk, kXbFecNone, 1024, 4, 16, 0, false, false, true, 4, false, true, false, 882, 2, false, "AVHRR", kDish8},
    {"xb_metopsg", "MetOp-SG DDB", 535, 7825, 140e6, 105e6, 93.3e6, 0.5, kXbQpsk, kXbFecConv, 1024, 4, 16, 0, false, false, true, 4, false, false, true, 884, 0, false, "METimage", kDish8},
    {"xb_landsatx", "Landsat 8/9 X-band", 536, 8200.5, 300e6, 240e6, 220.4e6, 0.5, kXbOqpsk, kXbFecLdpc, 1034, 0, 0, 0, false, false, false, 4, false, false, false, 1022, 0, false, "TIRS (from .cadu files; no LDPC decoding of IQ)", kDish8},
    {"xb_landsats", "Landsat 8/9 S-band", 537, 2200, 6e6, 2e6, 1.067e6, 0.5, kXbOqpsk, kXbFecConv, 1279, 5, 16, 0, true, false, true, 4, false, false, false, 0, 0, false, "frames only", kDish2},
    {"xb_probas", "Proba S-band dump", 538, 2235, 4e6, 3e6, 2e6, 0.5, kXbBpsk, kXbFecConv, 1279, 5, 16, 0, false, false, true, 4, false, false, true, 1103, 0, true, "Proba-V VEGETATION", kDish2},
    {"xb_probav", "Proba-V X-band dump", 539, 8300, 45e6, 38e6, 35e6, 0.5, kXbOqpsk, kXbFecConv, 1279, 5, 16, 0, false, false, true, 4, false, false, false, 1103, 0, true, "VEGETATION", kDish8},
    {"xb_gcomw", "GCOM-W1", 540, 8245, 20e6, 13e6, 10e6, 0.5, kXbOqpsk, kXbFecConv, 1264, 5, 8, 3, true, false, true, 4, false, false, false, 1092, 0, false, "AMSR2", kDish8},
    {"xb_gcomc", "GCOM-C1", 541, 8200, 90e6, 75e6, 69.416e6, 0.5, kXbOqpsk, kXbFecNone, 1264, 0, 0, 0, true, false, true, 4, false, false, false, 0, 0, false, "frames only", kDish8},
    {"xb_stereo", "STEREO high rate", 542, 8443.52, 3e6, 2e6, 1.44e6, 0.35, kXbBpsk, kXbFecTurbo, 1119, 0, 0, 0, false, false, false, 4, false, false, false, 0, 0, false, "frames only from .cadu files (no turbo decoding, no ICER images)", "Needs a large tracking X-band dish (STEREO is in deep space, about 18 m and up)"},
    {"xb_oceansat2", "Oceansat-2 DB", 543, 8200, 35e6, 28e6, 21.226e6, 0.5, kXbQpsk, kXbFecNone, 0, 0, 0, 0, false, false, false, 0, false, false, false, 0, 0, false, "OCM", kDish8},
    {"xb_oceansat3", "Oceansat-3 L-band", 544, 1703, 3e6, 1.5e6, 1e6, 0.5, kXbQpsk, kXbFecConv, 1024, 4, 16, 0, true, false, true, 4, false, false, false, 0, 0, false, "frames only", kDish1},
    {"xb_saral", "Saral L-band", 545, 1698.4, 3e6, 0.6e6, 0.4e6, 0.5, kXbQpsk, kXbFecConv, 1024, 4, 16, 0, true, false, true, 4, false, false, false, 0, 0, false, "frames only", kDish1},
    {"xb_oceansat3x", "Oceansat-3 X-band dump", 546, 8200, 120e6, 90e6, 80e6, 0.35, kXbQpsk, kXbFecNone, 2044, 8, 8, 0, false, false, true, 4, false, false, false, 0, 0, false, "frames only", kDish8},
    {"xb_saralx", "Saral X-band dump", 547, 8200, 25e6, 20e6, 16e6, 0.5, kXbQpsk, kXbFecConv, 1279, 5, 16, 0, false, false, true, 4, false, false, false, 0, 0, false, "frames only", kDish8},
    {"xb_kanopus", "Kanopus-V", 548, 8200, 90e6, 72e6, 61.44e6, 0.25, kXbQpsk, kXbFecNone, 0, 0, 0, 0, false, false, false, 0, false, false, false, 0, 0, false, "PSS, MSS", kDish8},
    {"xb_dmsp", "DMSP RTD", 549, 2252.5, 2e6, 1.5e6, 1.024e6, 0.5, kXbBpsk, kXbFecNone, 0, 0, 0, 0, false, false, false, 0, false, false, false, 0, 0, false, "OLS", kDish2},
};
}

const XbLink& xbLink(int p) { return kLinks[std::clamp(p, 0, (int)kXbNumProfiles - 1)]; }

namespace xband {

Cadu::Cadu(const XbLink& l) : L_(l) {
    const int parity = l.rsI * 2 * l.rsE;
    data_ = l.caduBytes - 4 - parity;
    if (l.rsI > 0) {
        n_ = 255 - l.rsFill;
        rs_ = std::make_unique<ccsds::Rs>(l.rsE, n_);
    }
    if (l.derand) pn_ = ccsds::randomiser((size_t)std::max(0, l.caduBytes - l.derandFrom));
}

int Cadu::decode(uint8_t* c) const {
    if (L_.invertBytes) for (int i = 4; i < L_.caduBytes; i++) c[i] ^= 0xFF;
    const auto rnd = [&]() { if (L_.derand) for (int i = L_.derandFrom; i < L_.caduBytes; i++) c[i] ^= pn_[(size_t)i - L_.derandFrom]; };
    if (L_.derand && !L_.derandAfterRs) rnd();
    int fixed = 0;
    bool bad = false;
    if (L_.rsI > 0) {
        std::vector<uint8_t> w((size_t)n_);
        for (int i = 0; i < L_.rsI; i++) {
            for (int j = 0; j < n_; j++) w[(size_t)j] = c[4 + i + j * L_.rsI];
            const int e = rs_->decode(w.data(), true);
            if (e < 0) { bad = true; continue; }
            fixed += e;
            if (e > 0) for (int j = 0; j < n_; j++) c[4 + i + j * L_.rsI] = w[(size_t)j];
        }
    }
    if (L_.derand && L_.derandAfterRs) rnd();
    return bad ? -1 : fixed;
}

void Cadu::encode(uint8_t* c) const {
    c[0] = kAsm >> 24 & 255; c[1] = kAsm >> 16 & 255; c[2] = kAsm >> 8 & 255; c[3] = kAsm & 255;
    const auto rnd = [&]() { if (L_.derand) for (int i = L_.derandFrom; i < L_.caduBytes; i++) c[i] ^= pn_[(size_t)i - L_.derandFrom]; };
    if (L_.derand && L_.derandAfterRs) rnd();
    if (L_.rsI > 0) {
        std::vector<uint8_t> w((size_t)n_);
        for (int i = 0; i < L_.rsI; i++) {
            for (int j = 0; j < n_ - 2 * L_.rsE; j++) w[(size_t)j] = c[4 + i + j * L_.rsI];
            rs_->encode(w.data(), true);
            for (int j = n_ - 2 * L_.rsE; j < n_; j++) c[4 + i + j * L_.rsI] = w[(size_t)j];
        }
    }
    if (L_.derand && !L_.derandAfterRs) rnd();
    if (L_.invertBytes) for (int i = 4; i < L_.caduBytes; i++) c[i] ^= 0xFF;
}

void parseHeader(const XbLink& l, const uint8_t* c, Header& h) {
    if (l.tmFrames) {
        h.scid = ((c[4] & 0x3F) << 4) | (c[5] >> 4);
        h.vcid = (c[5] >> 1) & 7;
        h.counter = c[7];
        h.fhp = ((c[8] & 7) << 8) | c[9];
        h.data = c + 10;
        return;
    }
    h.scid = ((c[4] & 0x3F) << 2) | (c[5] >> 6);
    h.vcid = c[5] & 0x3F;
    h.counter = (uint32_t)c[6] << 16 | (uint32_t)c[7] << 8 | c[8];
    const int m = 10 + l.insertZone;
    h.fhp = ((c[m] & 7) << 8) | c[m + 1];
    h.data = c + m + 2;
}

void Demux::take(const uint8_t* p, size_t n) {
    buf_.insert(buf_.end(), p, p + n);
    size_t at = 0;
    while (buf_.size() - at >= 6) {
        const uint8_t* h = &buf_[at];
        if ((h[0] >> 5) != 0) { have_ = false; buf_.clear(); dropped++; return; }
        const size_t len = (size_t)(h[4] << 8 | h[5]) + 1 + 6;
        if (buf_.size() - at < len) break;
        const int apid = (h[0] & 7) << 8 | h[1];
        if (apid != 0x7FF) {
            Packet pk;
            pk.apid = apid;
            pk.seqFlag = h[2] >> 6;
            pk.seqCount = (h[2] & 0x3F) << 8 | h[3];
            pk.data.assign(h + 6, h + len);
            packets++;
            if (onPacket) onPacket(pk);
        }
        at += len;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)at);
}

void Demux::push(const Header& v, int mpduData, bool counted) {
    if (counted && last_ >= 0) {
        const uint32_t want = (uint32_t)(last_ + 1) & (v.counter > 0xFF || (uint64_t)last_ > 0xFF ? 0xFFFFFFu : 0xFFu);
        if (v.counter != want && !((uint64_t)last_ == 0xFF && v.counter == 0)) { if (have_) dropped++; have_ = false; buf_.clear(); }
    }
    last_ = v.counter;
    if (v.fhp >= 0x7FE) { if (have_) take(v.data, (size_t)mpduData); return; }
    if (v.fhp >= mpduData) return;
    if (have_) take(v.data, (size_t)v.fhp);
    if (!buf_.empty()) dropped++;
    buf_.clear();
    have_ = true;
    take(v.data + v.fhp, (size_t)(mpduData - v.fhp));
}

void Mux::add(int apid, int seqFlag, int seqCount, const std::vector<uint8_t>& u) {
    starts_.push_back(q_.size());
    const size_t len = u.size() - 1;
    q_.push_back((uint8_t)(1 << 3 | (apid >> 8 & 7)));
    q_.push_back((uint8_t)apid);
    q_.push_back((uint8_t)(seqFlag << 6 | (seqCount >> 8 & 0x3F)));
    q_.push_back((uint8_t)seqCount);
    q_.push_back((uint8_t)(len >> 8)); q_.push_back((uint8_t)len);
    q_.insert(q_.end(), u.begin(), u.end());
}

bool Mux::next(uint8_t* c, const Cadu& cc) {
    const int mpdu = L_.mpduData;
    if (mpdu <= 0 || q_.size() < (size_t)mpdu) return false;
    int fhp = 0x7FF;
    for (size_t s : starts_) if (s < (size_t)mpdu) { fhp = (int)s; break; }
    std::memset(c, 0, (size_t)L_.caduBytes);
    const uint32_t count = counter_++;
    if (L_.tmFrames) {
        c[4] = (uint8_t)((scid_ >> 4) & 0x3F);
        c[5] = (uint8_t)(((scid_ & 15) << 4) | ((vcid_ & 7) << 1));
        c[7] = (uint8_t)count;
        c[8] = (uint8_t)(fhp >> 8 & 7);
        c[9] = (uint8_t)fhp;
        std::memcpy(c + 10, q_.data(), (size_t)mpdu);
    } else {
        c[4] = (uint8_t)(1 << 6 | (scid_ >> 2 & 0x3F));
        c[5] = (uint8_t)(((scid_ & 3) << 6) | (vcid_ & 0x3F));
        c[6] = (uint8_t)(count >> 16); c[7] = (uint8_t)(count >> 8); c[8] = (uint8_t)count;
        const int m = 10 + L_.insertZone;
        c[m] = (uint8_t)(fhp >> 8 & 7);
        c[m + 1] = (uint8_t)fhp;
        std::memcpy(c + m + 2, q_.data(), (size_t)mpdu);
    }
    q_.erase(q_.begin(), q_.begin() + mpdu);
    std::vector<size_t> ns;
    for (size_t s : starts_) if (s >= (size_t)mpdu) ns.push_back(s - mpdu);
    starts_ = std::move(ns);
    cc.encode(c);
    return true;
}

void unpack12(const uint8_t* in, int n, uint16_t* out) {
    for (int i = 0; i < n; i++) {
        const int b = i * 3 / 2;
        if (i & 1) out[i] = (uint16_t)(((in[b] & 15) << 8) | in[b + 1]);
        else out[i] = (uint16_t)((in[b] << 4) | (in[b + 1] >> 4));
    }
}
void pack12(const uint16_t* w, int n, uint8_t* out) {
    for (int i = 0; i < n; i++) {
        const int b = i * 3 / 2;
        if (i & 1) { out[b] = (uint8_t)((out[b] & 0xF0) | (w[i] >> 8 & 15)); out[b + 1] = (uint8_t)w[i]; }
        else { out[b] = (uint8_t)(w[i] >> 4); out[b + 1] = (uint8_t)((w[i] & 15) << 4); }
    }
}

void Plane::ensure(int y) {
    if (y < lines) return;
    lines = y + 1;
    px.resize((size_t)lines * (size_t)width, 0);
}
void Plane::setRow(int y, const uint16_t* row) { if (y < 0 || width <= 0) return; ensure(y); std::memcpy(&px[(size_t)y * width], row, (size_t)width * 2); }
void Plane::setPixel(int y, int x, uint16_t v) { if (y < 0 || x < 0 || x >= width) return; ensure(y); px[(size_t)y * width + x] = v; }

int Store::add(const std::string& name, double um, int width) {
    Plane p; p.name = name; p.um = um; p.width = width;
    p_.push_back(std::move(p));
    pub_.emplace_back();
    pubLines_.push_back(0);
    return (int)p_.size() - 1;
}
void Store::clear() { for (auto& p : p_) { p.px.clear(); p.lines = 0; } for (auto& v : pub_) v.reset(); for (auto& v : pubLines_) v = 0; }

std::vector<std::shared_ptr<const XbPreview>> Store::previews(int maxWidth) {
    for (size_t i = 0; i < p_.size(); i++) {
        const Plane& p = p_[i];
        if (p.lines == pubLines_[i] || p.lines == 0) continue;
        pubLines_[i] = p.lines;
        const int step = std::max(1, (int)std::ceil((double)p.width / maxWidth));
        auto v = std::make_shared<XbPreview>();
        v->id = ++id_; v->name = p.name; v->um = p.um;
        v->fullWidth = p.width; v->fullHeight = p.lines;
        v->width = p.width / step; v->height = p.lines / step;
        v->px.resize((size_t)v->width * v->height);
        // stretch on the pixels that hold data
        uint32_t hist[256] = {};
        uint64_t total = 0;
        for (size_t k = 0; k < p.px.size(); k += 7) { const int b = p.px[k] >> 8; if (p.px[k]) { hist[b]++; total++; } }
        int lo = 0, hi = 255;
        uint64_t acc = 0;
        for (int b = 0; b < 256; b++) { acc += hist[b]; if (acc * 100 > total) { lo = b; break; } }
        acc = 0;
        for (int b = 255; b >= 0; b--) { acc += hist[b]; if (acc * 100 > total) { hi = b; break; } }
        const int lv = lo << 8, ra = std::max(1, ((hi + 1) << 8) - lv);
        for (int y = 0; y < v->height; y++) {
            const uint16_t* r = &p.px[(size_t)(y * step) * p.width];
            for (int x = 0; x < v->width; x++) v->px[(size_t)y * v->width + x] = (uint8_t)std::clamp((r[x * step] - lv) * 255 / ra, 0, 255);
        }
        pub_[i] = v;
    }
    return pub_;
}

} // namespace xband
} // namespace dect2
