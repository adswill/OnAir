// DTMB transmitter model (see dtmb_tx.h).
#include "dect2/dtmb_tx.h"
#include "dect2/dtmb_map.h"
#include <cmath>
#include <cstring>

namespace dect2::dtmb {

FrameTx::FrameTx(const TxConfig& cfg, TsSource ts)
    : cfg_(cfg), ts_(std::move(ts)), ldpc_(ldpcCode(cfg.profile.rate)), fft_(kBody),
      symIl_(interleaverDelay(cfg.profile), false), bitIl_(interleaverDelay(cfg.profile), false), carriers_((size_t)kBody), siChips_(kSiSymbols) {
    frame_ = cfg.firstFrame;
    const int si = siIndex(cfg.profile);
    siChips(si > 0 ? si : 3, siChips_.data());
    coded_.reserve(kLdpcSent * 8);
    if (cfg.warmUp) {
        const long frames = (51L * kBranches * interleaverDelay(cfg.profile) + kDataSymbols - 1) / kDataSymbols + 1;
        std::vector<cf32> tmp((size_t)dtmb::frameLength(cfg.header));
        for (long i = 0; i < frames; i++) nextFrame(tmp.data());
    }
}

int FrameTx::superIndex() const { return (int)(frame_ % headerInfo(cfg_.header).framesPerSuper); }

// One LDPC codeword: its transport stream packets are scrambled (the generator restarts at every signal frame's worth of packets), cut into
// BCH blocks and encoded.
void FrameTx::encodeCodeword() {
    const int nPk = payloadBits(cfg_.profile.rate) / kTsBits;
    const int perFrame = packetsPerFrame(cfg_.profile);
    std::vector<uint8_t> payload((size_t)payloadBits(cfg_.profile.rate));
    uint8_t pkt[188];
    for (int k = 0; k < nPk; k++) {
        ts_(pkt);
        if (pkCount_ % perFrame == 0) scr_.reset();
        pkCount_++;
        for (int i = 0; i < 188; i++) for (int b = 0; b < 8; b++)
            payload[(size_t)(k * kTsBits + i * 8 + b)] = (uint8_t)(((pkt[i] >> (7 - b)) & 1) ^ scr_.next());
    }
    const int nb = bchBlocks(cfg_.profile.rate);
    std::vector<uint8_t> info((size_t)ldpcInfoBits(cfg_.profile.rate)), sent(kLdpcSent);
    for (int b = 0; b < nb; b++) bchEncode(&payload[(size_t)b * kBchK], &info[(size_t)b * kBchN]);
    ldpc_.encode(info.data(), sent.data());
    if (coded0_ > kLdpcSent * 4) { coded_.erase(coded_.begin(), coded_.begin() + (long)coded0_); coded0_ = 0; }
    coded_.insert(coded_.end(), sent.begin(), sent.end());
}

void FrameTx::nextFrame(cf32* out) {
    const Mapping map = cfg_.profile.map;
    const bool nr = map == Mapping::Qam4Nr;
    const int bps = bitsPerSymbol(map);
    const size_t need = nr ? (size_t)kDataSymbols : (size_t)kDataSymbols * (size_t)bps;
    while (coded_.size() - coded0_ < need) encodeCodeword();
    const uint8_t* bits = &coded_[coded0_];
    std::vector<cf32> data((size_t)kDataSymbols);
    if (nr) {
        // bit interleaver, then eight bits -> 16 bits (x0..x7, y0..y7) -> eight 4QAM symbols
        std::vector<uint8_t> il((size_t)kDataSymbols), nb(2 * (size_t)kDataSymbols);
        bitIl_.process(bits, il.data(), il.size());
        for (int g = 0; g < kDataSymbols / 8; g++) {
            int x = 0;
            for (int i = 0; i < 8; i++) x = (x << 1) | il[(size_t)(g * 8 + i)];
            const int y = nrParity((uint8_t)x);
            for (int i = 0; i < 8; i++) { nb[(size_t)(g * 16 + i)] = (uint8_t)((x >> (7 - i)) & 1); nb[(size_t)(g * 16 + 8 + i)] = (uint8_t)((y >> (7 - i)) & 1); }
        }
        mapSymbols(Mapping::Qam4, nb.data(), data.size(), data.data());
    } else {
        std::vector<cf32> mapped((size_t)kDataSymbols);
        mapSymbols(map, bits, mapped.size(), mapped.data());
        symIl_.process(mapped.data(), data.data(), mapped.size());
    }
    coded0_ += need;
    const float r = 1.f / std::sqrt(2.f);
    const HeaderInfo& hi = headerInfo(cfg_.header);
    cf32* body = out + hi.length;
    if (cfg_.carriers == 1) {
        // single carrier: the body is the time domain signal itself, system information first
        for (int s = 0; s < kSiSymbols; s++) body[s] = siChips_[(size_t)s] ? cf32(r, r) : cf32(-r, -r);
        for (int i = 0; i < kDataSymbols; i++) body[kSiSymbols + i] = data[(size_t)i];
        for (int k = 0; k < kBody; k++) carriers_[(size_t)k] = body[k];
    } else {
        // logical body: system information at its positions, data in between
        std::vector<cf32> logical((size_t)kBody);
        const auto& pos = siPositions();
        std::vector<char> isSi((size_t)kBody, 0);
        for (int s = 0; s < kSiSymbols; s++) {
            isSi[(size_t)pos[(size_t)s]] = 1;
            logical[(size_t)pos[(size_t)s]] = siChips_[(size_t)s] ? cf32(r, r) : cf32(-r, -r);
        }
        size_t di = 0;
        for (int l = 0; l < kBody; l++) if (!isSi[(size_t)l]) logical[(size_t)l] = data[di++];
        const auto& map2 = carrierMap();
        for (int l = 0; l < kBody; l++) carriers_[(size_t)map2[(size_t)l]] = logical[(size_t)l];
        // body: unitary inverse FFT
        for (int k = 0; k < kBody; k++) body[k] = carriers_[(size_t)k];
        fft_.inverse(body);
        const float g = 1.f / std::sqrt((float)kBody);
        for (int k = 0; k < kBody; k++) body[k] *= g;
    }
    // header
    const int phase = cfg_.phaseRotate ? pnPhase(cfg_.header, superIndex()) : 0;
    int8_t chips[945];
    pnHeader(cfg_.header, phase, chips);
    const float a = (float)std::sqrt(hi.powerRatio / 2.0);
    for (int n = 0; n < hi.length; n++) out[n] = cf32(a * chips[n], a * chips[n]);
    frame_++;
}

} // namespace dect2::dtmb
