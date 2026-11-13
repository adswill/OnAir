// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISDB-S test signal generator, see isdbs.h. Nothing is transmitted: the samples only exist in memory or in a file.
#include "dect2/isdbs/isdbs.h"
#include "dect2/dvbs/dvbs_gen.h"
#include "isdbs_fec.h"
#include "dect2/fec/fec_proto.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace dect2 {
namespace isdbs {

bool tmccFromStreams(const std::vector<StreamCfg>& in, int change, Tmcc& t, std::string* err, std::vector<int>* orderOut) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (in.empty() || in.size() > (size_t)kMaxStreams) return fail("1 to 8 streams");
    // most efficient first (BO.1408 3.2); streams of one modulation keep their order, and the relative numbers follow the slots (TR-B15 6.1.1)
    std::vector<int> order(in.size());
    for (size_t i = 0; i < in.size(); i++) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return efficiencyRank(in[(size_t)a].mod) < efficiencyRank(in[(size_t)b].mod); });
    Tmcc r;
    r.change = change & 31;
    int slot = 0, groups = 0;
    for (size_t i = 0; i < order.size(); i++) {
        const StreamCfg& c = in[(size_t)order[i]];
        if (!isMod(c.mod)) return fail("a stream needs a modulation");
        if (c.slots < 1 || c.slots % slotUnit(c.mod)) return fail("the slots of a stream must be a whole number of units of its modulation");
        if (slot + c.slots > kSlots) return fail("more than 48 slots");
        if (c.tsId == 0xFFFF) return fail("TS id 0xFFFF marks an unused relative number");
        if (groups > 0 && r.mode[groups - 1] == c.mod) r.count[groups - 1] += c.slots;
        else {
            if (groups >= kMaxGroups) return fail("more than four modulations");
            r.mode[groups] = c.mod; r.count[groups] = c.slots; groups++;
        }
        for (int k = 0; k < c.slots; k++) r.slotTs[(size_t)(slot + k)] = (uint8_t)i;
        r.tsId[i] = c.tsId;
        slot += c.slots;
    }
    if (slot != kSlots) return fail("the streams must take all 48 slots (as on the air)");
    r.valid = true;
    t = r;
    if (orderOut) *orderOut = order;
    return true;
}

struct Generator::Impl {
    GenConfig cfg;
    std::string err;
    bool ok = false;
    Tmcc tmcc;                                          // layout of the super-frame being sent
    uint64_t frame = 0, super = 0;
    uint64_t packets = 0;
    FrameEncoder enc;
    // pulse shaping
    static constexpr int kPhases = 128;
    int half = 10;
    std::vector<float> taps;                            // kPhases + 1 rows of 2 * half
    std::vector<cf32> hist;                             // symbols, hist[0] has index base
    long long base = 0;
    double t = 0, dt = 0;                               // position of the next sample in symbols; symbols per sample
    std::complex<double> rot{1.0, 0.0}, step{1.0, 0.0};
    std::mt19937 rng;
    std::normal_distribution<float> nd{0.f, 1.f};
    float noiseSigma = 0, scale = 1;
    std::vector<cf32> pending;                          // shaped samples not handed out yet
    size_t pendPos = 0;

    bool second(uint64_t sf) const { return cfg.changeAfterSuper >= 0 && sf >= (uint64_t)cfg.changeAfterSuper; }
    const std::vector<StreamCfg>& streamsAt(uint64_t sf) const { return second(sf) ? cfg.streams2 : cfg.streams; }

