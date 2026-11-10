// SSDV packet check, picture assembly and the test-signal side, see ssdv_proto.h. Uses fsphil/ssdv (GPL-3.0-or-later) and stb_image / stb_image_write.
#include "dect2/ssdv/ssdv_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "ssdv.h"
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../../third_party/stb/stb_image_write.h"

namespace dect2 {
namespace ssdv {

Info header(const uint8_t* pkt) {
    ssdv_packet_info_t p;
    uint8_t copy[kPktSize];
    std::memcpy(copy, pkt, kPktSize);
    ssdv_dec_header(&p, copy);
    Info i;
    i.type = p.type; i.call = p.callsign_s; i.imageId = p.image_id; i.packetId = p.packet_id;
    i.width = p.width; i.height = p.height; i.eoi = p.eoi != 0; i.quality = p.quality; i.mcuMode = p.mcu_mode;
    i.mcuOffset = p.mcu_offset; i.mcuId = p.mcu_id; i.mcuCount = p.mcu_count;
    return i;
}

bool check(uint8_t* pkt, int* rsErrors, Info* info) {
    int e = 0;
    if (ssdv_dec_is_packet(pkt, kPktSize, &e) != 0) return false;
    if (rsErrors) *rsErrors = e;
    if (info) *info = header(pkt);
    return true;
}

bool Assembler::add(const uint8_t* pkt) {
    const Info i = header(pkt);
    if (pk_.count(i.packetId)) return false;
    std::array<uint8_t, kPktSize> a;
    std::memcpy(a.data(), pkt, kPktSize);
    pk_[i.packetId] = a;
    w_ = i.width; h_ = i.height; q_ = i.quality; mcuCount_ = i.mcuCount;
    if (i.eoi) eoiId_ = i.packetId;
    return true;
}

bool Assembler::complete() const { return eoiId_ >= 0 && missing() == 0; }

int Assembler::missing() const {
    if (pk_.empty()) return 0;
    const int last = eoiId_ >= 0 ? eoiId_ : pk_.rbegin()->first;
    return last + 1 - (int)std::count_if(pk_.begin(), pk_.end(), [&](const auto& kv) { return kv.first <= last; });
}

std::vector<std::pair<int, int>> Assembler::gapMcus() const {
    std::vector<std::pair<int, int>> v;
    if (pk_.empty() || mcuCount_ <= 0) return v;
    auto mcu = [](const std::array<uint8_t, kPktSize>& p) { return ((int)p[13] << 8) | p[14]; };   // the first MCU that starts in the packet; 0xFFFF: none
    int prevId = -1, lastMcu = 0;
    for (const auto& kv : pk_) {
        const int m = mcu(kv.second);
        if (kv.first != prevId + 1 && m != 0xFFFF && m > lastMcu) v.push_back({lastMcu, m - 1});
        if (m != 0xFFFF) lastMcu = m;
        prevId = kv.first;
    }
    if (eoiId_ < 0 || prevId != eoiId_) { if (lastMcu < mcuCount_ - 1) v.push_back({lastMcu, mcuCount_ - 1}); }
    return v;
}

int fillGaps(Rgb& im, const std::vector<std::pair<int, int>>& gaps, int mcu) {
    if (im.w <= 0 || im.h <= 0 || mcu <= 0) return 0;
    const int cols = im.w / mcu, rows = im.h / mcu;
    std::vector<char> hole((size_t)rows * cols, 0);
    int n = 0;
    auto at = [&](int x, int y) { return &im.rgb[((size_t)y * im.w + x) * 3]; };
    for (const auto& g : gaps) for (int m = g.first; m <= g.second && m < rows * cols; m++) {
        if (m < 0) continue;
        const int cx = m % cols, cy = m / cols;
        // only what the decoder really left empty: flat (a damaged neighbour keeps its detail)
        double sum = 0, sq = 0; int c = 0;
        for (int y = cy * mcu; y < (cy + 1) * mcu; y++) for (int x = cx * mcu; x < (cx + 1) * mcu; x++) { const uint8_t* p = at(x, y); const double v = 0.3 * p[0] + 0.6 * p[1] + 0.1 * p[2]; sum += v; sq += v * v; c++; }
        const double var = sq / c - (sum / c) * (sum / c);
        if (var < 9.0) { hole[(size_t)cy * cols + cx] = 1; n++; }
    }
    for (int cx = 0; cx < cols; cx++) for (int cy = 0; cy < rows; cy++) {
        if (!hole[(size_t)cy * cols + cx]) continue;
        int top = cy; while (top >= 0 && hole[(size_t)top * cols + cx]) top--;
        int bot = cy; while (bot < rows && hole[(size_t)bot * cols + cx]) bot++;
        for (int x = cx * mcu; x < (cx + 1) * mcu; x++) for (int y = cy * mcu; y < (cy + 1) * mcu; y++) {
            const int y0 = top >= 0 ? (top + 1) * mcu - 1 : -1, y1 = bot < rows ? bot * mcu : -1;
            uint8_t* p = at(x, y);
            for (int k = 0; k < 3; k++) {
                if (y0 >= 0 && y1 >= 0) { const double f = (double)(y - y0) / (y1 - y0); p[k] = (uint8_t)std::lround(at(x, y0)[k] * (1 - f) + at(x, y1)[k] * f); }
                else if (y0 >= 0) p[k] = at(x, y0)[k];
                else if (y1 >= 0) p[k] = at(x, y1)[k];
            }
        }
    }
    return n;
}

std::vector<uint8_t> Assembler::jpeg() const {
    if (pk_.empty()) return {};
    ssdv_t s;
    if (ssdv_dec_init(&s, kPktSize) != SSDV_OK) return {};
    std::vector<uint8_t> buf((size_t)w_ * (size_t)h_ * 2 + 65536);
    ssdv_dec_set_buffer(&s, buf.data(), buf.size());
    for (const auto& kv : pk_) {
        uint8_t p[kPktSize];
        std::memcpy(p, kv.second.data(), kPktSize);
        ssdv_dec_feed(&s, p);
    }
    uint8_t* out = nullptr;
    size_t len = 0;
    ssdv_dec_get_jpeg(&s, &out, &len);
    if (!out || len < 4 || len > buf.size()) return {};
    return std::vector<uint8_t>(out, out + len);
}

bool decodeJpeg(const uint8_t* data, size_t n, Rgb& o) {
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)n, &w, &h, &c, 3);
    if (!px) return false;
    o.w = w; o.h = h;
    o.rgb.assign(px, px + (size_t)w * h * 3);
    stbi_image_free(px);
    return true;
}

