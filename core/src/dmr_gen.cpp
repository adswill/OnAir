// DMR test signal (see dmr_gen.h): the content of a base station or a handset as 4FSK symbols, the pulse shaping and the frequency modulator.
#include "dect2/dmr_gen.h"
#include "dect2/dmr_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace dect2 {
using namespace dmr;

namespace {

// ---------------------------------------------------------------------------------------------------- random numbers

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL) { next(); next(); }
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
    double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int n) { return n > 0 ? (int)(next() % (uint64_t)n) : 0; }
    // sum of eight uniform values: close enough to a Gaussian for noise that only has to look like noise
    float gauss() {
        const uint64_t a = next(), b = next();
        const float sum = (float)((a & 0xFFFF) + ((a >> 16) & 0xFFFF) + ((a >> 32) & 0xFFFF) + (a >> 48) + (b & 0xFFFF) + ((b >> 16) & 0xFFFF) + ((b >> 32) & 0xFFFF) + (b >> 48));
        return (sum * (1.f / 65535.f) - 4.f) * 1.2247449f;      // mean 4, variance 8/12 -> mean 0, variance 1
    }
};

// Gaussian noise from a table read at pseudo random places (one random number gives both axes): much cheaper than a Gaussian per sample
struct NoiseTable {
    static constexpr size_t kN = 1 << 18;
    std::vector<float> t;
    NoiseTable() {
        Rng r(20240612);
        t.resize(kN);
        for (auto& v : t) v = r.gauss();
    }
    static const NoiseTable& get() { static const NoiseTable n; return n; }
};

const uint32_t kUsers[8] = {2145007, 2145016, 2623266, 2308155, 250997, 2504105, 3101234, 2621042};
const uint32_t kGroups[8] = {9, 91, 2149, 3100, 262, 2621, 9990, 31337};
const char* kAliases[8] = {"Test Station 1", "OnAir", "Dispatch", "Mobile 7", "Base North", "Truck 12", "Ops Desk", "Radio 2623266"};
const char* kTexts[6] = {"OnAir DMR test message", "Hello from the synthetic DMR source", "Meeting at the main gate at 14:30", "Battery low on unit 7",
                         "Caf\xC3\xA9 is open, 20% off today", "Status: all clear"};

constexpr uint8_t kIdleInfo[12] = {0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91};   // table D.2

struct Burst {
    Bits bits;
    int truth = -1;
    bool first = false, last = false;
    int act = 0;                 // what the time slot is busy with (activity id of the short LC), 0 for idle
    uint32_t actDst = 0;
};

uint8_t crc8Of24(uint32_t v) {      // hash of an address (B.3.7): the 24 bits, then eight zeros
    unsigned c = 0;
    for (int i = 23; i >= 0; i--) {
        c = (c << 1) | ((v >> i) & 1);
        if (c & 0x100) c ^= 0x107;
    }
    for (int i = 0; i < 8; i++) {
        c <<= 1;
        if (c & 0x100) c ^= 0x107;
    }
    return (uint8_t)c;
}

// ---------------------------------------------------------------------------------------------------- what one time slot sends

class SlotScript {
public:
    SlotScript(const DmrGenConfig& c, int slot, int family, uint64_t seed, std::vector<DmrTruth>& truth)
        : c_(c), slot_(slot), family_(family), rng_(seed), truth_(truth) {}

    void setFamily(int f) { family_ = f; }
    bool pending() const { return !q_.empty(); }
    int activity() const { return activity_; }
    uint32_t activityDst() const { return actDst_; }

    // Base station: always returns a burst (idle when there is nothing else)
    Burst nextBs(double t) {
        if (q_.empty()) refillBs();
        return pop(t);
    }
    // Direct mode: queue one transmission
    void queueDirectCall() {
        const int r = rng_.range(10);
        if (r < 7) pushVoiceCall(0);
        else if (r < 8 && c_.textMessages) pushText();
        else pushVoiceCall(1);
    }
    Burst pop(double t) {
        Burst b = q_.front();
        q_.pop_front();
        if (b.truth >= 0) {
            DmrTruth& tr = truth_[(size_t)b.truth];
            if (b.first) tr.startSec = t;
            if (b.last) { tr.endSec = t + 0.030; tr.finished = true; }
        }
        activity_ = b.act;
        actDst_ = b.actDst;
        return b;
    }

private:
    const DmrGenConfig& c_;
    int slot_, family_;
    Rng rng_;
    std::vector<DmrTruth>& truth_;
    std::deque<Burst> q_;
    int activity_ = 0;
    uint32_t actDst_ = 0;
    int textCounter_ = 0, callCounter_ = 0;

