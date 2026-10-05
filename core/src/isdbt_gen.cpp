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

struct Generator::LayerTx {
    Layer cfg;
    int mode, nc, packets;
    dvbt::ConvInterleaver ci{false};
    dvbt::InnerEncoder enc;
    std::vector<std::vector<uint8_t>> ring;
    std::vector<size_t> ringPos;
    std::vector<cf32> dq;      // DQPSK reference of every cell position
    std::vector<uint8_t> byteDelay;   // delay adjustment before byte interleaving: K - 11 packets
    size_t byteDelayPos = 0;
    std::vector<uint8_t> bitDelay;    // delay adjustment before bit interleaving: two symbols minus the 120 cells of the interleaver
    size_t bitDelayPos = 0;
    std::vector<cf32> symDelay;       // delay adjustment before time interleaving, whole symbols
    size_t symDelayPos = 0;
    LayerTx(int mode_, const Layer& l) : cfg(l), mode(mode_), nc(dataPerSegment(mode_) * l.segments), packets(packetsPerFrame(mode_, l)), enc(l.rate), dq((size_t)nc, cf32(1, 0)) {
        const int m = bitsPerCell(l.mod);
        byteDelay.assign((size_t)std::max(0, packets - 11) * 204, 0);
        bitDelay.assign((size_t)(2 * nc - 120) * (size_t)m, 0);
        symDelay.assign((size_t)timeInterleaveAdjust(mode_, l.ti) * (size_t)nc, cf32(0, 0));
        ring.resize((size_t)m);
        ringPos.assign((size_t)m, 0);
        for (int r = 0; r < m; r++) ring[(size_t)r].assign((size_t)kBranchDelay[l.mod][r], 0);
    }
    // pk: packets * 188 bytes (sync byte first); cells: 204 * nc
    void encodeFrame(const uint8_t* pk, std::vector<cf32>& cells) {
        const int K = packets;
        std::vector<uint8_t> rs((size_t)K * 204 + 1);
        for (int i = 0; i < K; i++) dvbt::rsEncode(pk + i * 188, rs.data() + (size_t)i * 204);
        rs[(size_t)K * 204] = 0x47;
        // the frame starts at the byte after the first synchronisation byte
        std::vector<uint8_t> s(rs.begin() + 1, rs.end());
        Prbs prbs;
        prbs.reset();
        for (size_t j = 0; j < s.size(); j++) {
            unsigned x = 0;
            for (int b = 0; b < 8; b++) x = (x << 1) | prbs.bit();
            if (j % 204 != 203) s[j] ^= (uint8_t)x;      // the synchronisation byte is shifted through but not changed
        }
        for (size_t j = 0; j < s.size() && !byteDelay.empty(); j++) { const uint8_t o = byteDelay[byteDelayPos]; byteDelay[byteDelayPos] = s[j]; s[j] = o; if (++byteDelayPos == byteDelay.size()) byteDelayPos = 0; }
        std::vector<uint8_t> il(s.size());
        ci.process(s.data(), il.data(), s.size());
        std::vector<uint8_t> bits(il.size() * 8), coded;
        for (size_t i = 0; i < il.size(); i++) for (int b = 0; b < 8; b++) bits[i * 8 + (size_t)b] = (il[i] >> (7 - b)) & 1;
        enc.setPhase(0);
        enc.encode(bits, coded);
        const int m = bitsPerCell(cfg.mod);
        const size_t total = (size_t)kSymbolsPerFrame * nc;
        if (coded.size() != total * (size_t)m) { cells.assign(total, cf32(0, 0)); return; }
        cells.resize(total);
        for (size_t j = 0; j < coded.size() && !bitDelay.empty(); j++) { const uint8_t o = bitDelay[bitDelayPos]; bitDelay[bitDelayPos] = coded[j]; coded[j] = o; if (++bitDelayPos == bitDelay.size()) bitDelayPos = 0; }
        for (size_t n = 0; n < total; n++) {
            uint8_t b[6];
            for (int r = 0; r < m; r++) {
                uint8_t in = coded[n * (size_t)m + (size_t)r];
                auto& rg = ring[(size_t)r];
                if (!rg.empty()) { const uint8_t out = rg[ringPos[(size_t)r]]; rg[ringPos[(size_t)r]] = in; if (++ringPos[(size_t)r] == rg.size()) ringPos[(size_t)r] = 0; in = out; }
                b[r] = in;
            }
            if (cfg.mod == kDqpsk) {
                // pi/4-shift DQPSK (Table 3-10): the phase step relative to the same cell of the preceding symbol
                static const float kPi4 = 0.78539816339f;
                const int idx = b[0] * 2 + b[1];     // 00 pi/4, 01 -pi/4, 10 3pi/4, 11 -3pi/4
                static const float theta[4] = {kPi4, -kPi4, 3 * kPi4, -3 * kPi4};
                const cf32 rot(std::cos(theta[idx]), std::sin(theta[idx]));
                cf32& st = dq[n % (size_t)nc];
                st *= rot;
                const float mag = std::abs(st);
                if (mag > 0) st /= mag;
                cells[n] = st;
            } else {
                unsigned label = 0;
                for (int r = 0; r < m; r++) label = (label << 1) | b[r];
                cells[n] = mapLabel(cfg.mod, label);
            }
        }
        for (size_t j = 0; j < cells.size() && !symDelay.empty(); j++) { const cf32 o = symDelay[symDelayPos]; symDelay[symDelayPos] = cells[j]; cells[j] = o; if (++symDelayPos == symDelay.size()) symDelayPos = 0; }
    }
};

