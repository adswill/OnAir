#include "dect2/isdbt_gen.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace isdbt {

namespace {
const int kBranchDelay[4][6] = {{0, 120, 0, 0, 0, 0}, {0, 120, 0, 0, 0, 0}, {0, 40, 80, 120, 0, 0}, {0, 24, 48, 72, 96, 120}};   // by Mod: DQPSK, QPSK, 16QAM, 64QAM

// energy dispersal: 1 + x^14 + x^15, started with 100101010000000 (the first bit is the oldest)
struct Prbs {
    unsigned r = 0;
    void reset() { r = 0; for (int i = 0; i < 15; i++) r = (r << 1) | (unsigned)("100101010000000"[i] - '0'); }
    unsigned bit() { const unsigned b = ((r >> 14) ^ (r >> 13)) & 1; r = ((r << 1) | b) & 0x7FFF; return b; }
};
}

PacketSource countingSource(unsigned seed) {
    auto n = std::make_shared<std::array<unsigned, 3>>();
    n->fill(0);
    return [n, seed](int layer, uint8_t* pkt) {
        const unsigned c = (*n)[layer]++;
        pkt[0] = 0x47;
        const unsigned pid = 0x100 + (unsigned)layer;
        pkt[1] = (uint8_t)(pid >> 8); pkt[2] = (uint8_t)pid; pkt[3] = (uint8_t)(0x10 | (c & 15));
        pkt[4] = (uint8_t)(c >> 24); pkt[5] = (uint8_t)(c >> 16); pkt[6] = (uint8_t)(c >> 8); pkt[7] = (uint8_t)c;
        unsigned x = c * 2654435761u + seed + (unsigned)layer * 97;
        for (int i = 8; i < 188; i++) { x = x * 1664525u + 1013904223u; pkt[i] = (uint8_t)(x >> 24); }
    };
}

bool checkCountingPacket(const uint8_t* pkt, int* layer, unsigned* counter) {
    if (pkt[0] != 0x47) return false;
    const unsigned pid = ((unsigned)(pkt[1] & 0x1F) << 8) | pkt[2];
    if (pid < 0x100 || pid > 0x102) return false;
    const unsigned c = ((unsigned)pkt[4] << 24) | ((unsigned)pkt[5] << 16) | ((unsigned)pkt[6] << 8) | pkt[7];
    if (layer) *layer = (int)(pid - 0x100);
    if (counter) *counter = c;
    return true;
}

PacketSource singleLayerSource(int layer, std::function<void(uint8_t*)> src) {
    return [layer, src](int l, uint8_t* pkt) {
        if (l == layer) { src(pkt); return; }
        std::memset(pkt, 0xFF, 188);
        pkt[0] = 0x47; pkt[1] = 0x1F; pkt[2] = 0xFF; pkt[3] = 0x10;
    };
}

// ---------------------------------------------------------------- one hierarchical layer

namespace {
// A delay line over a block: x[k] becomes the value that was D positions earlier (the first D come from the history h, oldest first);
// h then holds the last D values.
template <class T> void delayInPlace(T* x, size_t n, std::vector<T>& h) {
    const size_t D = h.size();
    if (!D) return;
    if (n >= D) {
        std::vector<T> nh(x + n - D, x + n);
        std::memmove((void*)(x + D), (const void*)x, (n - D) * sizeof(T));
        std::copy(h.begin(), h.end(), x);
        h.swap(nh);
    } else {
        std::vector<T> out(h.begin(), h.begin() + (long)n);
        std::copy(h.begin() + (long)n, h.end(), h.begin());
        std::copy(x, x + n, h.end() - (long)n);
        std::copy(out.begin(), out.end(), x);
    }
}
}

