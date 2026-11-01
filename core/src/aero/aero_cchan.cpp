// Inmarsat Aero C channel framing and coding (aero_cchan.h). Follows JAERO's AeroL::DecodeC (GPL-3, Jonti Olds, see THIRD_PARTY.md).
#include "dect2/aero/aero_cchan.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
inline int popc64(uint64_t v) { return __builtin_popcountll(v); }
constexpr int kDepunctured = 2 * kAeroCCodedPairs;      // 5460
constexpr int kBlockBits = 256, kBlocks = kAeroCDataBits / kBlockBits, kCols = 4;

// the puncturing of PuncturedCode::depunture_soft_block(.., pattern 4): of every four coded bits the fourth is not sent
inline bool punctured(int pos) { return (pos & 3) == 3; }
}

std::vector<uint8_t> aeroCUniqueWordBits() {
    std::vector<uint8_t> b(kAeroCUwBits);
    for (int n = 0; n < kAeroCUwSyms; n++) {
        b[2 * n] = (uint8_t)((kAeroCUwQ >> (kAeroCUwSyms - 1 - n)) & 1);
        b[2 * n + 1] = (uint8_t)((kAeroCUwI >> (kAeroCUwSyms - 1 - n)) & 1);
    }
    return b;
}

// ============================ coding ============================
std::vector<uint8_t> AeroCFrameEncoder::frame(const uint8_t* voice, const uint8_t* su, uint8_t lead) {
    std::vector<uint8_t> info(kAeroCCodedPairs, 0);
    std::vector<uint8_t> vbits((size_t)kAeroCVoiceFrames * 96), sbits((size_t)kAeroCSus * 96);
    aeroBytesToBits(voice, (size_t)kAeroCVoiceFrames * kAeroCVoiceBytes, vbits.data());
    aeroBytesToBits(su, (size_t)kAeroCSus * 12, sbits.data());
    for (int y = 0; y < kAeroCVoiceFrames; y++) {
        const int o = y * kAeroCSubBits;
        info[o] = lead & 1;
        for (int k = 0; k < 96; k++) info[o + 1 + k] = vbits[(size_t)y * 96 + k];
        if (y < 24) for (int k = 0; k < 12; k++) info[o + 97 + k] = sbits[(size_t)y * 12 + k];
    }
    info[kAeroCInfoBits - 1] = lead & 1;
    AeroScrambler scr;
    for (int i = 0; i < kAeroCInfoBits; i++) info[i] ^= (uint8_t)scr.next();
    std::vector<uint8_t> coded(kDepunctured), sent;
    for (int i = 0; i < kAeroCCodedPairs; i++) enc_.encode(info[i], coded[2 * i], coded[2 * i + 1]);
    std::vector<uint8_t> data(kAeroCDataBits, 0);
    int n = 0;
    for (int p = 0; p < kDepunctured; p++) if (!punctured(p)) data[n++] = coded[p];       // 4095 bits, the 4096th stays 0
    std::vector<uint8_t> out = aeroCUniqueWordBits();
    out.reserve(kAeroCFrameBits);
    std::vector<uint8_t> tx(kBlockBits);
    for (int b = 0; b < kBlocks; b++) {
        aeroInterleave(data.data() + (size_t)b * kBlockBits, tx.data(), kCols);
        out.insert(out.end(), tx.begin(), tx.end());
    }
    return out;
}

AeroCFrameDecoder::AeroCFrameDecoder() : deint_(kAeroCDataBits), dep_(kDepunctured), bits_(kAeroCCodedPairs) {}