    int sync(bool voice) const { return syncOf(family_, voice); }

    Burst dataBurst(int dt, const Bits& pay196, bool noSync = false) {
        Burst b;
        b.bits = makeDataBurst(c_.cc, dt, pay196, noSync ? -1 : sync(false));
        return b;
    }
    Burst bptcBurst(int dt, const Bits& info96, bool noSync = false) {
        Bits pay;
        bptc196Encode(info96, pay);
        return dataBurst(dt, pay, noSync);
    }

    void pushIdle(int n) {
        Bits info;
        bytesToBits(kIdleInfo, 12, info);
        for (int i = 0; i < n; i++) q_.push_back(bptcBurst(kDtIdle, info));
    }

    int newTruth(int kind, uint32_t src, uint32_t dst) {
        DmrTruth t;
        t.slot = slot_; t.kind = kind; t.src = src; t.dst = dst; t.cc = c_.cc;
        truth_.push_back(t);
        return (int)truth_.size() - 1;
    }

    void markEnds(size_t from, int truthId, int act, uint32_t dst) {
        if (q_.size() <= from) return;
        for (size_t i = from; i < q_.size(); i++) { q_[i].act = act; q_[i].actDst = dst; }
        q_[from].first = true;
        q_[from].truth = truthId;
        q_.back().last = true;
        q_.back().truth = truthId;
    }

    void pushVoiceCall(int kind) {
        const int nTg = std::max(1, std::min(8, c_.talkgroups));
        const uint32_t src = kUsers[(size_t)rng_.range(8)];
        uint32_t dst;
        if (kind == 1) { do dst = kUsers[(size_t)rng_.range(8)]; while (dst == src); }
        else if (kind == 2) dst = 0xFFFFFF;
        else dst = kGroups[(size_t)rng_.range(nTg)];
        FullLc lc;
        lc.flco = kind == 1 ? kFlcoUnitVoice : kFlcoGroupVoice;
        lc.svc = kind == 2 ? 0x08 : 0x00;
        lc.src = src; lc.dst = dst;
        uint8_t raw[9];
        packFullLc(lc, raw);
        const int traffic = c_.traffic;
        int nSuper = 2 + rng_.range(traffic == 2 ? 6 : 4);     // 0.7 s to 2.9 s
        const int ti = newTruth(kind, src, dst);
        DmrTruth& tr = truth_[(size_t)ti];
        std::string alias;
        FullLc ta[4];
        int taPieces = 0;
        if (c_.talkerAlias && nSuper >= 4 && (callCounter_++ % 2) == 0) {
            alias = kAliases[rng_.range(8)];
            const int fmt = (callCounter_ & 2) ? 3 : 1;
            if (fmt == 3 && alias.size() > 13) alias.resize(13);      // 16 bit characters: at most 13 in one header and three blocks
            while (!alias.empty() && alias.back() == ' ') alias.pop_back();
            taPieces = talkerAliasPdus(alias, fmt, ta);
            tr.alias = alias;
            nSuper = std::max(nSuper, 2 * taPieces);      // the pieces go out in every second superframe
        }
        const size_t first = q_.size();
        Bits info;
        lcBurstInfo(raw, false, info);
        q_.push_back(bptcBurst(kDtVoiceLcHeader, info, !c_.headerSync && family_ == 0));
        Bits frag[4], taFrag[4][4];
        embLcEncode(raw, frag);
        for (int p = 0; p < taPieces; p++) {
            uint8_t r2[9];
            memcpy(r2, ta[p].raw, 9);
            embLcEncode(r2, taFrag[p]);
        }
        Bits zero32(32, 0);
        for (int sf = 0; sf < nSuper; sf++) {
            // the embedded message of this superframe: the call's own LC, every other one the next piece of the talker alias
            const Bits* fr = frag;
            if (taPieces && sf % 2 == 1 && sf / 2 < taPieces) fr = taFrag[sf / 2];
            Bits vs(216);
            for (int burst = 0; burst < 6; burst++) {
                for (auto& x : vs) x = (uint8_t)(rng_.next() & 1);
                Burst b;
                if (burst == 0) b.bits = makeVoiceBurst(vs, sync(true), c_.cc, 0, 0, Bits());
                else if (burst < 5) b.bits = makeVoiceBurst(vs, -1, c_.cc, 0, burst == 1 ? 1 : burst == 4 ? 2 : 3, fr[burst - 1]);
                else b.bits = makeVoiceBurst(vs, -1, c_.cc, 0, 0, zero32);
                q_.push_back(b);
                tr.voiceBursts++;
            }
        }
        lcBurstInfo(raw, true, info);
        q_.push_back(bptcBurst(kDtTerminatorLc, info));
        markEnds(first, ti, kind == 1 ? 9 : 8, dst);
        // hang time: the base station keeps the channel reserved with more terminators (TS 102 361-2 clause 5.2)
        if (family_ == 0) for (int h = 0; h < c_.hangBursts; h++) q_.push_back(bptcBurst(kDtTerminatorLc, info));
    }

