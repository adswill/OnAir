// SITOR-A / AMTOR ARQ monitor and test generator (see sitor_arq.h). Clean-room from ITU-R M.476-5 Annex 1.
#include "dect2/sitor/sitor_arq.h"
#include "dect2/marine/marine_navtex.h"
#include <algorithm>
#include <cmath>

namespace dect2::sitor {

namespace {
constexpr size_t kKeepText = 4000;
bool isService(uint8_t w) { return w == kArqAlpha || w == kArqBeta || w == kArqRq; }
int csIndex(uint8_t w) { return w == kCs1 ? 1 : w == kCs2 ? 2 : w == kCs3 ? 3 : 0; }
char letterOf(uint8_t w) {
    const int c = marine::ccir476Char(w, false);
    return c > 0 ? static_cast<char>(c) : '?';
}
}  // namespace

ArqMonitor::ArqMonitor(bool invert) : invert_(invert) {}

void ArqMonitor::reset() {
    const bool inv = invert_;
    auto ev = std::move(ev_);
    auto tx = std::move(txt_);
    *this = ArqMonitor(inv);
    ev_ = std::move(ev);
    txt_ = std::move(tx);
}

bool ArqMonitor::wordAt(uint64_t endBit, uint8_t& w) const {
    if (endBit + 1 < 7 || endBit >= n_ || n_ - endBit > 200) return false;
    unsigned v = 0;
    for (int i = 0; i < 7; i++) v |= static_cast<unsigned>(bits_[(endBit - 6 + static_cast<uint64_t>(i)) & 255]) << i;   // the oldest bit is the least significant
    w = static_cast<uint8_t>(v);
    return marine::ccir476Valid(v);
}

bool ArqMonitor::blockAt(uint64_t endBit, uint8_t w[3]) const {
    if (endBit + 1 < static_cast<uint64_t>(kBlockBits)) return false;
    return wordAt(endBit - 14, w[0]) && wordAt(endBit - 7, w[1]) && wordAt(endBit, w[2]);
}

void ArqMonitor::event(const std::string& s) { if (ev_) ev_(s); }

void ArqMonitor::unlock() {
    if (st_.locked) event("SITOR-A: link lost");
    st_.locked = false;
    phase_ = -1;
    missRun_ = 0;
    havePending_ = false;
    csPrev_ = csPrev2_ = 0;
    prevValid_ = false;
    callAValid_ = false;
    expectTurn_ = false;
}

void ArqMonitor::pushBit(float soft) {
    uint8_t b = soft > 0 ? 1 : 0;
    if (invert_) b ^= 1;
    bits_[n_ & 255] = b;
    n_++;
    const uint64_t e = n_ - 1;
    uint8_t w[3];
    const bool v = blockAt(e, w);
    const int ph = static_cast<int>(e % kCycleBits);
    hist_[ph] = (hist_[ph] << 1) | (v ? 1u : 0u);

    // the control signal of the block slot that ended 24 bits ago
    if (havePending_ && e >= pendingCsEnd_ + 24) { havePending_ = false; scanCs(pendingCsEnd_); }

    if (!st_.locked) {
        if (v && (hist_[ph] & 0xF) == 0xF) {      // three valid characters at the same place in four cycles running
            st_.locked = true;
            phase_ = ph;
            missRun_ = 0;
            csPrev_ = csPrev2_ = 0;
            prevValid_ = false;
            event("SITOR-A: link found (blocks every 450 ms)");
            // the three blocks that made the lock are read too (the call blocks, or the first text), with their control signals
            for (int j = 3; j >= 1; j--) {
                const uint64_t be = e - static_cast<uint64_t>(j * kCycleBits);
                uint8_t pw[3];
                if (blockAt(be, pw)) { onBlock(pw, be); havePending_ = false; scanCs(be); }
            }
            onBlock(w, e);
            nextEnd_ = e + kCycleBits;
        }
        return;
    }
    // a new sending station (change of direction): another phase starts to carry blocks while the locked one has gone quiet
    const int dph = std::abs(ph - phase_);
    // after the change-over was acknowledged the new sending station's first block is enough; otherwise two in a row while the old one is quiet
    const bool turn = expectTurn_ ? (hist_[ph] & 0x1) == 0x1 : (missRun_ >= 2 && (hist_[ph] & 0x3) == 0x3);
    if (v && turn && std::min(dph, kCycleBits - dph) > 2) {
        st_.changes++;
        event("SITOR-A: the other station is sending now");
        figs_ = false;   // the new sending station's shift is its own
        expectTurn_ = false;
        phase_ = ph;
        missRun_ = 0;
        csOff_ = -1;
        std::fill(std::begin(csHist_), std::end(csHist_), 0.f);
        csPrev_ = csPrev2_ = 0;
        prevValid_ = false;
        // its block of the cycle before, if that one was readable
        uint8_t pw[3];
        const uint64_t be = e - static_cast<uint64_t>(kCycleBits);
        if ((hist_[ph] & 0x2) && blockAt(be, pw)) { onBlock(pw, be); havePending_ = false; scanCs(be); }
        onBlock(w, e);
        nextEnd_ = e + kCycleBits;
        return;
    }
    if (e != nextEnd_ + 1) return;
    if (expectTurn_) {   // the old sending station now sends control signals only: what looks like a block in its slot is not one
        nextEnd_ += kCycleBits;
        if (++missRun_ >= 24) unlock();
        return;
    }
    // the expected block, or one bit either side (clock wander)
    uint8_t c[3];
    uint64_t got = 0;
    bool ok = false;
    for (uint64_t cand : {nextEnd_, nextEnd_ - 1, nextEnd_ + 1}) {
        if (blockAt(cand, c)) { ok = true; got = cand; break; }
    }
    if (ok) {
        missRun_ = 0;
        phase_ = static_cast<int>(got % kCycleBits);
        onBlock(c, got);
        nextEnd_ = got + kCycleBits;
    } else {
        missRun_++;
        onBadBlock();
        pendingCsEnd_ = nextEnd_;
        havePending_ = true;
        nextEnd_ += kCycleBits;
        if (missRun_ >= 24) unlock();
    }
}

void ArqMonitor::scanCs(uint64_t end) {
    // the IRS answers in the pause: one CS word somewhere in the 24 bits after the block
    for (auto& h : csHist_) h *= 0.8f;
    for (int o = 0; o <= 17; o++) {
        uint8_t w;
        if (wordAt(end + 7 + static_cast<uint64_t>(o), w) && csIndex(w)) csHist_[o] += 1.f;
    }
    int best = -1;
    float bv = 0, second = 0;
    for (int o = 0; o <= 17; o++) {
        if (csHist_[o] > bv) { second = bv; bv = csHist_[o]; best = o; } else if (csHist_[o] > second) second = csHist_[o];
    }
    csOff_ = (best >= 0 && bv >= 2.0f && bv >= 1.4f * second) ? best : -1;
    int cs = 0;
    if (csOff_ >= 0) {
        for (int d : {0}) {   // only where the control signal has been found: a neighbouring position can read as a false one
            const int o = csOff_ + d;
            if (o < 0 || o > 17) continue;
            uint8_t w;
            if (wordAt(end + 7 + static_cast<uint64_t>(o), w) && csIndex(w)) { cs = csIndex(w); break; }
        }
    }
    st_.csOffsetBits = csOff_;
    if (cs) st_.controlSignals++;
    if (cs == 3) { st_.changes++; event("SITOR-A: CS3, the receiving station asks to change direction"); }
    csPrev2_ = csPrev_;
    csPrev_ = cs;
}

void ArqMonitor::onBadBlock() {
    st_.bad++;
    prevValid_ = false;
}

void ArqMonitor::onBlock(const uint8_t w[3], uint64_t endBit) {
    st_.blocks++;
    pendingCsEnd_ = endBit;
    havePending_ = true;
    const bool rq = w[0] == kArqRq || w[1] == kArqRq || w[2] == kArqRq;
    if (rq) {
        // call blocks (M.476-5 3.1.4.3): RQ in the second place, then RQ in the third place; anything else with RQ is a repeat request
        if (w[1] == kArqRq && w[0] != kArqRq && w[2] != kArqRq) { std::copy(w, w + 3, callA_); callAValid_ = true; }
        else if (w[2] == kArqRq && w[0] != kArqRq && w[1] != kArqRq && callAValid_) {
            std::string call{letterOf(callA_[0]), letterOf(callA_[2]), letterOf(w[0]), letterOf(w[1])};
            if (call != selcall_) { selcall_ = call; event("SITOR-A: call to " + call); }
            callAValid_ = false;
        }
        csPrev_ = csPrev2_ = 0;       // block numbering is interrupted (3.1.3.1)
        prevValid_ = false;
        return;
    }
    callAValid_ = false;
    // a new block or the repeat of the last one: the ISS sends the next block only when the control signal changed
    // a repeat is the same three characters again; the control signals say whether it was asked for (a misread one must not drop a new block)
    const bool same = prevValid_ && std::equal(w, w + 3, prevBlock_);
    bool repeat;
    if (csPrev_ && csPrev2_ && csPrev_ != 3 && csPrev2_ != 3) repeat = csPrev_ == csPrev2_ && same;
    else repeat = same;
    std::copy(w, w + 3, prevBlock_);
    prevValid_ = true;
    if (repeat) { st_.repeats++; return; }
    st_.newBlocks++;
    emit(w);
}

void ArqMonitor::emit(const uint8_t w[3]) {
    if (w[0] == kArqAlpha && w[1] == kArqAlpha && w[2] == kArqAlpha) { event("SITOR-A: end of communication"); return; }
    if (w[0] == kArqBeta && w[1] == kArqAlpha && w[2] == kArqBeta) { event("SITOR-A: change-over acknowledged"); expectTurn_ = true; return; }
    std::string out;
    for (int i = 0; i < 3; i++) {
        const uint8_t x = w[i];
        if (isService(x)) continue;
        if (x == marine::kLtrs) { figs_ = false; continue; }
        if (x == marine::kFigs) { figs_ = true; continue; }
        const int c = marine::ccir476Char(x, figs_);
        if (c <= 0) continue;
        out.push_back(static_cast<char>(c));
        tail_.push_back(static_cast<char>(c));
        if (tail_.size() > 4) tail_.erase(0, tail_.size() - 4);
        if (figs_ && tail_.size() >= 2 && tail_.compare(tail_.size() - 2, 2, "+?") == 0) event("SITOR-A: \"+?\", the sending station hands over");
    }
    if (out.empty()) return;
    st_.chars += out.size();
    text_ += out;
    if (text_.size() > kKeepText) text_.erase(0, text_.size() - kKeepText);
    if (txt_) txt_(out);
}

// ---- generator ---------------------------------------------------------------------------------------------------------------------

std::vector<uint8_t> arqWords(const std::string& text) {
    std::vector<uint8_t> w = marine::navtexEncode(text);
    while (w.size() % 3) w.push_back(kArqBeta);
    return w;
}

namespace {
struct Burst { double startBit; std::vector<uint8_t> words; bool stationA; };
}

std::vector<cf32> genSitorArq(const ArqGenCfg& c, const std::string& text) {
    // the cycles of the link: station A (the caller, the master, near the listener) transmits at the start of every cycle; station B
    // answers irsDelayBits after the end of what it heard
    std::vector<Burst> bursts;
    double cyc = 0;   // cycle start in bits
    auto cycle = [&](std::vector<uint8_t> a, std::vector<uint8_t> b) {
        const double aLen = 7.0 * static_cast<double>(a.size());
        if (!a.empty()) bursts.push_back({cyc, a, true});
        if (!b.empty()) bursts.push_back({cyc + aLen + c.irsDelayBits, b, false});
        cyc += kCycleBits;
    };
    auto code = [](char ch) { uint8_t w = kArqBeta; marine::ccir476Code(ch, false, w); return w; };
    const double lead = c.leadSec * c.baud;
    cyc = lead;
    if (c.call && c.selcall.size() >= 4) {
        const std::vector<uint8_t> call1 = {code(c.selcall[0]), kArqRq, code(c.selcall[1])};
        const std::vector<uint8_t> call2 = {code(c.selcall[2]), code(c.selcall[3]), kArqRq};
        for (int k = 1; k < std::max(1, c.callPairs); k++) { cycle(call1, {}); cycle(call2, {}); }
        cycle(call1, {});
        cycle(call2, {kCs1});
        cycle(call1, {kCs1});     // two identical control signals: the caller becomes the ISS (3.1.4.5)
    }
    // sending: blocks numbered 1, 2, 1, ...; the IRS asks for "the other" block after a good one, the same after a mutilated one
    // when B sends, A's answer to a block goes at the start of the next cycle (A keys first in every cycle)
    std::vector<uint8_t> answerB = {kCs1};
    auto send = [&](const std::vector<uint8_t>& words, bool aSends, bool firstCs1) {
        int want = 1;          // the block number asked for
        int nBlocks = 0;
        (void)firstCs1;
        for (size_t i = 0; i < words.size();) {
            std::vector<uint8_t> blk(words.begin() + static_cast<long>(i), words.begin() + static_cast<long>(i) + 3);
            nBlocks++;
            const bool mutilate = c.repeatEvery > 0 && nBlocks % c.repeatEvery == 0;
            const int next = mutilate ? want : (want == 1 ? 2 : 1);
            const std::vector<uint8_t> cs = {next == 1 ? kCs1 : kCs2};
            if (aSends) cycle(blk, cs); else { cycle(answerB, blk); answerB = cs; }
            if (!mutilate) { i += 3; want = next; }
        }
    };
    std::vector<uint8_t> w1 = arqWords(c.changeOver ? text + "+?" : text);
    send(w1, true, true);
    if (c.changeOver) {
        cycle({kArqBeta, kArqBeta, kArqBeta}, {kCs3});           // the IRS asks for the change (3.1.6.2)
        cycle({kArqBeta, kArqAlpha, kArqBeta}, {});              // the ISS acknowledges
        cycle({kCs1}, {kArqRq, kArqRq, kArqRq});                 // B is the ISS now: a block of three RQ (slave)
        send(arqWords(c.text2), false, true);
        if (c.endOfComm) cycle(answerB, {kArqAlpha, kArqAlpha, kArqAlpha});
    } else if (c.endOfComm) {
        cycle({kArqAlpha, kArqAlpha, kArqAlpha}, {kCs1});
    }
    const double totalBits = cyc + c.tailSec * c.baud;
    const double spb = c.rate / c.baud;
    std::vector<cf32> out(static_cast<size_t>(totalBits * spb) + 1, cf32(0, 0));
    double phA = 0, phB = 0;
    for (const Burst& b : bursts) {
        const std::vector<uint8_t> bits = marine::codesToBits(b.words);
        const double amp = b.stationA ? 1.0 : c.irsLevel;
        const double off = b.stationA ? 0.0 : c.irsOffsetHz;
        double& ph = b.stationA ? phA : phB;
        const double s0 = b.startBit * spb, len = static_cast<double>(bits.size()) * spb;
        const size_t i0 = static_cast<size_t>(std::ceil(s0)), i1 = std::min(out.size(), static_cast<size_t>(s0 + len));
        const double ramp = 0.001 * c.rate;   // 1 ms raised-cosine edges
        for (size_t i = i0; i < i1; i++) {
            const double t = static_cast<double>(i) / c.rate;
            const size_t k = std::min(bits.size() - 1, static_cast<size_t>((static_cast<double>(i) - s0) / spb));
            const double f = c.centreHz + off + c.driftHzPerSec * t + (bits[k] ? 0.5 : -0.5) * c.shiftHz;   // B = 1 = the higher tone
            ph += 2 * M_PI * f / c.rate;
            if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            double g = 1;
            const double a = static_cast<double>(i) - s0, z = s0 + len - static_cast<double>(i);
            if (a < ramp) g = 0.5 - 0.5 * std::cos(M_PI * a / ramp);
            if (z < ramp) g = std::min(g, 0.5 - 0.5 * std::cos(M_PI * z / ramp));
            out[i] += cf32(static_cast<float>(amp * g * std::cos(ph)), static_cast<float>(amp * g * std::sin(ph)));
        }
    }
    return out;
}

}  // namespace dect2::sitor
