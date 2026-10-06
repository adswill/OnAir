#include "dect2/dvbt_fec.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace dvbt {

namespace {
const int kRateK[5] = {1, 2, 3, 5, 7};
const int kRateN[5] = {2, 3, 4, 6, 8};
}

void FecDecoder::configure(const Params& p) {
    if (!(p == p_) || p_.mode != p.mode) { p_ = p; reset(); }
    else p_ = p;
}

void FecDecoder::reset() {
    st_ = FecStats();
    llrQueue_.clear(); carry_.clear(); bits_.clear(); bytes_.clear(); after_.clear(); out_.clear();
    bitsDropped_ = 0; bitOffset_ = -1; syncBytePos_ = -1;
    deint_ = ConvInterleaver(true);
    groupIdx_ = 0; haveGroup_ = false; warm_ = 0; blocksSinceSync_ = 0; syncMisses_ = 0;
}

void FecDecoder::pushSymbol(const cf32* cells, const float* n0, int symIdx) {
    const int N = dataCarriers(p_.mode), m = bitsPerCell(p_.mod);
    static thread_local std::vector<cf32> c2;
    static thread_local std::vector<float> n2, llr, llr2;
    c2.resize(N); n2.resize(N); llr.resize((size_t)N * m); llr2.resize((size_t)N * m);
    symbolDeinterleave(p_.mode, symIdx, cells, n0, c2.data(), n2.data());
    demap(c2.data(), n2.data(), N, p_.mod, p_.hier, llr.data());
    bitDeinterleave(llr.data(), p_.mod, N, llr2.data());
    llrQueue_.insert(llrQueue_.end(), llr2.begin(), llr2.end());
    st_.symbols++;
    // decode in blocks of 16 symbols (the carry gives the trellis its history)
    if (llrQueue_.size() >= (size_t)N * m * 16) process(false);
}