    void pushCsbk(int opcode, uint32_t src, uint32_t dst, int extra) {
        Csbk c;
        c.opcode = opcode;
        switch (opcode) {
        case 0x3D:      // preamble: data follows, target is an individual, blocks to follow
            c.data[0] = 0x80; c.data[1] = (uint8_t)extra; putBe24(c.data + 2, dst); putBe24(c.data + 5, src); break;
        case 0x04:      // unit to unit voice service request
            c.data[0] = 0x00; putBe24(c.data + 2, dst); putBe24(c.data + 5, src); break;
        case 0x38:      // BS outbound activation
            putBe24(c.data + 2, dst); putBe24(c.data + 5, src); break;
        default: break;
        }
        Bits info;
        csbkInfo(c, info);
        const size_t first = q_.size();
        q_.push_back(bptcBurst(kDtCsbk, info));
        const int ti = newTruth(4, src, dst);
        truth_[(size_t)ti].csbkOpcode = opcode;
        markEnds(first, ti, 3, dst);
    }

    void pushText() {
        const uint32_t src = kUsers[(size_t)rng_.range(8)];
        uint32_t dst;
        do dst = kUsers[(size_t)rng_.range(8)]; while (dst == src);
        const int idx = textCounter_++;
        std::string text = kTexts[rng_.range(6)];
        text += " #" + std::to_string(idx + 1);
        const int dtList[3] = {kDtRate12, kDtRate34, kDtRate1};
        const int dt = dtList[idx % 3];
        const int ddList[3] = {kDdIso8859_1, kDdUtf8, kDdUtf16Le};
        const int dd = ddList[(idx / 3) % 3];
        // the text in the chosen coding
        std::vector<uint8_t> data;
        if (dd == kDdUtf16Le) {         // the text is decoded as UTF-8 first
            size_t i = 0;
            while (i < text.size()) {
                const unsigned char c = (unsigned char)text[i];
                uint32_t cp = c;
                size_t len = 1;
                if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
                else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
                for (size_t k = 1; k < len && i + k < text.size(); k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
                i += len;
                data.push_back((uint8_t)cp); data.push_back((uint8_t)(cp >> 8));
            }
        } else if (dd == kDdIso8859_1) {
            size_t i = 0;
            while (i < text.size()) {
                const unsigned char c = (unsigned char)text[i];
                if (c >= 0xC0 && i + 1 < text.size()) { data.push_back((uint8_t)(((c & 0x1F) << 6) | ((unsigned char)text[i + 1] & 0x3F))); i += 2; }
                else { data.push_back(c); i++; }
            }
        } else {
            data.assign(text.begin(), text.end());
        }
        const int bs = dataBlockBytes(dt, false);
        const int nBlocks = ((int)data.size() + 4 + bs - 1) / bs;
        const int area = nBlocks * bs - 4;
        const int pad = area - (int)data.size();
        std::vector<uint8_t> msg = data;
        msg.resize((size_t)area, 0);
        const uint32_t crc = crc32Msg(msg.data(), msg.size());
        for (int i = 0; i < 4; i++) msg.push_back((uint8_t)(crc >> (8 * i)));
        const size_t first = q_.size();
        pushCsbk(0x3D, src, dst, nBlocks + 1);
        const size_t afterPre = q_.size();
        DataHeader h;
        h.dpf = kDpfShortDefined; h.sap = 10; h.group = false; h.dst = dst; h.src = src; h.blocks = nBlocks; h.dd = dd; h.fmf = true; h.pad = pad * 8;
        uint8_t hb[10];
        packDataHeader(h, hb);
        Bits info;
        crcBlockInfo(hb, kMaskDataHeader, info);
        q_.push_back(bptcBurst(kDtDataHeader, info));
        for (int b = 0; b < nBlocks; b++) {
            Bits pay;
            dataBlockEncode(dt, msg.data() + (size_t)b * (size_t)bs, (size_t)bs, false, 0, pay);
            q_.push_back(dataBurst(dt, pay, !c_.blockSync && family_ == 0 && (b & 1) == 1));
        }
        // the preamble burst keeps its own truth entry (a control message); the message gets one of its own
        const int ti = newTruth(3, src, dst);
        truth_[(size_t)ti].text = text;
        truth_[(size_t)ti].dataRate = dt;
        for (size_t i = afterPre; i < q_.size(); i++) { q_[i].act = 10; q_[i].actDst = dst; }
        q_[afterPre].first = true; q_[afterPre].truth = ti;
        q_.back().last = true; q_.back().truth = ti;
        (void)first;
    }

    void refillBs() {
        const int mix = c_.traffic == 1 ? 1 : c_.traffic == 2 ? 3 : 2;      // how often something happens instead of idle bursts
        const int r = rng_.range(100);
        const int idleChance = mix == 1 ? 75 : mix == 3 ? 15 : 40;
        if (r < idleChance) { pushIdle(3 + rng_.range(mix == 1 ? 40 : 14)); return; }
        const int k = rng_.range(100);
        if (k < 55) pushVoiceCall(0);
        else if (k < 68) pushVoiceCall(1);
        else if (k < 72) pushVoiceCall(2);
        else if (k < 82) {
            const uint32_t src = kUsers[(size_t)rng_.range(8)];
            uint32_t dst;
            do dst = kUsers[(size_t)rng_.range(8)]; while (dst == src);
            pushCsbk(rng_.range(2) ? 0x04 : 0x38, src, dst, 0);
            pushIdle(1 + rng_.range(3));
        } else if (c_.textMessages) { pushText(); pushIdle(2 + rng_.range(5)); }
        else pushVoiceCall(0);
        pushIdle(1 + rng_.range(4));
    }
};

// ---------------------------------------------------------------------------------------------------- pulse shaping and modulation

// Symbols in, samples at 48 kHz out (frequency in Hz and the carrier gate). A symbol's pulse is spread over +-6 symbols, so the output runs
// six symbols behind the input.
class Shaper {
public:
    Shaper() : g_(rrcTaps()) {
        for (auto& x : g_) x *= (float)kSps;               // a run of equal symbols must give the nominal deviation
        buf_.assign(g_.size() + kSps, 0.f);
        gateQ_.assign(13, 0);
    }
    // One symbol (value in +-3, +-1; `on` says whether the carrier is there for it). Appends 10 frequency samples and 10 gate samples.
    void push(float sym, bool on, std::vector<float>& f, std::vector<float>& gate) {
        for (size_t t = 0; t < g_.size(); t++) buf_[t] += sym * g_[t];
        gateQ_.push_back(on ? 1 : 0);
        gateQ_.pop_front();
        const bool gOn = gateQ_[6] != 0;      // the symbol whose pulse is now complete (six symbols back)
        for (int i = 0; i < kSps; i++) {
            f.push_back(buf_[(size_t)i + 0] * (float)kDevUnitHz);
            gateState_ = gOn ? std::min(1.f, gateState_ + kRamp) : std::max(0.f, gateState_ - kRamp);
            gate.push_back(gateState_);
        }
        std::copy(buf_.begin() + kSps, buf_.end(), buf_.begin());
        std::fill(buf_.end() - kSps, buf_.end(), 0.f);
    }
    // extra samples with the carrier off (a gap that is not a whole number of symbols)
    void pad(int n, std::vector<float>& f, std::vector<float>& gate) {
        for (int i = 0; i < n; i++) {
            f.push_back(0.f);
            gateState_ = std::max(0.f, gateState_ - kRamp);
            gate.push_back(gateState_);
        }
    }

private:
    static constexpr float kRamp = 1.0f / 72.0f;      // 1.5 ms power ramp (clause 10.2.3.1.1)
    std::vector<float> g_, buf_;
    std::deque<uint8_t> gateQ_;
    float gateState_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------------------------------- the signal

struct DmrSignal::Impl {
    DmrGenConfig c;
    Rng rng;
    Rng noiseRng;
    std::vector<DmrTruth> truth;
    SlotScript s1, s2;
    Shaper shaper;
    uint64_t slotCounter = 0, symbols = 0;
    int cachPhase = 0;
    Bits shortLc[4];
    // direct mode
    int directSlot = 0, gapSlots = 0;
    bool carrierOn = true;
    // symbol stream waiting to be shaped
    std::deque<std::pair<float, uint8_t>> symQ;
    // 48 kHz samples
    std::vector<float> f48, g48;
    uint64_t base48 = 0;
    double pos48 = 3;
    // modulator
    double phase = 0;
    cf32 z = cf32(1, 0);
    double amp = 0.2, sigma = 0.01;
    uint32_t count = 0;