struct Generator::LayerTx {
    Layer cfg;
    int mode, nc, packets;
    dvbt::ConvInterleaver ci{false};
    dvbt::InnerEncoder enc;
    std::vector<std::vector<uint8_t>> ring;   // delay of every bit branch before the mapper, oldest first
    std::vector<cf32> dq;      // DQPSK reference of every cell position
    std::vector<uint8_t> byteDelay;   // delay adjustment before byte interleaving: K - 11 packets
    std::vector<uint8_t> bitDelay;    // delay adjustment before bit interleaving: two symbols minus the 120 cells of the interleaver
    std::vector<cf32> symDelay;       // delay adjustment before time interleaving, whole symbols
    std::vector<uint8_t> prbsSeq;     // energy dispersal sequence of one frame, zero where the synchronisation byte sits
    std::vector<cf32> lut;            // constellation by bit label
    std::vector<uint8_t> rs, shifted, il, bits, coded;   // scratch
    LayerTx(int mode_, const Layer& l) : cfg(l), mode(mode_), nc(dataPerSegment(mode_) * l.segments), packets(packetsPerFrame(mode_, l)), enc(l.rate), dq((size_t)nc, cf32(1, 0)) {
        const int m = bitsPerCell(l.mod);
        byteDelay.assign((size_t)std::max(0, packets - 11) * 204, 0);
        bitDelay.assign((size_t)(2 * nc - 120) * (size_t)m, 0);
        symDelay.assign((size_t)timeInterleaveAdjust(mode_, l.ti) * (size_t)nc, cf32(0, 0));
        ring.resize((size_t)m);
        for (int r = 0; r < m; r++) ring[(size_t)r].assign((size_t)kBranchDelay[l.mod][r], 0);
        if (l.mod != kDqpsk) { lut.resize((size_t)1 << m); for (unsigned i = 0; i < lut.size(); i++) lut[i] = mapLabel(l.mod, i); }
        // the frame starts at the byte after the first synchronisation byte
        const size_t n = (size_t)packets * 204;
        prbsSeq.assign(n, 0);
        Prbs prbs;
        prbs.reset();
        for (size_t j = 0; j < n; j++) {
            unsigned x = 0;
            for (int b = 0; b < 8; b++) x = (x << 1) | prbs.bit();
            if (j % 204 != 203) prbsSeq[j] = (uint8_t)x;      // the synchronisation byte is shifted through but not changed
        }
    }
    // pk: packets * 188 bytes (sync byte first); cells: 204 * nc
    void encodeFrame(const uint8_t* pk, std::vector<cf32>& cells) {
        const int K = packets;
        const size_t n = (size_t)K * 204;
        rs.resize(n + 1);
        for (int i = 0; i < K; i++) dvbt::rsEncode(pk + i * 188, rs.data() + (size_t)i * 204);
        rs[n] = 0x47;
        uint8_t* s = rs.data() + 1;
        for (size_t j = 0; j < n; j++) s[j] ^= prbsSeq[j];
        delayInPlace(s, n, byteDelay);
        il.resize(n);
        ci.process(s, il.data(), n);
        bits.resize(n * 8);
        for (size_t i = 0; i < n; i++) { const unsigned v = il[i]; uint8_t* d = &bits[i * 8]; for (int b = 0; b < 8; b++) d[b] = (uint8_t)((v >> (7 - b)) & 1); }
        enc.setPhase(0);
        enc.encode(bits, coded);
        const int m = bitsPerCell(cfg.mod);
        const size_t total = (size_t)kSymbolsPerFrame * nc;
        if (coded.size() != total * (size_t)m) { cells.assign(total, cf32(0, 0)); return; }
        cells.resize(total);
        delayInPlace(coded.data(), coded.size(), bitDelay);
        for (int r = 0; r < m; r++) {   // the delay of every bit branch: a delay line over every m-th bit
            auto& h = ring[(size_t)r];
            const size_t D = h.size();
            if (!D) continue;
            std::vector<uint8_t> col(total);
            for (size_t i = 0; i < total; i++) col[i] = coded[i * (size_t)m + (size_t)r];
            delayInPlace(col.data(), total, h);
            for (size_t i = 0; i < total; i++) coded[i * (size_t)m + (size_t)r] = col[i];
        }
        if (cfg.mod == kDqpsk) {
            // pi/4-shift DQPSK (Table 3-10): the phase step relative to the same cell of the preceding symbol
            static const float kPi4 = 0.78539816339f;
            static const float theta[4] = {kPi4, -kPi4, 3 * kPi4, -3 * kPi4};   // 00 pi/4, 01 -pi/4, 10 3pi/4, 11 -3pi/4
            cf32 rot[4];
            for (int i = 0; i < 4; i++) rot[i] = cf32(std::cos(theta[i]), std::sin(theta[i]));
            for (size_t k = 0; k < total; k++) {
                const int idx = coded[k * (size_t)m] * 2 + coded[k * (size_t)m + 1];
                cf32& st = dq[k % (size_t)nc];
                st *= rot[idx];
                const float mag = std::abs(st);
                if (mag > 0) st /= mag;
                cells[k] = st;
            }
        } else {
            const uint8_t* c = coded.data();
            for (size_t k = 0; k < total; k++, c += m) {
                unsigned label = 0;
                for (int r = 0; r < m; r++) label = (label << 1) | c[r];
                cells[k] = lut[label];
            }
        }
        delayInPlace(cells.data(), cells.size(), symDelay);
    }
};

