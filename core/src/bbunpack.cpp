#include "dect2/bbunpack.h"
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dect2 {

static uint8_t crc8Byte(const uint8_t* p, int n) {
    unsigned crc = 0;
    for (int i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            unsigned fb = ((crc >> 7) & 1) ^ ((p[i] >> b) & 1);
            crc = (crc << 1) & 0xff;
            if (fb) crc ^= 0xD5;
        }
    }
    return (uint8_t)crc;
}

void BbUnpacker::lost() {
    st_.framesLost++;
    gap_ = true;
    havePartial_ = false;
    partial_.clear();
    haveCrc_ = false;
}

void BbUnpacker::emit(const uint8_t* payload) {
    uint8_t pkt[188];
    pkt[0] = 0x47;
    memcpy(pkt + 1, payload, 187);
    st_.packets++;
    if (sink_) sink_(pkt);
}

void BbUnpacker::push(const std::vector<uint8_t>& bits, const BbHeader& h) {
    if (bits.size() < 80 || !h.crcOk) { lost(); return; }
    st_.frames++;
    st_.hem = h.hem; st_.issyi = h.issyi; st_.npd = h.npd; st_.tsGs = h.tsGs;
    if (h.tsGs != 3 || h.sisMis != 1) { st_.unsupported++; lost(); st_.framesLost--; return; } // transport streams only
    const int dfl = h.dfl;
    if (dfl <= 0 || 80 + dfl > (int)bits.size() || (dfl & 7)) { st_.unsupported++; return; }
    const int len = dfl / 8;
    std::vector<uint8_t> data(len);
    for (int i = 0; i < len; i++) {
        unsigned v = 0;
        for (int b = 0; b < 8; b++) v = (v << 1) | bits[80 + i * 8 + b];
        data[i] = (uint8_t)v;
    }
    // user-packet layout inside the data field
    // HEM: 187 bytes (sync removed). Normal mode: 188 bytes, the first one is the CRC-8 of the previous packet.
    int stride = h.hem ? 187 : 188;
    if (h.npd) stride += 1;                      // deleted-null-packet counter
    if (h.issyi && !h.hem) stride += 2;          // ISSY after every user packet (normal mode)
    const int sd = h.syncd == 0xFFFF ? -1 : h.syncd / 8;
    auto finish = [&](const std::vector<uint8_t>& pk) {
        int o = h.hem ? 0 : 1;
        if (!h.hem && haveCrc_ && pk[0] != lastCrc_) st_.crcErrors++;
        emit(pk.data() + o);
        lastCrc_ = crc8Byte(pk.data() + o, 187);
        haveCrc_ = true;
        if (h.npd) for (int k = 0; k < pk[stride - 1]; k++) { uint8_t nul[187] = {0x1F, 0xFF, 0x10}; memset(nul + 3, 0xFF, 184); emit(nul); st_.nullInserted++; }
    };
    int pos = 0;
    const int head = sd < 0 ? len : std::min(sd, len);
    if (havePartial_ && !gap_) {
        partial_.insert(partial_.end(), data.begin(), data.begin() + head);
        if ((int)partial_.size() == stride) { finish(partial_); partial_.clear(); havePartial_ = false; }
        else if ((int)partial_.size() < stride && sd < 0) { gap_ = false; return; } // still incomplete: wait for the next frame
        else { st_.resyncs++; partial_.clear(); havePartial_ = false; }
    } else {
        if (havePartial_) st_.resyncs++;
        partial_.clear();
        havePartial_ = false;
    }
    gap_ = false;
    if (sd < 0) return;
    pos = sd;
    while (pos + stride <= len) {
        std::vector<uint8_t> pk(data.begin() + pos, data.begin() + pos + stride);
        finish(pk);
        pos += stride;
    }
    if (pos < len) { partial_.assign(data.begin() + pos, data.end()); havePartial_ = true; }
}

} // namespace dect2