    explicit Impl(const DmrGenConfig& cfg)
        : c(cfg), rng(cfg.seed * 7919ULL + 17), noiseRng(cfg.seed * 104729ULL + 5), s1(c, 1, 0, cfg.seed * 31ULL + 1, truth), s2(c, 2, 0, cfg.seed * 31ULL + 2, truth) {
        truth.reserve(1 << 16);
        if (c.direct) {
            if (c.mobile) { s1.setFamily(1); s2.setFamily(1); }
            else { s1.setFamily(2); s2.setFamily(3); }
        }
        const double snr = std::pow(10.0, c.snrDb / 10.0);
        const double noiseRatio = c.rate / 12500.0 / snr;       // total noise power over signal power
        amp = c.rms / std::sqrt(1.0 + noiseRatio);
        sigma = amp * std::sqrt(noiseRatio / 2.0);
        f48.assign(8, 0.f); g48.assign(8, 0.f);
    }

    double now() const { return (double)symbols / kSymbolRate; }

    void makeShortLc() {
        uint32_t lc = 0;
        static int toggle = 0;
        if ((toggle++ & 1) == 0) {
            // activity update: SLCO 1, the activity of both time slots and the hashed address of their destinations
            lc = (1u << 24) | (uint32_t)s1.activity() << 20 | (uint32_t)s2.activity() << 16 | (uint32_t)crc8Of24(s1.activityDst()) << 8 | crc8Of24(s2.activityDst());
        }
        shortLcEncode(lc, shortLc);
    }