static void jpegSink(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

std::vector<uint8_t> testJpeg(int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = &px[((size_t)y * w + x) * 3];
            p[0] = (uint8_t)(255 * x / (w - 1)); p[1] = (uint8_t)(255 * y / (h - 1)); p[2] = (uint8_t)(128 + 100 * std::sin(0.3 * x) * std::cos(0.25 * y));
            const double dx = x - 0.65 * w, dy = y - 0.4 * h;
            if (dx * dx + dy * dy < (0.2 * h) * (0.2 * h)) { p[0] = 250; p[1] = 230; p[2] = 40; }
            if (x > w / 8 && x < w / 4 && y > h / 2 && y < h * 7 / 8) { p[0] = 20; p[1] = 40; p[2] = 200; }
        }
    std::vector<uint8_t> out;
    stbi_write_jpg_to_func(jpegSink, &out, w, h, 3, px.data(), 80);
    return out;
}

std::vector<std::array<uint8_t, kPktSize>> encodePackets(const std::vector<uint8_t>& jpeg, const std::string& call, int imageId, int quality, bool noFec) {
    std::vector<std::array<uint8_t, kPktSize>> out;
    ssdv_t s;
    char cs[16];
    std::snprintf(cs, sizeof cs, "%s", call.c_str());
    if (ssdv_enc_init(&s, noFec ? SSDV_TYPE_NOFEC : SSDV_TYPE_NORMAL, cs, (uint8_t)imageId, (int8_t)quality, kPktSize) != SSDV_OK) return out;
    uint8_t pkt[kPktSize];
    ssdv_enc_set_buffer(&s, pkt);
    size_t pos = 0;
    for (int guard = 0; guard < 100000; guard++) {
        int r;
        while ((r = ssdv_enc_get_packet(&s)) == SSDV_FEED_ME) {
            if (pos >= jpeg.size()) { r = SSDV_EOI; break; }
            const size_t n = std::min<size_t>(128, jpeg.size() - pos);
            ssdv_enc_feed(&s, const_cast<uint8_t*>(jpeg.data()) + pos, n);
            pos += n;
        }
        if (r != SSDV_OK) break;
        std::array<uint8_t, kPktSize> a;
        std::memcpy(a.data(), pkt, kPktSize);
        out.push_back(a);
    }
    return out;
}

} // namespace ssdv
} // namespace dect2