// ---------------------------------------------------------------- generator

struct Generator::Plan {
    struct Ctrl { uint16_t idx; uint8_t isTmcc; };
    struct Seg {
        bool used = false;
        int layer = 0, local = 0;
        std::vector<uint32_t> src;                 // for data cell i of the segment after frequency interleaving: index into the time interleaved cells (segment * dps + j)
        std::vector<uint8_t> w;                    // pilot PRBS of the segment
        std::vector<cf32> tmpl[4];                 // carriers of the segment by symbol number modulo four, pilots in place, data and control carriers empty
        std::vector<uint16_t> dataIdx[4];
        std::vector<Ctrl> ctrl[4];
    };
    Seg seg[kSegments];
    std::vector<int> kmap;                         // carrier -> IFFT bin
    std::vector<cf32> timeInter, dataOut;          // working arrays, segment * dps + i
    int firstSeg[3] = {0, 0, 0};
};

Generator::Generator(const Params& p, PacketSource src, unsigned) : p_(p), N_(fftN(p.mode)), G_(guardSamples(p.mode, p.guard)), K_(totalCarriers(p.mode)), src_(std::move(src)) {
    if (!src_) src_ = countingSource();
    segmentLayout(p_, seg_);
    for (int i = 0; i < 3; i++) tx_.emplace_back(p_.layer[i].used() ? new LayerTx(p.mode, p_.layer[i]) : nullptr);
    const int dps = dataPerSegment(p.mode);
    fifo_.resize((size_t)kSegments * (size_t)dps);
    fifoPos_.assign(fifo_.size(), 0);
    int maxI = 0;
    for (int s = 0; s < kSegments; s++) {
        if (seg_[s].layer < 0) continue;
        const int I = interleavingLength(p.mode, p_.layer[seg_[s].layer].ti);
        maxI = std::max(maxI, I);
        for (int i = 0; i < dps; i++) fifo_[(size_t)s * dps + (size_t)i].assign((size_t)I * (size_t)((i * 5) % 96), cf32(0, 0));
    }
    carriers_.assign((size_t)kSymbolsPerFrame * K_, cf32(0, 0));
    chain_.assign((size_t)K_, 0);
    fft_ = new Fft(N_);
    {   // the layout: where every segment's cells come from, the carrier roles and pilots of the four symbol phases
        plan_.reset(new Plan);
        Plan& P = *plan_;
        const int cps = carriersPerSegment(p.mode);
        const uint16_t* rand = p.mode == 1 ? tables::kRandomizing1 : p.mode == 2 ? tables::kRandomizing2 : tables::kRandomizing3;
        for (int s = kSegments - 1; s >= 0; s--) if (seg_[s].layer >= 0) P.firstSeg[seg_[s].layer] = s;
        P.timeInter.assign((size_t)kSegments * dps, cf32(0, 0));
        P.dataOut.assign((size_t)kSegments * dps, cf32(0, 0));
        for (int g = 0; g < 3; g++) {
            std::vector<int> members;
            for (int s = 0; s < kSegments; s++) if (seg_[s].layer >= 0 && seg_[s].group == g) members.push_back(s);
            const int n = (int)members.size();
            for (int k = 0; k < n; k++) {
                Plan::Seg& S = P.seg[members[(size_t)k]];
                S.src.assign((size_t)dps, 0);
                for (int i = 0; i < dps; i++) {
                    const int j = (i + k) % dps;
                    uint32_t flat;
                    if (g == 0) flat = (uint32_t)(members[(size_t)k] * dps + j);
                    else { const int q = j * n + k; flat = (uint32_t)(members[(size_t)(q / dps)] * dps + q % dps); }
                    S.src[(size_t)rand[i]] = flat;      // dataOut[rand[i]] = value
                }
            }
        }
        std::vector<uint8_t> roles((size_t)cps);
        for (int s = 0; s < kSegments; s++) {
            if (seg_[s].layer < 0) continue;
            Plan::Seg& S = P.seg[s];
            S.used = true; S.layer = seg_[s].layer; S.local = s - P.firstSeg[S.layer];
            S.w = prbsW(p.mode, s);
            for (int ph = 0; ph < 4; ph++) {
                segmentRoles(p.mode, s, seg_[s].diff, ph, roles.data());
                S.tmpl[ph].assign((size_t)cps, cf32(0, 0));
                for (int i = 0; i < cps; i++) {
                    switch (roles[(size_t)i]) {
                    case kData: S.dataIdx[ph].push_back((uint16_t)i); break;
                    case kSP: case kCP: S.tmpl[ph][(size_t)i] = cf32(pilotValue(S.w[(size_t)i]), 0); break;
                    case kTMCC: case kAC1: case kAC2: S.ctrl[ph].push_back({(uint16_t)i, (uint8_t)(roles[(size_t)i] == kTMCC)}); break;
                    }
                }
            }
        }
        P.kmap.resize((size_t)K_);
        const int kc = centerCarrier(p.mode);
        for (int k = 0; k < K_; k++) P.kmap[(size_t)k] = ((k - kc) % N_ + N_) % N_;
    }
    // run the interleavers full before the first frame that is handed out
    int adj = 0;
    for (int li = 0; li < 3; li++) if (p_.layer[li].used()) adj = std::max(adj, timeInterleaveAdjust(p.mode, p_.layer[li].ti));
    const int warm = (95 * maxI + adj + 2 * 204) / kSymbolsPerFrame + 4;   // the time interleaver, then a frame for the byte delay and some for the coder
    for (int i = 0; i < warm; i++) produceFrame(nullptr);
    frames_ = 0;
}