    void pushBurstSymbols(const Burst& b, bool withCach, int slotIdx, bool active) {
        if (withCach) {
            // CACH: TACT (channel number of the burst after it, access type) and one 17 bit piece of the short LC
            if (cachPhase == 0) makeShortLc();
            const int lcss = cachPhase == 0 ? 1 : cachPhase == 3 ? 2 : 3;
            unsigned payload = 0;
            for (int i = 0; i < 17; i++) payload = (payload << 1) | shortLc[cachPhase][(size_t)i];
            Bits cach;
            cachEncode(active ? 1 : 0, slotIdx, lcss, payload, cach);
            std::vector<int8_t> cs;
            bitsToSymbols(cach, cs);
            for (int8_t s : cs) symQ.push_back({(float)s, 1});
            cachPhase = (cachPhase + 1) & 3;
        }
        std::vector<int8_t> sy;
        bitsToSymbols(b.bits, sy);
        for (int8_t s : sy) symQ.push_back({(float)s, 1});
    }

    void fillSymbols() {
        if (!c.direct) {
            const int slotIdx = (int)(slotCounter & 1);
            SlotScript& sc = slotIdx == 0 ? s1 : s2;
            Burst b = sc.nextBs(now());
            pushBurstSymbols(b, true, slotIdx, sc.activity() != 0);
            slotCounter++;
            symbols += kSlotSymbols;
            return;
        }
        // direct mode: a transmission uses one time slot, a burst every 60 ms; the rest of the time the carrier is off
        const int slotIdx = (int)(slotCounter & 1);
        SlotScript* sc = directSlot == 1 ? &s1 : directSlot == 2 ? &s2 : nullptr;
        if (sc && !sc->pending()) { directSlot = 0; sc = nullptr; gapSlots = 2 * (8 + rng.range(c.traffic == 2 ? 20 : c.traffic == 1 ? 80 : 40)); }
        if (!sc && gapSlots > 0) {
            gapSlots--;
        } else if (!sc) {
            directSlot = 1 + rng.range(2);
            sc = directSlot == 1 ? &s1 : &s2;
            sc->queueDirectCall();
            // start between symbol instants: the carrier comes up after a gap of a random number of 48 kHz samples
            const int pad = rng.range(kSps);
            extraPad += pad;
        }
        if (sc && slotIdx == directSlot - 1) {
            Burst b = sc->pop(now());
            std::vector<int8_t> sy;
            bitsToSymbols(b.bits, sy);
            for (int8_t s : sy) symQ.push_back({(float)s, 1});
            for (int i = 0; i < kCachSymbols; i++) symQ.push_back({0.f, 0});
        } else {
            for (int i = 0; i < kSlotSymbols; i++) symQ.push_back({0.f, 0});
        }
        slotCounter++;
        symbols += kSlotSymbols;
    }
    int extraPad = 0;