    // the eight frames of the next super-frame
    void superFrame(std::vector<cf32>& out) {
        const uint64_t sf = super++;
        std::vector<int> order;
        Tmcc now, announced;
        tmccFromStreams(streamsAt(sf), second(sf) ? 1 : 0, now, nullptr, &order);
        // the TMCC is sent two super-frames ahead of the frames it describes (TR-B15 6.2.1)
        tmccFromStreams(streamsAt(sf + 2), second(sf + 2) ? 1 : 0, announced);
        tmcc = now;
        const std::vector<StreamCfg>& st = streamsAt(sf);
        Layout L;
        L.build(now);
        // packets -> RS(204,188) -> energy dispersal (the sync byte is left alone and not sent)
        static thread_local std::vector<uint8_t> orig, inter;
        orig.assign((size_t)kFramesPerSuper * kSlots * kSlotBytes, 0);
        inter.assign(orig.size(), 0);
        for (int f = 0; f < kFramesPerSuper; f++)
            for (int s = 0; s < kSlots; s++) {
                uint8_t ts[188], rs[204];
                memset(ts, 0xFF, sizeof ts); ts[0] = 0x47; ts[1] = 0x1F; ts[2] = 0xFF; ts[3] = 0x10;   // null packet (dummy slots)
                if (L.ordinal[(size_t)s] >= 0) {
                    const StreamCfg& c = st[(size_t)order[now.slotTs[(size_t)s]]];
                    if (c.src) c.src(ts);
                    ts[0] = 0x47;
                    packets++;
                }
                fecp::rs204Encode(ts, rs);
                uint8_t* o = &orig[(size_t)(f * kSlots + s) * kSlotBytes];
                for (int b = 1; b < 204; b++) o[b - 1] = rs[b] ^ prbsByte(f, s, b);
            }
        // byte interleaving of each slot across the eight frames
        for (int f = 0; f < kFramesPerSuper; f++)
            for (int s = 0; s < kSlots; s++)
                for (int j = 0; j < kSlotBytes; j++) {
                    int F, k;
                    interleavePos(f, j, F, k);
                    inter[(size_t)(F * kSlots + s) * kSlotBytes + (size_t)k] = orig[(size_t)(f * kSlots + s) * kSlotBytes + (size_t)j];
                }
        uint8_t tm[kTmccCoded];
        const auto info = tmccPack(announced);
        tmccEncode(info.data(), tm);
        out.resize((size_t)kFramesPerSuper * kFrameSyms);
        std::vector<std::vector<uint8_t>> gb(L.groups.size());
        for (int f = 0; f < kFramesPerSuper; f++) {
            for (size_t g = 0; g < L.groups.size(); g++) {
                gb[g].clear();
                const auto& G = L.groups[g];
                for (int s = G.firstSlot; s < G.firstSlot + G.count; s++)
                    if (L.ordinal[(size_t)s] >= 0) { const uint8_t* p = &inter[(size_t)(f * kSlots + s) * kSlotBytes]; gb[g].insert(gb[g].end(), p, p + kSlotBytes); }
            }
            uint8_t hb[kHeaderBits];
            headerBits(tm, f, hb);
            if (cfg.burstData) {
                std::vector<float> bd((size_t)kBlocks * kBurstSyms);
                for (auto& v : bd) v = (rng() & 1) ? -1.f : 1.f;
                enc.encode(L, hb, gb, &out[(size_t)f * kFrameSyms], bd.data());
            } else {
                enc.encode(L, hb, gb, &out[(size_t)f * kFrameSyms]);
            }
            frame++;
        }
    }

    void ensureSymbols(long long upto) {   // symbols with index < upto exist
        while (base + (long long)hist.size() < upto) {
            std::vector<cf32> f;
            superFrame(f);
            hist.insert(hist.end(), f.begin(), f.end());
        }
    }