void AeroCFrameDecoder::decode(const float* coded, AeroCDecoded& out) {
    for (int b = 0; b < kBlocks; b++) aeroDeinterleave(coded + (size_t)b * kBlockBits, deint_.data() + (size_t)b * kBlockBits, kCols);
    {   // depuncture: an erasure (0) after every third value; the 4096th value is not used
        int w = 0;
        for (int i = 0; i < kAeroCDataBits - 1; i++) {
            dep_[w++] = deint_[i];
            if ((w & 3) == 3) dep_[w++] = 0.f;
        }
    }
    vit_.reset();
    vit_.decode(dep_.data(), kAeroCCodedPairs, bits_.data());
    int errs = 0, kept = 0;
    reenc_.reset();
    for (int i = 0; i < kAeroCCodedPairs; i++) {
        uint8_t c0, c1;
        reenc_.encode(bits_[i], c0, c1);
        const int p = 2 * i;
        if (!punctured(p) && dep_[p] != 0.f) { kept++; errs += (c0 != (dep_[p] > 0)); }
        if (!punctured(p + 1) && dep_[p + 1] != 0.f) { kept++; errs += (c1 != (dep_[p + 1] > 0)); }
    }
    out.channelBer = kept ? (float)errs / (float)kept : 0.5f;
    AeroScrambler scr;
    for (int i = 0; i < kAeroCInfoBits; i++) bits_[i] ^= (uint8_t)scr.next();
    out.voice.assign((size_t)kAeroCVoiceFrames * kAeroCVoiceBytes, 0);
    out.su.assign((size_t)kAeroCSus * 12, 0);
    std::vector<uint8_t> vb((size_t)kAeroCVoiceFrames * 96), sb((size_t)kAeroCSus * 96);
    for (int y = 0; y < kAeroCVoiceFrames; y++) {
        const int o = y * kAeroCSubBits;
        for (int k = 0; k < 96; k++) vb[(size_t)y * 96 + k] = bits_[o + 1 + k];
        if (y < 24) for (int k = 0; k < 12; k++) sb[(size_t)y * 12 + k] = bits_[o + 97 + k];
    }
    aeroBitsToBytes(vb.data(), out.voice.size(), out.voice.data());
    aeroBitsToBytes(sb.data(), out.su.size(), out.su.data());
    for (int j = 0; j < kAeroCSus; j++) out.suOk[j] = aeroSuCrcOk(&out.su[(size_t)j * 12]);
}

// ============================ framer ============================
struct AeroCFramer::Impl {
    std::function<void(AeroCFrameEvent&)> cb;
    static constexpr size_t kHist = 16384;
    std::vector<float> hist = std::vector<float>(kHist, 0.f);
    uint64_t idx = 0;
    enum State { Search, Collect, Expect } st = Search;
    uint64_t frameStart = 0, nextUwEnd = 0;
    bool fromSearch = false;
    int invPar[2] = {0, 0};
    int lastUwErr = 0;
    int misses = 0, badFrames = 0, dcd = 0;
    uint64_t nFrames = 0, nMiss = 0, nFalse = 0;
    std::vector<float> coded = std::vector<float>(kAeroCDataBits);
    AeroCFrameDecoder dec;
    AeroCFrameEvent ev;

    float at(uint64_t i) const { return hist[i & (kHist - 1)]; }

    // unique word ending at absolute bit `end`: bit errors summed over both arms after the best inversion and arm assignment
    int uwErrors(uint64_t end, int inv[2]) const {
        uint64_t wa = 0, wb = 0;
        for (int n = 0; n < kAeroCUwSyms; n++) {
            wa = (wa << 1) | (uint64_t)(at(end - 2 * (uint64_t)(kAeroCUwSyms - 1 - n)) > 0);
            wb = (wb << 1) | (uint64_t)(at(end - 1 - 2 * (uint64_t)(kAeroCUwSyms - 1 - n)) > 0);
        }
        const int aQ = popc64(wa ^ kAeroCUwQ), aI = popc64(wa ^ kAeroCUwI), bQ = popc64(wb ^ kAeroCUwQ), bI = popc64(wb ^ kAeroCUwI);
        auto m = [](int e) { return std::min(e, kAeroCUwSyms - e); };
        const int opt1 = m(aQ) + m(bI), opt2 = m(aI) + m(bQ);
        int ea, eb;
        if (opt1 <= opt2) { ea = aQ; eb = bI; } else { ea = aI; eb = bQ; }
        inv[end & 1] = ea > kAeroCUwSyms / 2;
        inv[(end - 1) & 1] = eb > kAeroCUwSyms / 2;
        return std::min(opt1, opt2);
    }

    bool trySync(uint64_t cur) {
        if (cur + 1 < (uint64_t)kAeroCUwBits) return false;
        int inv[2];
        const int e = uwErrors(cur, inv);
        if (e > 14) return false;
        invPar[0] = inv[0]; invPar[1] = inv[1];
        lastUwErr = e;
        frameStart = cur + 1;
        fromSearch = true;
        misses = 0;
        st = Collect;
        return true;
    }

