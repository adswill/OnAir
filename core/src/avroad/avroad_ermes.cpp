// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ERMES paging, ETS 300 133-4: 4-level FM, 3125 baud, one batch of 154 (or, for the last batch of a subsequence, 190) codewords of 30 bits at a time. Written from the published
// specification. No public ERMES recording was found and no networks are known to be on the air: simulated signals only.
#include "avroad_int.h"
#include "dect2/avroad/avroad_proto.h"
#include <cstring>
#include <unordered_map>

namespace dect2::avroad {

namespace {
constexpr uint32_t kGen = 0b1101011001101;      // x^12 + x^11 + x^9 + x^7 + x^6 + x^3 + x^2 + 1
uint32_t rem30(uint32_t w) {
    for (int i = 29; i >= 12; i--) if (w >> i & 1) w ^= kGen << (i - 12);
    return w & 0xFFF;
}
}

uint32_t ermesEncode(uint32_t m) { const uint32_t w = (m & 0x3FFFF) << 12; return w | rem30(w); }

bool ermesDecode(uint32_t w, uint32_t& info, int* fixed) {
    static const auto table = [] {
        std::unordered_map<uint32_t, uint32_t> t;
        for (int i = 0; i < 30; i++) {
            t[rem30(1u << i)] = 1u << i;
            for (int j = i + 1; j < 30; j++) t[rem30(1u << i | 1u << j)] = 1u << i | 1u << j;
        }
        return t;
    }();
    const uint32_t s = rem30(w);
    if (fixed) *fixed = 0;
    if (s) {
        auto it = table.find(s);
        if (it == table.end()) return false;
        w ^= it->second;
        if (fixed) *fixed = __builtin_popcount(it->second);
    }
    info = w >> 12;
    return true;
}

ErmesSystem ermesSystem(uint32_t a, uint32_t b, uint32_t c) {
    ErmesSystem s;
    s.country = a >> 11 & 0x7F; s.oper = a >> 8 & 7; s.area = a >> 2 & 0x3F; s.eti = a >> 1 & 1; s.bai = a & 1;
    s.fsi = b >> 13 & 0x1F; s.cycle = b >> 7 & 0x3F; s.subseq = b >> 4 & 7; s.batch = b & 0xF;
    s.ssit = c >> 14 & 0xF; s.ssif = c & 0x3FFF;
    return s;
}

std::string ermesSystemText(const ErmesSystem& s) {
    std::string t = fmt("country %d operator %d area %d, cycle %d subsequence %d batch %c", s.country, s.oper, s.area, s.cycle, s.subseq, 'A' + s.batch);
    if (s.ssit == 0) t += fmt(", zone %d hour %d day %d", s.ssif >> 11, s.ssif >> 6 & 0x1F, s.ssif >> 1 & 0x1F);
    return t;
}

std::string ermesMessageText(int cat, const std::vector<uint32_t>& d) {
    if (cat == 0) return "tone";
    std::vector<uint8_t> bits;
    for (uint32_t w : d) for (int i = 17; i >= 0; i--) bits.push_back((uint8_t)(w >> i & 1));
    std::string s;
    if (cat == 1) {
        for (size_t i = 0; i + 4 <= bits.size(); i += 4) {
            const int v = bits[i] << 3 | bits[i + 1] << 2 | bits[i + 2] << 1 | bits[i + 3];
            s += v < 10 ? (char)('0' + v) : (char)('A' + v - 10);      // the codes above 9 (ETS 300 133-2 annex B) are shown as hexadecimal digits
        }
        while (!s.empty() && (s.back() == 'C' || s.back() == 'F')) s.pop_back();
        return s;
    }
    if (cat == 2) {
        for (size_t i = 0; i + 7 <= bits.size(); i += 7) {
            int v = 0; for (int k = 0; k < 7; k++) v = v << 1 | bits[i + (size_t)k];
            if (v == 0x11) break;      // EOM
            s += v >= 32 && v < 127 ? (char)v : '.';
        }
        return s;
    }
    char b[8];
    for (uint32_t w : d) { snprintf(b, sizeof b, "%05X ", w); s += b; }
    return s;
}

namespace {

constexpr int kBaud = 3125, kSps = 8, kWordSyms = 15;
constexpr int kBatch = 154, kBatchLast = 190;
constexpr float kOuter = 4687.5f, kInner = 1562.5f, kTrack = 120;   // hertz of mean early-late difference that moves the symbol clock by one sample

inline int sym2bits(float v) {       // Gray: +4687 -> 10, +1562 -> 11, -1562 -> 01, -4687 -> 00
    if (v > 0) return v > (kOuter + kInner) / 2 ? 2 : 3;
    return v < -(kOuter + kInner) / 2 ? 0 : 1;
}

class ErmesDec : public Decoder {
public:
    bool configure(double fs) override {
        if (!ch_.configure(fs, kBaud * kSps, 6500, 9500)) return false;
        reset();
        return true;
    }
    void reset() override {
        ch_.reset(); prev_ = cf32(1, 0); n_ = 0; integ_ = 0; memset(buf_, 0, sizeof buf_); memset(hist_, 0, sizeof hist_); act_ = nullptr; wait_ = 0; terr_ = 0; tn_ = 0;
        for (auto& p : ph_) { p.reg = 0; p.hi = 0; p.ema = 0; p.active = false; p.wordsDone = 0; p.nsym = 0; p.cur = 0; p.block.clear(); }
    }
    void feed(const cf32* x, size_t n) override {
        ch_.process(x, n, y_);
        for (const cf32& v : y_) {
            const float f = std::arg(v * std::conj(prev_)) * (float)(kBaud * kSps / (2 * kPi));
            prev_ = v;
            const int k = (int)(n_ % kSps);
            integ_ += f - buf_[k]; buf_[k] = f;
            n_++;
            for (int i = 4; i > 0; i--) hist_[i] = hist_[i - 1];
            hist_[0] = integ_ / kSps;
            if (act_) {
                if (--wait_ == 0) { wait_ = kSps; tracked(); }
            } else symbol(ph_[n_ % kSps], hist_[2]);
        }
    }
    void tracked() {
        const float mid = hist_[2], early = hist_[4], late = hist_[0];
        symbol(*act_, mid);
        // the symbol integrator peaks at the centre: late above early means the peak is yet to come
        const float e = (late - early) * (mid - act_->dc > 0 ? 1.f : -1.f);
        terr_ += e; if (++tn_ >= 16) { const float m = terr_ / (float)tn_; tn_ = 0; terr_ = 0; if (m > kTrack) wait_ += 1; else if (m < -kTrack) wait_ -= 1; }
        if (act_ && !act_->active) act_ = nullptr;
    }
    void fill(AvRoadTelemetry& t) const override {
        t.headline = batches_ ? fmt("%llu batches, %llu addresses, %llu messages", (unsigned long long)batches_, (unsigned long long)addresses_, (unsigned long long)messages_) : std::string("listening for ERMES batches");
        t.facts.push_back({"network", net_.empty() ? std::string("-") : net_});
        t.facts.push_back({"codewords repaired", fmt("%llu", (unsigned long long)repaired_)});
        t.facts.push_back({"channels", "169.425 to 169.800 MHz, 25 kHz steps; tune one channel"});
    }
private:
    struct Ph {
        float hist[30] = {}; int hi = 0; uint64_t reg = 0; float ema = 0; bool active = false; float dc = 0;
        int nsym = 0, wordsDone = 0; uint32_t cur = 0;
        int stage = 0, addrWords = 0, aptSeen = 0, blocksLeft = 0, total = 0;
        uint32_t si[3] = {}; std::vector<uint32_t> addrs; std::vector<uint32_t> blockBits; std::vector<uint32_t> block;
        std::vector<uint32_t> words; int badInBlock = 0;
        int mstate = 0; uint32_t hdr0 = 0; int cat = 0, aii = 0; uint32_t addr = 0; std::vector<uint32_t> mdata;
        ErmesSystem sys;
    };
    void symbol(Ph& p, float s) {
        if (p.active) {
            p.cur = p.cur << 2 | (uint32_t)sym2bits(s - p.dc);
            if (++p.nsym == kWordSyms) { p.nsym = 0; uint32_t w = p.cur & 0x3FFFFFFF; p.cur = 0; word(p, w); }
            return;
        }
        p.hist[p.hi % 30] = s; p.hi++;
        if (p.hi < 30) return;
        // The preamble (eight -4687 and seven +4687 Hz symbols) measures the carrier offset, then the 30 symbols are compared
        float m = 0; for (int i = 0; i < 15; i++) m += p.hist[(p.hi + i) % 30];
        const float dc = m / 15 + 312.5f;
        static const uint64_t kWant = (uint64_t)kErmesPreamble << 30 | kErmesSync;
        int errs = 0; float rd = 0;
        for (int i = 0; i < 30 && errs <= 2; i++) {
            const int want = (int)(kWant >> (2 * (29 - i)) & 3);
            const float v = p.hist[(p.hi + i) % 30] - dc;
            rd += std::fabs(v);
            if (sym2bits(v) != want) errs++;
        }
        if (errs <= 2 && rd / 30 > 1800) begin(p);
        (void)m;
    }
    void begin(Ph& p) {
        float m = 0; for (int i = 0; i < 15; i++) m += p.hist[(p.hi + i) % 30];
        p.dc = m / 15 + 312.5f; act_ = &p; wait_ = kSps; terr_ = 0; tn_ = 0;
        for (auto& q : ph_) if (&q != &p) { q.hi = 0; }
        p.active = true; p.stage = 0; p.nsym = 0; p.cur = 0; p.wordsDone = 2; p.addrs.clear(); p.aptSeen = 0; p.addrWords = 0; p.words.clear(); p.badInBlock = 0; p.block.clear(); p.mstate = 0; p.mdata.clear();
        out->energy(); syncs_++;
    }
    void word(Ph& p, uint32_t w) {
        p.wordsDone++;
        uint32_t info = 0; int fx = 0;
        const bool ok = ermesDecode(w, info, &fx);
        if (fx) repaired_++;
        if (p.stage == 0) {            // SI, SI, SSI
            int i = p.wordsDone - 3;
            if (!ok) { fail(p); return; }
            p.si[i] = info;
            if (i == 2) { p.sys = ermesSystem(p.si[0], p.si[1], p.si[2]); p.stage = 1; p.total = p.sys.batch == 15 ? kBatchLast : kBatch; }
            return;
        }
        if (p.stage == 1) {            // addresses, then terminators up to the codeblock boundary
            const bool apt = __builtin_popcount(w ^ kErmesApt) <= 2;
            if (apt) p.aptSeen++;
            if (p.aptSeen) { if ((p.wordsDone - 1) % 9 == 0) startMessages(p); return; }   // 5 + I + J - 1 is a multiple of 9 (Table 1)
            if (!ok) { p.badInBlock++; if (p.badInBlock > 4) { fail(p); return; } return; }
            p.addrs.push_back(info);
            if (p.wordsDone >= 145) { finish(p); }
            return;
        }
        // Stage 2: codeblocks, nine by thirty, column by column: collect then undo
        p.block.push_back(w);
        if (p.block.size() == 9) { codeblock(p); p.block.clear(); if (--p.blocksLeft <= 0) { finish(p); return; } }
    }
    void startMessages(Ph& p) {
        p.stage = 2; p.blocksLeft = (p.total - p.wordsDone) / 9; p.block.clear();
        batches_++;
        net_ = ermesSystemText(p.sys);
        if (!p.addrs.empty()) {
            addresses_ += p.addrs.size();
            std::string a; for (uint32_t v : p.addrs) a += fmt("%u ", v);
            out->row("Addresses", fmt("batch %c", 'A' + p.sys.batch), a, 0);
        }
        out->good();
        if (p.blocksLeft <= 0) finish(p);
    }
    void codeblock(Ph& p) {
        uint32_t w[9] = {};
        // 270 received bits in transmit order, column by column; "block" holds them in received 30 bit groups
        int bitIndex = 0;
        for (uint32_t rx : p.block) for (int i = 29; i >= 0; i--) { const int col = bitIndex / 9, row = bitIndex % 9; w[row] |= (rx >> i & 1u) << (29 - col); bitIndex++; }
        for (uint32_t c : w) {
            if (__builtin_popcount(c ^ kErmesDelim) <= 2) { endMessage(p); continue; }
            uint32_t info = 0; int fx = 0;
            if (!ermesDecode(c, info, &fx)) { p.mstate = 3; continue; }       // a damaged word: the rest of this message is skipped
            if (fx) repaired_++;
            if (p.mstate == 0) { p.hdr0 = info; p.mstate = 1; }
            else if (p.mstate == 1) {
                p.addr = p.hdr0 >> 0 & 0x3FFFF; p.addr = (p.hdr0 << 4 | info >> 14) & 0x3FFFFF;   // 22 bit local address, then number, external, AII, VIF
                p.aii = info >> 7 & 1; p.cat = (info >> 3 & 3); p.mstate = 2; p.mdata.clear();
            } else if (p.mstate == 2) p.mdata.push_back(info);
        }
    }
    void endMessage(Ph& p) {
        if (p.mstate >= 1) {
            if (p.mstate == 2 || p.mstate == 1) {
                messages_++;
                std::string txt;
                if (p.aii) txt = "(additional information, not read)";
                else txt = ermesMessageText(p.cat, p.mdata);
                out->row("Message", fmt("%u", p.addr), txt, 0);
            }
        }
        p.mstate = 0; p.mdata.clear();
    }
    void fail(Ph& p) { p.active = false; p.hi = 0; out->bad(); }
    void finish(Ph& p) { p.active = false; p.hi = 0; }
    Chain ch_;
    std::vector<cf32> y_;
    cf32 prev_{1, 0};
    uint64_t n_ = 0;
    float integ_ = 0, buf_[kSps] = {}, hist_[5] = {};
    Ph ph_[kSps];
    Ph* act_ = nullptr;
    int wait_ = 0, tn_ = 0;
    float terr_ = 0;
    uint64_t batches_ = 0, addresses_ = 0, messages_ = 0, repaired_ = 0, syncs_ = 0;
    std::string net_;
};

} // namespace

std::unique_ptr<Decoder> makeErmes() { return std::make_unique<ErmesDec>(); }

} // namespace dect2::avroad