Generator::~Generator() { delete fft_; }

void Generator::nextFrame(std::vector<cf32>& out) { produceFrame(&out); }

void Generator::produceFrame(std::vector<cf32>* out) {
    const int mode = p_.mode, dps = dataPerSegment(mode), cps = carriersPerSegment(mode);
    Plan& P = *plan_;
    // 1. channel coding of every layer for the whole frame
    std::vector<std::vector<cf32>> cells(3);
    for (int li = 0; li < 3; li++) {
        if (!tx_[(size_t)li]) continue;
        std::vector<uint8_t> pk((size_t)tx_[(size_t)li]->packets * 188);
        for (int i = 0; i < tx_[(size_t)li]->packets; i++) src_(li, pk.data() + (size_t)i * 188);
        tx_[(size_t)li]->encodeFrame(pk.data(), cells[(size_t)li]);
    }
    const bool evenFrame = (frames_ % 2) == 0;
    uint8_t tmccInfo[kTmccInfoBits];
    tmccPack(tmccFromParams(p_), tmccInfo);
    uint8_t tmccDiff[kSymbolsPerFrame], tmccSync[kSymbolsPerFrame];
    tmccFrameBits(tmccInfo, evenFrame, true, tmccDiff);
    tmccFrameBits(tmccInfo, evenFrame, false, tmccSync);
    std::vector<cf32> X((size_t)N_);
    if (out) out->resize((size_t)frameSamples());
    const float scale = 1.f / std::sqrt((float)K_ * 1.04f);
    for (int sym = 0; sym < kSymbolsPerFrame; sym++) {
        // 2. combine the layers into data segments and interleave in time
        for (int s = 0; s < kSegments; s++) {
            const Plan::Seg& S = P.seg[s];
            if (!S.used) continue;
            const cf32* in = &cells[(size_t)S.layer][(size_t)sym * (size_t)tx_[(size_t)S.layer]->nc + (size_t)S.local * (size_t)dps];
            cf32* o = &P.timeInter[(size_t)s * dps];
            for (int i = 0; i < dps; i++) {
                auto& f = fifo_[(size_t)s * dps + (size_t)i];
                if (f.empty()) { o[i] = in[i]; continue; }
                size_t& pos = fifoPos_[(size_t)s * dps + (size_t)i];
                o[i] = f[pos];
                f[pos] = in[i];
                if (++pos == f.size()) pos = 0;
            }
        }
        // 3. frequency interleaving, rotation and randomizing (the permutation is in the plan)
        for (int s = 0; s < kSegments; s++) {
            const Plan::Seg& S = P.seg[s];
            if (!S.used) continue;
            cf32* o = &P.dataOut[(size_t)s * dps];
            for (int i = 0; i < dps; i++) o[i] = P.timeInter[S.src[(size_t)i]];
        }
        // 4. the OFDM frame structure: data and pilots on the carriers
        cf32* car = &carriers_[(size_t)sym * K_];
        std::fill(car, car + K_, cf32(0, 0));
        for (int pos = 0; pos < kSegments; pos++) {
            const int s = kSegmentAtPosition[pos];
            const Plan::Seg& S = P.seg[s];
            if (!S.used) continue;
            const bool diff = seg_[s].diff;
            const int ph = diff ? 0 : sym % 4;
            cf32* c0 = car + (size_t)pos * cps;
            std::copy(S.tmpl[ph].begin(), S.tmpl[ph].end(), c0);
            const cf32* d = &P.dataOut[(size_t)s * dps];
            const std::vector<uint16_t>& di = S.dataIdx[ph];
            for (size_t q = 0; q < di.size(); q++) c0[di[q]] = d[q];
            const uint8_t* tb = diff ? tmccDiff : tmccSync;
            for (const Plan::Ctrl& cc : S.ctrl[ph]) {
                // differential BPSK: the reference of symbol 0 is the pilot sequence, then every 1 flips the phase
                uint8_t& c = chain_[(size_t)pos * cps + cc.idx];
                if (sym == 0) c = S.w[cc.idx];
                else c ^= cc.isTmcc ? tb[sym] : 1;
                c0[cc.idx] = cf32(c ? -kPilotAmp : kPilotAmp, 0);
            }
        }
        car[K_ - 1] = cf32(lastCarrierValue(mode), 0);
        // 5. IFFT with the guard interval
        std::fill(X.begin(), X.end(), cf32(0, 0));
        for (int k = 0; k < K_; k++) X[(size_t)P.kmap[(size_t)k]] = car[k];
        fft_->inverse(X.data());
        if (out) {
            cf32* o = out->data() + (size_t)sym * (size_t)(N_ + G_);
            for (int i = 0; i < N_; i++) o[G_ + i] = X[(size_t)i] * scale;
            for (int i = 0; i < G_; i++) o[i] = o[N_ + i];
        }
    }
    frames_++;
}

} // namespace isdbt
} // namespace dect2