    void produce48(size_t upTo) {
        while (base48 + f48.size() < upTo) {
            if (symQ.empty()) fillSymbols();
            if (extraPad > 0 && c.direct) {
                const int n = extraPad;
                extraPad = 0;
                shaper.pad(n, f48, g48);
            }
            const auto s = symQ.front();
            symQ.pop_front();
            shaper.push(s.first, carrierOn && s.second != 0, f48, g48);
        }
    }

    static float cubic(const float* p, float t) {      // p[-1], p[0], p[1], p[2]; Catmull-Rom
        return p[0] + 0.5f * t * (p[1] - p[-1] + t * (2.f * p[-1] - 5.f * p[0] + 4.f * p[1] - p[2] + t * (3.f * (p[0] - p[1]) + p[2] - p[-1])));
    }

    void generate(cf32* out, size_t n) {
        const double step = (kWorkRate / c.rate) * (1.0 + c.sroPpm * 1e-6);
        const double dt = (1.0 + c.sroPpm * 1e-6) / c.rate;
        const double gdb = std::pow(10.0, c.iqImbalanceDb / 20.0), th = c.iqImbalanceDb * 3.0 * kPi / 180.0;
        const float cth = (float)std::cos(th), sth = (float)std::sin(th);
        const float sig = (float)sigma, dco = (float)c.dcOffset;
        const bool imb = c.iqImbalanceDb != 0;
        float zr = z.real(), zi = z.imag();
        const float* nz = NoiseTable::get().t.data();
        const uint64_t nmask = NoiseTable::kN - 1;
        constexpr size_t kBlk = 256;
        float fA[kBlk], aA[kBlk];
        const float ampF = (float)amp;
        const double wScale = 2.0 * kPi * dt;
        for (size_t i0 = 0; i0 < n; i0 += kBlk) {
            const size_t nb = std::min(kBlk, n - i0);
            // the shaped samples this block reads, made in one go
            produce48((size_t)(pos48 + step * (double)(nb - 1)) + 4);
            // the frequency and the gate at the output samples
            for (size_t i = 0; i < nb; i++) {
                const double pos = pos48 + step * (double)i;
                const size_t ip = (size_t)pos;
                const size_t k = ip - (size_t)base48;
                const float frac = (float)(pos - (double)ip);
                fA[i] = cubic(&f48[k], frac);
                const float gt = g48[k] + frac * (g48[k + 1] - g48[k]);
                // the carrier gate: a raised cosine on the power ramps, flat (exactly) before and after
                aA[i] = gt >= 1.f ? ampF : gt <= 0.f ? 0.f : ampF * 0.5f * (1.f - std::cos((float)kPi * gt));
            }
            pos48 += step * (double)nb;
            // the modulator: rotate by dphi with a short series (dphi stays below 0.1 rad for rates of 1 Msps and up), then the noise
            for (size_t i = 0; i < nb; i++) {
                const float d = (float)(((double)fA[i] * c.devScale + c.cfoHz) * wScale), d2 = d * d;
                const float rc = 1.f - 0.5f * d2 + d2 * d2 * (1.f / 24.f), rs = d * (1.f - d2 * (1.f / 6.f));
                const float zr2 = zr * rc - zi * rs;
                zi = zr * rs + zi * rc; zr = zr2;
                if ((++count & 1023) == 0) { const float m = 1.f / std::sqrt(zr * zr + zi * zi); zr *= m; zi *= m; }
                const uint64_t r = noiseRng.next();
                float re = aA[i] * zr + sig * nz[r & nmask] + dco;
                float im = aA[i] * zi + sig * nz[(r >> 24) & nmask];
                if (imb) im = (float)gdb * (im * cth + re * sth);
                out[i0 + i] = cf32(re, im + dco * 0.5f);
            }
        }
        z = cf32(zr, zi);
        // drop what is behind us
        const size_t ip = (size_t)pos48;
        if (ip > base48 + 4096 + 4) {
            const size_t drop = ip - (size_t)base48 - 4;
            f48.erase(f48.begin(), f48.begin() + (ptrdiff_t)drop);
            g48.erase(g48.begin(), g48.begin() + (ptrdiff_t)drop);
            base48 += drop;
        }
    }
};

DmrSignal::DmrSignal(const DmrGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
DmrSignal::~DmrSignal() = default;
void DmrSignal::generate(cf32* out, size_t n) { p_->generate(out, n); }
const std::vector<DmrTruth>& DmrSignal::truth() const { return p_->truth; }
double DmrSignal::seconds() const { return p_->now(); }
void DmrSignal::setCarrier(bool on) { p_->carrierOn = on; }

DmrGenConfig dmrGenConfigFrom(const SynthConfig& s, double rate) {
    DmrGenConfig c;
    c.rate = rate;
    c.cc = s.modeOpt[0] == 0 ? 1 : s.modeOpt[0] < 0 ? 0 : s.modeOpt[0] & 15;
    c.talkgroups = s.modeOpt[1] <= 0 ? 3 : std::min(8, s.modeOpt[1]);
    c.direct = s.modeOpt[2] == 1 || s.modeOpt[2] == 2;
    c.mobile = s.modeOpt[2] == 2;
    c.traffic = s.modeOpt[3] == 1 ? 1 : s.modeOpt[3] == 2 ? 2 : 0;
    c.seed = s.modeOpt[4] == 0 ? 1u : (uint32_t)s.modeOpt[4];
    c.textMessages = !(s.modeOpt[5] & 1);
    c.talkerAlias = !(s.modeOpt[5] & 2);
    c.hangBursts = s.modeOpt[6] == 0 ? 6 : std::max(0, std::min(100, s.modeOpt[6]));
    c.headerSync = !(s.modeOpt[5] & 4);
    c.blockSync = !(s.modeOpt[5] & 8);
    c.devScale = s.modeVal[0] > 0 ? s.modeVal[0] : 1.0;
    c.dcOffset = s.modeVal[1];
    c.iqImbalanceDb = s.modeVal[2];
    c.snrDb = s.snrDb;
    c.cfoHz = s.cfoHz;
    c.sroPpm = s.sroPpm;
    return c;
}

namespace {
class DmrSynth : public ModeSynth {
public:
    DmrSynth(const SynthConfig& cfg, double rate) : rate_(rate), sig_(dmrGenConfigFrom(cfg, rate)) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { sig_.generate(out, n); }
private:
    double rate_;
    DmrSignal sig_;
};
} // namespace

std::unique_ptr<ModeSynth> makeDmrSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 1e6 || sampleRate > 20.5e6) return nullptr;
    return std::make_unique<DmrSynth>(cfg, sampleRate);
}

} // namespace dect2