    void push(float s) {
        hist[idx & (kHist - 1)] = s;
        const uint64_t cur = idx++;
        if (st == Search) { trySync(cur); return; }
        if (st == Expect) {
            if (cur < nextUwEnd + 1) return;
            int best = 999, bi[2] = {0, 0};
            uint64_t bestEnd = nextUwEnd;
            for (int d = -1; d <= 1; d++) {
                int inv[2];
                const uint64_t end = nextUwEnd + (uint64_t)(int64_t)d;
                const int e = uwErrors(end, inv);
                if (e < best || (e == best && d == 0)) { best = e; bestEnd = end; bi[0] = inv[0]; bi[1] = inv[1]; }
            }
            if (best <= 30) {
                invPar[0] = bi[0]; invPar[1] = bi[1];
                lastUwErr = best; frameStart = bestEnd + 1; misses = 0;
            } else {
                lastUwErr = -1; nMiss++; frameStart = nextUwEnd + 1;
                if (++misses > 3) { st = Search; return; }
            }
            fromSearch = false;
            st = Collect;
        }
        if (st == Collect) {
            if (lastUwErr < 0 && cur + 1 > frameStart && trySync(cur)) return;     // timing alone: keep looking for the word
            if (cur + 1 < frameStart + (uint64_t)kAeroCDataBits) return;
            frame();
        }
    }

    void frame() {
        for (int k = 0; k < kAeroCDataBits; k++) {
            const uint64_t p = frameStart + (uint64_t)k;
            float v = at(p);
            if (invPar[p & 1]) v = -v;
            coded[(size_t)k] = v;
        }
        AeroCDecoded df;
        dec.decode(coded.data(), df);
        int ok = 0;
        for (int j = 0; j < kAeroCSus; j++) ok += df.suOk[j];
        if (fromSearch && ok == 0 && df.channelBer > 0.2f) { nFalse++; st = Search; return; }
        const bool good = ok > 0 || df.channelBer < 0.12f;
        badFrames = good ? 0 : badFrames + 1;
        dcd = good ? std::min(dcd + 2, 12) : std::max(dcd - 3, 0);
        nFrames++;
        ev.voice = std::move(df.voice);
        ev.su = std::move(df.su);
        for (int j = 0; j < kAeroCSus; j++) ev.suOk[j] = df.suOk[j];
        ev.uwErrors = lastUwErr;
        ev.channelBer = df.channelBer;
        ev.bitIndex = frameStart;
        ev.haveCall = false; ev.aesId = 0; ev.gesId = 0;
        for (int j = 0; j < kAeroCSus; j++) {
            const uint8_t* su = &ev.su[(size_t)j * 12];
            if (ev.suOk[j] && su[0] == kAeroCCallProgress) {
                ev.haveCall = true;
                ev.aesId = ((uint32_t)su[1] << 16) | ((uint32_t)su[2] << 8) | su[3];
                ev.gesId = su[4];
            }
        }
        ev.voiceLooksUsed = false;
        for (size_t i = 1; i < ev.voice.size(); i++) if (ev.voice[i] != ev.voice[0]) { ev.voiceLooksUsed = true; break; }
        if (cb) cb(ev);
        nextUwEnd = frameStart + (uint64_t)kAeroCDataBits + (uint64_t)kAeroCUwBits - 1;
        st = badFrames >= 4 ? Search : Expect;
    }
};

AeroCFramer::AeroCFramer() : p_(std::make_unique<Impl>()) {}
AeroCFramer::~AeroCFramer() = default;
void AeroCFramer::reset() { auto cb = std::move(p_->cb); p_ = std::make_unique<Impl>(); p_->cb = std::move(cb); }
void AeroCFramer::push(float s) { p_->push(s); }
void AeroCFramer::setCallback(std::function<void(AeroCFrameEvent&)> cb) { p_->cb = std::move(cb); }
bool AeroCFramer::synced() const { return p_->st != Impl::Search; }
bool AeroCFramer::dataLock() const { return p_->st != Impl::Search && p_->dcd > 2; }
uint64_t AeroCFramer::frames() const { return p_->nFrames; }
uint64_t AeroCFramer::uwMisses() const { return p_->nMiss; }
uint64_t AeroCFramer::falseSyncs() const { return p_->nFalse; }

} // namespace dect2