    void produce(size_t n) {
        pending.erase(pending.begin(), pending.begin() + (long)pendPos);
        pendPos = 0;
        const size_t old = pending.size();
        pending.resize(old + n);
        for (size_t i = 0; i < n; i++) {
            const long long k0 = (long long)std::floor(t);
            ensureSymbols(k0 + half + 2);
            const double fr = t - (double)k0;
            const double ph = fr * kPhases;
            const int p0 = (int)ph;
            const float w = (float)(ph - p0);
            const float* r0 = &taps[(size_t)p0 * 2 * (size_t)half];
            const float* r1 = &taps[(size_t)(p0 + 1) * 2 * (size_t)half];
            float re = 0, im = 0;
            // output = sum over symbols k of s[k] h(t - k), k from k0 - half + 1 to k0 + half
            const cf32* sy = &hist[(size_t)(k0 - half + 1 - base)];
            for (int m = 0; m < 2 * half; m++) {
                const float h = r0[m] + w * (r1[m] - r0[m]);
                re += sy[m].real() * h; im += sy[m].imag() * h;
            }
            std::complex<double> o = std::complex<double>(re, im) * rot;
            rot *= step;
            if ((i & 1023) == 1023) rot /= std::abs(rot);
            float nr = 0, ni = 0;
            if (noiseSigma > 0) { nr = nd(rng) * noiseSigma; ni = nd(rng) * noiseSigma; }
            pending[old + i] = cf32(((float)o.real() + nr) * scale, ((float)o.imag() + ni) * scale);
            t += dt;
        }
        // drop symbols that can no longer be reached
        const long long keepFrom = (long long)std::floor(t) - half;
        if (keepFrom - base > 10 * kFrameSyms) {
            const long long cut = keepFrom - base;
            hist.erase(hist.begin(), hist.begin() + cut);
            base += cut;
        }
    }
};

Generator::Generator(const GenConfig& cfg) : p_(new Impl) {
    Impl& m = *p_;
    m.cfg = cfg;
    if (!tmccFromStreams(cfg.streams, 0, m.tmcc, &m.err)) return;
    if (cfg.changeAfterSuper >= 0) { Tmcc t2; if (!tmccFromStreams(cfg.streams2, 1, t2, &m.err)) return; }
    m.rng.seed(cfg.seed);
    m.half = std::max(6, 10);
    m.taps.assign((size_t)(Impl::kPhases + 1) * 2 * (size_t)m.half, 0.f);
    for (int p = 0; p <= Impl::kPhases; p++)
        for (int mm = 0; mm < 2 * m.half; mm++) {
            // sample at fractional position fr = p / kPhases after symbol k0; symbol k = k0 - half + 1 + mm sits (fr + half - 1 - mm) symbol periods away
            const double d = (double)p / Impl::kPhases + (double)(m.half - 1 - mm);
            m.taps[(size_t)p * 2 * (size_t)m.half + (size_t)mm] = (float)dvbs::rrcAt(d, kRollOff);
        }
    m.dt = cfg.symbolRate * (1.0 + cfg.clockPpm * 1e-6) / cfg.sampleRate;
    m.t = 0;
    m.base = -(long long)m.half;
    m.hist.assign((size_t)m.half, cf32(0, 0));
    const double w = 2.0 * dect2::math::kPi * cfg.cfoHz / cfg.sampleRate;
    m.step = std::complex<double>(std::cos(w), std::sin(w));
    if (cfg.snrDb < 150) {
        const double esn0 = std::pow(10.0, cfg.snrDb / 10.0);
        const double var = (cfg.sampleRate / cfg.symbolRate) / esn0;   // complex noise power per sample for unit signal power
        m.noiseSigma = (float)std::sqrt(var / 2.0);
        m.scale = (float)(cfg.level / std::sqrt(1.0 + var));
    } else {
        m.scale = (float)cfg.level;
    }
    m.ok = true;
}

Generator::~Generator() = default;
bool Generator::ok() const { return p_->ok; }
const std::string& Generator::error() const { return p_->err; }
const Tmcc& Generator::tmcc() const { return p_->tmcc; }
uint64_t Generator::packetsSent() const { return p_->packets; }
uint64_t Generator::framesSent() const { return p_->frame; }
double Generator::sampleRate() const { return p_->cfg.sampleRate; }

void Generator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    if (!m.ok) { std::fill(out, out + n, cf32(0, 0)); return; }
    size_t done = 0;
    while (done < n) {
        if (m.pendPos >= m.pending.size()) m.produce(std::min<size_t>(65536, n - done > 4096 ? n - done : 4096));
        const size_t take = std::min(n - done, m.pending.size() - m.pendPos);
        memcpy(out + done, m.pending.data() + m.pendPos, take * sizeof(cf32));
        m.pendPos += take;
        done += take;
    }
}

}  // namespace isdbs
}  // namespace dect2