// ---------------------------------------------------------------- generator

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
    // 1. channel coding of every layer for the whole frame
    std::vector<std::vector<cf32>> cells(3);
    for (int li = 0; li < 3; li++) {
        if (!tx_[(size_t)li]) continue;
        std::vector<uint8_t> pk((size_t)tx_[(size_t)li]->packets * 188);
        for (int i = 0; i < tx_[(size_t)li]->packets; i++) src_(li, pk.data() + (size_t)i * 188);
        tx_[(size_t)li]->encodeFrame(pk.data(), cells[(size_t)li]);
    }
    // first segment number of every layer
    int firstSeg[3] = {0, 0, 0};
    for (int s = kSegments - 1; s >= 0; s--) if (seg_[s].layer >= 0) firstSeg[seg_[s].layer] = s;
    const bool evenFrame = (frames_ % 2) == 0;
    uint8_t tmccInfo[kTmccInfoBits];
    tmccPack(tmccFromParams(p_), tmccInfo);
    uint8_t tmccDiff[kSymbolsPerFrame], tmccSync[kSymbolsPerFrame];
    tmccFrameBits(tmccInfo, evenFrame, true, tmccDiff);
    tmccFrameBits(tmccInfo, evenFrame, false, tmccSync);
    const uint16_t* rand = mode == 1 ? tables::kRandomizing1 : mode == 2 ? tables::kRandomizing2 : tables::kRandomizing3;
    std::vector<cf32> seg[kSegments];
    std::vector<uint8_t> roles((size_t)cps);
    std::vector<cf32> X((size_t)N_);
    if (out) out->resize((size_t)frameSamples());
    const float scale = 1.f / std::sqrt((float)K_ * 1.04f);
    for (int sym = 0; sym < kSymbolsPerFrame; sym++) {
        // 2. combine the layers into data segments and interleave in time
        for (int s = 0; s < kSegments; s++) {
            if (seg_[s].layer < 0) continue;
            const int li = seg_[s].layer;
            const int local = s - firstSeg[li];
            const size_t nc = (size_t)tx_[(size_t)li]->nc;
            seg[s].assign((size_t)dps, cf32(0, 0));
            for (int i = 0; i < dps; i++) {
                const cf32 in = cells[(size_t)li][(size_t)sym * nc + (size_t)local * (size_t)dps + (size_t)i];
                auto& f = fifo_[(size_t)s * dps + (size_t)i];
                if (f.empty()) { seg[s][(size_t)i] = in; continue; }
                size_t& pos = fifoPos_[(size_t)s * dps + (size_t)i];
                seg[s][(size_t)i] = f[pos];
                f[pos] = in;
                if (++pos == f.size()) pos = 0;
            }
        }
        // 3. frequency interleaving: inter-segment within the differential and the synchronous group, then rotation and randomizing
        std::vector<cf32> dataOut[kSegments];
        for (int g = 0; g < 3; g++) {
            std::vector<int> members;
            for (int s = 0; s < kSegments; s++) if (seg_[s].layer >= 0 && seg_[s].group == g) members.push_back(s);
            const int n = (int)members.size();
            if (!n) continue;
            std::vector<cf32> cat((size_t)n * dps);
            for (int k = 0; k < n; k++) std::copy(seg[members[(size_t)k]].begin(), seg[members[(size_t)k]].end(), cat.begin() + (size_t)k * dps);
            for (int k = 0; k < n; k++) {
                std::vector<cf32> inter((size_t)dps), rot((size_t)dps);
                if (g == 0) inter = seg[members[(size_t)k]];
                else for (int i = 0; i < dps; i++) inter[(size_t)i] = cat[(size_t)i * (size_t)n + (size_t)k];
                for (int i = 0; i < dps; i++) rot[(size_t)i] = inter[(size_t)((i + k) % dps)];
                std::vector<cf32>& o = dataOut[members[(size_t)k]];
                o.assign((size_t)dps, cf32(0, 0));
                for (int i = 0; i < dps; i++) o[rand[i]] = rot[(size_t)i];
            }
        }
        // 4. the OFDM frame structure: data and pilots on the carriers
        cf32* car = &carriers_[(size_t)sym * K_];
        std::fill(car, car + K_, cf32(0, 0));
        for (int pos = 0; pos < kSegments; pos++) {
            const int s = kSegmentAtPosition[pos];
            if (seg_[s].layer < 0) continue;
            const bool diff = seg_[s].diff;
            segmentRoles(mode, s, diff, sym, roles.data());
            const auto& w = prbsW(mode, s);
            const uint8_t* tb = diff ? tmccDiff : tmccSync;
            int di = 0;
            for (int i = 0; i < cps; i++) {
                const int k = pos * cps + i;
                switch (roles[(size_t)i]) {
                case kData: car[k] = dataOut[s][(size_t)di++]; break;
                case kSP: case kCP: car[k] = cf32(pilotValue(w[(size_t)i]), 0); break;
                case kTMCC: case kAC1: case kAC2: {
                    // differential BPSK: the reference of symbol 0 is the pilot sequence, then every 1 flips the phase
                    uint8_t& c = chain_[(size_t)k];
                    if (sym == 0) c = w[(size_t)i];
                    else c ^= (roles[(size_t)i] == kTMCC) ? tb[sym] : 1;
                    car[k] = cf32(c ? -kPilotAmp : kPilotAmp, 0);
                    break;
                }
                }
            }
        }
        car[K_ - 1] = cf32(lastCarrierValue(mode), 0);
        // 5. IFFT with the guard interval
        std::fill(X.begin(), X.end(), cf32(0, 0));
        const int kc = centerCarrier(mode);
        for (int k = 0; k < K_; k++) X[(size_t)(((k - kc) % N_ + N_) % N_)] = car[k];
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