void FecDecoder::process(bool) {
    const int rate = p_.crHp;
    const int k = kRateK[rate];
    if (llrQueue_.empty()) return;
    // normalise the soft values: the demapper's LLRs scale with 1/noise, the Viterbi works on small integers
    double mean = 0;
    for (float v : llrQueue_) mean += std::fabs(v);
    mean /= (double)llrQueue_.size();
    const float scale = (float)(std::min(40.0, 22.0 / std::max(1e-6, mean)));
    llrScale_ = scale;

    std::vector<int8_t> soft;
    if (!st_.phaseLocked) {
        // the puncturing pattern's phase relative to the start of the symbol stream is unknown: pick the best of k
        long bestMetric = -1; int bestPhase = 0;
        for (int ph = 0; ph < k; ph++) {
            std::vector<int8_t> s;
            Viterbi::depuncture(llrQueue_.data(), llrQueue_.size(), rate, ph, s, scale);
            std::vector<uint8_t> b;
            long metric = 0;
            Viterbi::decode(s, b, 4, &metric);
            if (ph == 0 || metric > bestMetric) { bestMetric = metric; bestPhase = ph; }
            if (k == 1) break;
        }
        st_.punctPhase = bestPhase;
        st_.phaseLocked = true;
    }
    Viterbi::depuncture(llrQueue_.data(), llrQueue_.size(), rate, st_.punctPhase, soft, scale);
    // each block starts at a symbol boundary and every symbol holds a whole number of puncture periods, so the phase repeats
    const size_t L = 192;
    std::vector<int8_t> window = carry_;
    window.insert(window.end(), soft.begin(), soft.end());
    const size_t steps = window.size() / 2;
    std::vector<uint8_t> dec;
    long metric = 0;
    Viterbi::decode(window, dec, 4, &metric);
    st_.viterbiMargin = steps ? (double)metric / (double)(steps * 2 * 22) : 0;
    const size_t fromStep = carry_.empty() ? 0 : L;                 // history steps were emitted before
    const size_t toStep = steps > L ? steps - L : 0;                // the tail is not yet reliable
    for (size_t t = fromStep; t < toStep; t++) bits_.push_back(dec[t]);
    // keep the last 2L steps as history
    const size_t keep = std::min(window.size() / 2, 2 * L);
    carry_.assign(window.end() - 2 * keep, window.end());
    llrQueue_.clear();

    // ---- bits -> bytes, find the byte phase and the sync position
    if (bitOffset_ < 0) {
        if (bits_.size() >= 8 * 204 * 12) {
            int bestOff = -1, bestPos = 0, bestScore = 0;
            for (int off = 0; off < 8; off++) {
                const size_t nbytes = (bits_.size() - off) / 8;
                std::vector<uint8_t> by(nbytes);
                for (size_t i = 0; i < nbytes; i++) { unsigned c = 0; for (int j = 0; j < 8; j++) c = (c << 1) | bits_[off + 8 * i + j]; by[i] = (uint8_t)c; }
                for (int pos = 0; pos < 204; pos++) {
                    int score = 0, total = 0;
                    for (size_t i = pos; i < nbytes; i += 204) { total++; score += (by[i] == 0x47 || by[i] == 0xB8); }
                    if (total >= 8 && score > bestScore) { bestScore = score; bestOff = off; bestPos = pos; }
                }
            }
            if (bestOff >= 0 && bestScore >= 8 && bestScore * 4 >= 3 * ((int)((bits_.size() - bestOff) / 8) / 204)) {
                bitOffset_ = bestOff;
                // drop everything before the first sync byte, so the stream starts on a packet boundary
                const size_t dropBits = (size_t)bestOff + 8 * (size_t)bestPos;
                bits_.erase(bits_.begin(), bits_.begin() + dropBits);
                st_.syncLocked = true;
                deint_ = ConvInterleaver(true);
                warm_ = 12;
                after_.clear();
            } else if (bits_.size() > 8 * 204 * 40) {
                bits_.erase(bits_.begin(), bits_.begin() + 8 * 204 * 20); // nothing found: look further on
            }
        }
        if (bitOffset_ < 0) return;
    }
    // whole 12-byte groups (the de-interleaver works on 12 branches)
    const size_t nb = bits_.size() / 8 / 12 * 12;
    if (!nb) return;
    std::vector<uint8_t> by(nb), di(nb);
    for (size_t i = 0; i < nb; i++) { unsigned c = 0; for (int j = 0; j < 8; j++) c = (c << 1) | bits_[8 * i + j]; by[i] = (uint8_t)c; }
    bits_.erase(bits_.begin(), bits_.begin() + nb * 8);
    deint_.process(by.data(), di.data(), nb);
    after_.insert(after_.end(), di.begin(), di.end());
    // RS blocks of 204 bytes
    size_t pos = 0;
    while (after_.size() - pos >= 204) {
        uint8_t* blk = &after_[pos];
        if (warm_ > 0) { warm_--; pos += 204; continue; }
        const bool syncByteOk = blk[0] == 0x47 || blk[0] == 0xB8;   // before decoding: is the alignment still right?
        const int r = rsDecode(blk);
        st_.packets++;
        uint8_t pkt[188];
        memcpy(pkt, blk, 188);
        if (r < 0) {
            st_.rsFailed++;
            pkt[1] |= 0x80; // transport_error_indicator
        } else if (r == 0) st_.rsClean++;
        else { st_.rsCorrected++; st_.bytesCorrected += r; }
        // the stream's sync bytes: 0xB8 marks the first packet of a scrambling group
        if (r >= 0 && pkt[0] == 0xB8) { groupIdx_ = 0; haveGroup_ = true; }
        else if (haveGroup_) groupIdx_ = (groupIdx_ + 1) % 8;
        if (haveGroup_) {
            descramblePacket(pkt, groupIdx_);
            if (r < 0) pkt[1] |= 0x80;
            out_.insert(out_.end(), pkt, pkt + 188);
        }
        // lost sync? 0x47/0xB8 expected at the start of every block
        if (r >= 0 && pkt[0] != 0x47) { /* descramble() restored it */ }
        // Lost sync only when the alignment is gone, not merely when blocks fail: through a fade the stream stays aligned and byte 0
        // of a block still reads 0x47 / 0xB8 even when the rest cannot be corrected. A misaligned stream shows one about once in 128
        // blocks, so a long run without one means the bits slipped. Dropping sync on every burst of failed blocks threw away the
        // alignment many times a second on a weak signal, and each new search cost a decoding block (16 symbols) of packets.
        if (r >= 0 || syncByteOk) syncMisses_ = 0;
        else if (++syncMisses_ > 100) { st_.syncLocked = false; bitOffset_ = -1; bits_.clear(); bytes_.clear(); after_.clear(); syncMisses_ = 0; st_.phaseLocked = false; carry_.clear(); return; }
        pos += 204;
    }
    after_.erase(after_.begin(), after_.begin() + pos);
}

void FecDecoder::descramblePacket(uint8_t* pkt, int g) {
    if (g == 0) prbs_ = 0xA9;
    auto clock8 = [&] {
        unsigned res = 0;
        for (int i = 0; i < 8; i++) {
            const unsigned fb = ((prbs_ >> 13) ^ (prbs_ >> 14)) & 1;
            prbs_ = ((prbs_ << 1) | fb) & 0x7FFF;
            res = (res << 1) | fb;
        }
        return res;
    };
    pkt[0] = 0x47;
    for (int k = 1; k < 188; k++) pkt[k] ^= (uint8_t)clock8();
    clock8();
}

void FecDecoder::takePackets(std::vector<uint8_t>& out) {
    out.insert(out.end(), out_.begin(), out_.end());
    out_.clear();
}

} // namespace dvbt
} // namespace dect2
