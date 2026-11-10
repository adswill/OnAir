// Demodulation chains of the Smart CubeSat decoder: see cubesat_chain.h.
// The FSK / AFSK slicers and the HDLC layer are the ones of the APRS and CubeSat receivers (packet_demod.h, packet_ax25.h); the AX100 / CCSDS
// framings are cubesat_proto.h; the AO-40 FEC is cubesat_ao40.h.
#include "dect2/cubesat/cubesat_chain.h"
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"
#include "dect2/cubesat/cubesat_ao40.h"
#include "dect2/cubesat/cubesat_db.h"
#include "dect2/cubesat/cubesat_more.h"
#include "dect2/cubesat/cubesat_telem.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_demod.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace cubesat {

namespace {

using dect2::math::kPi;
constexpr double kFs = kChanRate;

int popc(uint32_t v) { return dect2::bits::popcount(v); }

// ---- the ASM hunter: the AX100 (0x930B51DE) and CCSDS (0x1ACFFC1D) sync words in a bit stream, either polarity, then the frame
struct AsmHunter {
    enum St { Hunt, Golay, Body };
    St st = Hunt;
    uint32_t sh = 0;
    int nb = 0;
    bool inv = false, ccsds = false;
    uint32_t acc = 0; int accBits = 0;
    int flags = 0;
    size_t need = 0;
    std::vector<uint8_t> body;
    int ccsdsLen = 255;
    std::function<void(bool ccsds, std::vector<uint8_t>& body, int flags, bool golayFailed)> done;
    void reset() { st = Hunt; sh = 0; nb = 0; acc = 0; accBits = 0; body.clear(); }
    bool inFrame() const { return st != Hunt; }
    void bit(int b) {
        if (st == Hunt) {
            sh = (sh << 1) | (uint32_t)(b & 1);
            if (++nb < 32) return;
            for (int k = 0; k < 2; k++) {
                const uint32_t a = k ? kAsmCcsds : kAsmAx100;
                const int e = popc(sh ^ a), ei = popc(~sh ^ a);
                if (e <= 2 || ei <= 2) {
                    inv = ei < e; ccsds = k == 1;
                    acc = 0; accBits = 0; body.clear();
                    if (ccsds) { st = Body; need = (size_t)ccsdsLen; flags = 0; } else st = Golay;
                    return;
                }
            }
            return;
        }
        const int v = (b ^ (inv ? 1 : 0)) & 1;
        acc = (acc << 1) | (uint32_t)v;
        accBits++;
        if (st == Golay) {
            if (accBits < 24) return;
            const int d = golayDecode(acc & 0xFFFFFF);
            acc = 0; accBits = 0;
            if (d < 0 || (d & 0xFF) < 5) { std::vector<uint8_t> none; if (done) done(false, none, 0, true); st = Hunt; nb = 0; return; }
            flags = d & 0xF00; need = (size_t)(d & 0xFF);
            st = Body;
            return;
        }
        if (accBits < 8) return;
        body.push_back((uint8_t)acc);
        acc = 0; accBits = 0;
        if (body.size() >= need) {
            if (done) done(ccsds, body, flags, false);
            st = Hunt; nb = 0; sh = 0;
        }
    }
};

// ---- ASCII beacons in a bit stream: every 256 new bits the last 1024 are read as bytes in all 8 alignments, both bit orders and polarities
struct TextHunter {
    std::vector<uint8_t> bits;
    size_t sinceCheck = 0;
    std::string last;
    std::function<void(const std::string&)> found;
    void reset() { bits.clear(); sinceCheck = 0; last.clear(); }
    void bit(int b) {
        bits.push_back((uint8_t)(b & 1));
        if (++sinceCheck < 256 || bits.size() < 1024) return;
        sinceCheck = 0;
        const size_t n = bits.size();
        const uint8_t* w = bits.data() + (n - 1024);
        for (int inv = 0; inv < 2; inv++)
            for (int lsb = 0; lsb < 2; lsb++)
                for (int al = 0; al < 8; al++) {
                    std::vector<uint8_t> by;
                    for (size_t i = (size_t)al; i + 8 <= 1024; i += 8) {
                        unsigned v = 0;
                        for (int k = 0; k < 8; k++) {
                            const unsigned bt = (unsigned)(w[i + (size_t)k] ^ (uint8_t)inv);
                            v = lsb ? (v | (bt << k)) : ((v << 1) | bt);
                        }
                        by.push_back((uint8_t)v);
                    }
                    const std::string t = beaconText(by);
                    if (t.size() >= 24 && t != last) { last = t; if (found) found(t); return; }
                }
        if (bits.size() > 4096) bits.erase(bits.begin(), bits.begin() + (long)(bits.size() - 1024));
    }
};

// ---- shared frame helpers
ChainFrame ax25Frame(const std::vector<uint8_t>& d, const ax25::Frame& a, const char* name, int framing, int mod, int baud, int64_t pos) {
    ChainFrame f;
    f.framing = framing; f.framingName = name; f.mod = mod; f.baud = baud; f.how = "AX.25 FCS"; f.pos = pos;
    f.from = a.from.str(); f.to = a.to.str(); f.path = a.pathStr(); f.type = a.typeName(); f.pid = a.pid;
    f.data = d;
    f.payload.assign(a.info.begin(), a.info.end());
    return f;
}

// HDLC result -> frame (AX.25, or, for a scrambled stream, AX100 Mode 6)
struct HdlcOut {
    int mod = 0, baud = 0;
    bool g3 = false;
    uint64_t heard = 0, valid = 0, heard6 = 0, valid6 = 0;
    std::function<void(ChainFrame&)>* sink = nullptr;
    void onHdlc(const std::vector<uint8_t>& d, bool ok, int64_t pos, const char* name, int framing) {
        if (ok) {
            ax25::Frame a;
            if (!ax25::parseFrame(d.data(), d.size(), a)) { heard++; return; }
            valid++;
            if (sink && *sink) { ChainFrame f = ax25Frame(d, a, name, framing, mod, baud, pos); (*sink)(f); }
            return;
        }
        if (g3) {
            std::vector<uint8_t> csp;
            int rsErr = -1;
            if (ax100Mode6Decode(d, csp, rsErr)) {
                valid6++;
                if (sink && *sink) {
                    ChainFrame f;
                    f.framing = Ax100Mode6; f.framingName = "AX100 Mode 6"; f.mod = mod; f.baud = baud; f.rsErrors = rsErr; f.how = "Reed-Solomon"; f.pos = pos;
                    f.hasCsp = parseCsp(csp, f.csp);
                    f.data = csp;
                    if (csp.size() > 4) f.payload.assign(csp.begin() + 4, csp.end());
                    (*sink)(f);
                }
                return;
            }
        }
        if (d.size() >= 20) heard++;
    }
};

void asmFrame(bool cc, std::vector<uint8_t>& body, int flags, bool golayFailed, int mod, int baud, int64_t pos, uint64_t& heard, uint64_t& validM5, uint64_t& validCc,
              const std::function<void(ChainFrame&)>& sink) {
    if (golayFailed) { heard++; return; }
    ChainFrame f;
    f.mod = mod; f.baud = baud; f.pos = pos;
    int rsErr = -1;
    if (cc) {
        std::vector<uint8_t> data;
        if (!ccsdsDecode(body.data(), body.size(), data, rsErr)) { heard++; return; }
        f.framing = CcsdsRs; f.framingName = "CCSDS Reed-Solomon"; f.rsErrors = rsErr; f.how = "Reed-Solomon"; f.data = data; f.payload = data;
        validCc++;
    } else {
        std::vector<uint8_t> csp;
        if (!ax100Mode5Decode(body.data(), body.size(), flags, csp, rsErr)) { heard++; return; }
        f.framing = Ax100Mode5; f.framingName = "AX100 Mode 5"; f.rsErrors = (flags & kAx100Rs) ? rsErr : -1; f.how = (flags & kAx100Rs) ? "Golay + Reed-Solomon" : "Golay";
        f.hasCsp = parseCsp(csp, f.csp);
        f.data = csp;
        if (csp.size() > 4) f.payload.assign(csp.begin() + 4, csp.end());
        validM5++;
    }
    if (sink) sink(f);
}

// the cubesat_more.h framings (U482C, CCSDS concatenated, USP, NGHam, Mobitex) as chain frames
void moreFrame(const MoreFrame& m, int mod, int baud, int64_t pos, uint64_t& valid, const std::function<void(ChainFrame&)>& sink) {
    valid++;
    if (!sink) return;
    ChainFrame f;
    f.framing = m.framing; f.framingName = m.name; f.mod = mod; f.baud = baud; f.rsErrors = m.rsErrors; f.how = m.how; f.pos = pos;
    f.data = m.data; f.payload = m.data;
    if (m.framing == Usp) {
        ax25::Frame a;
        if (ax25::parseFrame(m.data.data(), m.data.size(), a)) {
            f.from = a.from.str(); f.to = a.to.str(); f.path = a.pathStr(); f.type = a.typeName(); f.pid = a.pid;
            f.payload.assign(a.info.begin(), a.info.end());
        }
    } else if (m.framing == U482c) {
        f.hasCsp = parseCsp(m.data, f.csp);
        if (m.data.size() > 4) f.payload.assign(m.data.begin() + 4, m.data.end());
    } else if (m.framing == Mobitex) {
        f.from = m.callsign;
    }
    sink(f);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// FM discriminator (Hz) of the channel stream
struct Disc {
    cf32 prev{1.f, 0.f};
    float clipHz = 12000.f;
    void run(const cf32* y, size_t n, std::vector<float>& out) {
        out.resize(n);
        const float k = (float)(kFs / (2 * kPi));
        for (size_t i = 0; i < n; i++) {
            const cf32 p = y[i] * std::conj(prev);
            prev = y[i];
            out[i] = clampf(std::atan2(p.imag(), p.real()) * k, -clipHz, clipHz);
        }
    }
    void reset() { prev = cf32(1.f, 0.f); }
};

// ======================================================= FSK
class FskChain : public Chain {
public:
    explicit FskChain(int baud) : baud_(baud) {
        disc_.clipHz = std::max(8000.f, (float)baud * 1.5f);
        const double pass = 0.58 * baud, stop = std::min(1.25 * baud, 46000.0);
        lp_.design(dmr::lowpassTaps(pass, std::max(stop, pass + 200.0), kFs, 40, 9), 1);
        g3_.baud = baud; g3_.descramble = true; g3_.init(0.70f, 0.80f);
        plainH_.baud = baud; plainH_.descramble = false; plainH_.init(0.70f, 0.80f);
        plainA_.baud = baud; plainA_.descramble = false; plainA_.init(0.70f, 0.85f);
        outG3_.mod = outPl_.mod = ModFsk; outG3_.baud = outPl_.baud = baud; outG3_.g3 = true;
        outG3_.sink = outPl_.sink = &sink;
        g3_.hdlc.setCallback([this](const std::vector<uint8_t>& d, bool ok) { outG3_.onHdlc(d, ok, g3_.pos, "AX.25 G3RUH", Ax25G3ruh); });
        plainH_.hdlc.setCallback([this](const std::vector<uint8_t>& d, bool ok) { outPl_.onHdlc(d, ok, plainH_.pos, "AX.25 NRZI", Ax25G3ruh); });
        plainA_.bitSink = [this](int b) { asm_.bit(b); text_.bit(b); more_.push(b ? 1.f : -1.f); plainA_.locked = asm_.inFrame() || more_.busy(); };
        more_.onFrame = [this](const MoreFrame& m) { moreFrame(m, ModFsk, baud_, plainA_.pos, moreOk_, sink); };
        asm_.done = [this](bool cc, std::vector<uint8_t>& body, int flags, bool gf) { asmFrame(cc, body, flags, gf, ModFsk, baud_, plainA_.pos, asmHeard_, m5_, cc_, sink); };
        text_.found = [this](const std::string& t) {
            texts_++;
            if (!sink) return;
            ChainFrame f; f.framing = kFramingText; f.framingName = "ASCII text"; f.mod = ModFsk; f.baud = baud_; f.how = "printable text run"; f.text = t;
            f.payload.assign(t.begin(), t.end()); f.data = f.payload; f.pos = plainA_.pos;
            sink(f);
        };
    }
    int mod() const override { return ModFsk; }
    int baud() const override { return baud_; }
    void reset() override {
        disc_.reset(); lp_.reset(); g3_.reset(); plainH_.reset(); plainA_.reset(); asm_.reset(); text_.reset(); more_.reset();
    }
    void process(const cf32* y, size_t n) override {
        disc_.run(y, n, d_);
        f_.clear();
        lp_.process(d_.data(), d_.size(), f_);
        if (f_.empty()) return;
        g3_.process(f_.data(), f_.size());
        plainH_.process(f_.data(), f_.size());
        plainA_.process(f_.data(), f_.size());
    }
    void stats(std::vector<TrialStats>& out) const override {
        const std::string b = "FSK " + std::to_string(baud_) + " baud, ";
        out.push_back({b + "AX.25 G3RUH", ModFsk, baud_, outG3_.heard, outG3_.valid});
        out.push_back({b + "AX100 Mode 6", ModFsk, baud_, outG3_.heard6, outG3_.valid6});
        out.push_back({b + "AX.25 NRZI", ModFsk, baud_, outPl_.heard, outPl_.valid});
        out.push_back({b + "AX100 Mode 5", ModFsk, baud_, asmHeard_, m5_});
        out.push_back({b + "CCSDS RS", ModFsk, baud_, 0, cc_});
        out.push_back({b + "ASCII text", ModFsk, baud_, 0, texts_});
        out.push_back({b + "U482C / CCSDS conc. / USP / NGHam / Mobitex", ModFsk, baud_, more_.failed, moreOk_});
    }
private:
    int baud_;
    Disc disc_;
    dmr::Fir<float> lp_;
    std::vector<float> d_, f_;
    pktdemod::FskSlicer g3_, plainH_, plainA_;
    HdlcOut outG3_, outPl_;
    AsmHunter asm_;
    TextHunter text_;
    MoreHunter more_;
    uint64_t asmHeard_ = 0, m5_ = 0, cc_ = 0, texts_ = 0, moreOk_ = 0;
};

// ======================================================= AFSK 1200
class AfskChain : public Chain {
public:
    AfskChain() {
        a48_.design(dmr::lowpassTaps(3300.0, 8000.0, kFs, 50, 9), 2);
        afsk_[0].init(40, false, 1.0f, 0.70f, 0.80f);
        afsk_[1].init(40, true, 0.7f, 0.75f, 0.85f);
        for (int i = 0; i < 2; i++) {
            out_[i].mod = ModAfsk; out_[i].baud = 1200; out_[i].sink = &sink;
            afsk_[i].hdlc.setCallback([this, i](const std::vector<uint8_t>& d, bool ok) { out_[i].onHdlc(d, ok, afsk_[i].pos * 2, "AX.25 AFSK 1200", Ax25Afsk1200); });
        }
    }
    int mod() const override { return ModAfsk; }
    int baud() const override { return 1200; }
    void reset() override { disc_.reset(); a48_.reset(); for (auto& a : afsk_) a.reset(); }
    void process(const cf32* y, size_t n) override {
        disc_.run(y, n, d_);
        a_.clear();
        a48_.process(d_.data(), d_.size(), a_);
        if (a_.empty()) return;
        // the tones are at +-500 Hz of deviation: bring the audio to a level the correlators like
        for (auto& v : a_) v *= (1.f / 3000.f);
        for (auto& a : afsk_) a.process(a_.data(), a_.size());
    }
    void stats(std::vector<TrialStats>& out) const override {
        out.push_back({"AFSK 1200 baud, AX.25", ModAfsk, 1200, std::max(out_[0].heard, out_[1].heard), std::max(out_[0].valid, out_[1].valid)});
    }
private:
    Disc disc_;
    dmr::Fir<float> a48_;
    std::vector<float> d_, a_;
    pktdemod::Afsk1200 afsk_[2];
    HdlcOut out_[2];
};

// ======================================================= BPSK
class BpskChain : public Chain {
public:
    explicit BpskChain(int baud) : baud_(baud) {
        // decimate the 96 kHz stream to 4 .. 8 samples per symbol
        D_ = std::max(1, (int)std::floor(kFs / (baud * 4.0)));
        fsd_ = kFs / D_;
        sps_ = fsd_ / baud;
        spsNom_ = sps_;
        const double pass = 0.75 * baud, stop = std::max(pass + 0.1 * baud, std::min(fsd_ - pass, 1.9 * baud));
        lp_.design(dmr::lowpassTaps(pass, std::min(stop, fsd_ * 0.49), kFs, 45, 9), D_);
        const double th = 0.02, z = 0.707, d = 1.0 + 2 * z * th + th * th;
        alpha_ = (float)(4 * z * th / d); beta_ = (float)(4 * th * th / d);
        kAfc = fsd_ >= 40000 ? 2048 : 4096;     // 43 ms at 9600 baud, 85 ms at 1200
        sq_.reserve((size_t)kAfc);
        fft_ = std::make_unique<Fft>(kAfc);
        hdlcN_.setCallback([this](const std::vector<uint8_t>& d, bool ok) { outN_.onHdlc(d, ok, pos_, "AX.25 NRZI (BPSK)", Ax25G3ruh); });
        hdlcG_.setCallback([this](const std::vector<uint8_t>& d, bool ok) { outG_.onHdlc(d, ok, pos_, "AX.25 G3RUH (BPSK)", Ax25G3ruh); });
        outN_.mod = outG_.mod = ModBpsk; outN_.baud = outG_.baud = baud; outG_.g3 = true;
        outN_.sink = outG_.sink = &sink;
        asm_.done = [this](bool cc, std::vector<uint8_t>& body, int flags, bool gf) { asmFrame(cc, body, flags, gf, ModBpsk, baud_, pos_, asmHeard_, m5_, cc_, sink); };
        text_.found = [this](const std::string& t) {
            texts_++;
            if (!sink) return;
            ChainFrame f; f.framing = kFramingText; f.framingName = "ASCII text"; f.mod = ModBpsk; f.baud = baud_; f.how = "printable text run"; f.text = t;
            f.payload.assign(t.begin(), t.end()); f.data = f.payload; f.pos = pos_;
            sink(f);
        };
        ao40_.onFrame = [this](const std::vector<uint8_t>& fr, int rs) {
            ao40Ok_++;
            if (!sink) return;
            ChainFrame f; f.framing = kFramingAo40; f.framingName = "AO-40 FEC"; f.mod = ModBpsk; f.baud = baud_; f.rsErrors = rs; f.how = "Viterbi + Reed-Solomon";
            f.data = fr; f.payload = fr; f.pos = pos_;
            sink(f);
        };
        ao40_.reset();
        more_.onFrame = [this](const MoreFrame& m) { moreFrame(m, ModBpsk, baud_, pos_, moreOk_, sink); };
        {   // BPSK CCSDS concatenated is usually NRZ-M precoded (LilacSat-2, BY70-1, ASRTU-1): a second hunter for that, conventional basis
            MoreConfig c; c.u482c = c.ngham = c.mobitex = c.usp = false; c.concatParams.differential = true; c.concatParams.dualBasis = false;
            moreDiff_.configure(c);
            moreDiff_.onFrame = more_.onFrame;
        }
        next_ = sps_ * 2;
    }
    int mod() const override { return ModBpsk; }
    int baud() const override { return baud_; }
    double snrDb() const override { return locked_ ? lockQ_ : 0; }
    void reset() override {
        lp_.reset(); sq_.clear(); afc_ = 0; haveAfc_ = false; ph_ = 0; freq_ = 0; agc_ = 0.f; hist_.clear(); base_ = 0; next_ = sps_ * 2; sps_ = spsNom_;
        prevS_ = 0; prevLev_ = 0; scr_ = 0; hdlcN_.reset(); hdlcG_.reset(); asm_.reset(); text_.reset(); ao40_.reset(); more_.reset(); moreDiff_.reset(); locked_ = false;
    }
    void process(const cf32* y, size_t n) override {
        dec_.clear();
        lp_.process(y, n, dec_);
        for (const cf32& x : dec_) one(x);
        pos_ += (int64_t)n;
        gardner();
    }
    void stats(std::vector<TrialStats>& out) const override {
        const std::string b = "BPSK " + std::to_string(baud_) + " baud, ";
        out.push_back({b + "AX.25 NRZI", ModBpsk, baud_, outN_.heard, outN_.valid});
        out.push_back({b + "AX.25 G3RUH", ModBpsk, baud_, outG_.heard, outG_.valid});
        out.push_back({b + "AX100 Mode 5", ModBpsk, baud_, asmHeard_, m5_});
        out.push_back({b + "CCSDS RS", ModBpsk, baud_, 0, cc_});
        out.push_back({b + "AO-40 FEC", ModBpsk, baud_, ao40_.failed, ao40Ok_});
        out.push_back({b + "ASCII text", ModBpsk, baud_, 0, texts_});
        out.push_back({b + "U482C / CCSDS conc. / USP / NGHam / Mobitex", ModBpsk, baud_, more_.failed + moreDiff_.failed, moreOk_});
    }
private:
    int kAfc = 4096;
    void one(const cf32& x) {
        // coarse carrier: the square of a BPSK signal is a line at twice the offset
        sq_.push_back(x * x);
        if ((int)sq_.size() == kAfc) afcUpdate();
        const float c = std::cos((float)ph_), s = std::sin((float)ph_);
        const cf32 z = x * cf32(c, -s);
        const float mag = std::abs(z);
        agc_ += (agc_ == 0.f ? 1.f : 0.002f) * (mag - agc_);
        const float err = z.real() * z.imag() / (mag * mag + 1e-12f);
        freq_ += beta_ * err;
        ph_ += afc_ + freq_ + alpha_ * err;
        if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
        hist_.push_back(agc_ > 1e-9f ? z.real() / agc_ : 0.f);
        // lock quality: the energy in I against Q after rotation
        qi_ += 0.001f * (z.real() * z.real() - qi_);
        qq_ += 0.001f * (z.imag() * z.imag() - qq_);
        lockQ_ = 10.f * std::log10((qi_ + 1e-12f) / (qq_ + 1e-12f));
        locked_ = lockQ_ > 6.f;
    }
    void afcUpdate() {
        std::vector<cf32> v(sq_);
        sq_.clear();
        // Hann window
        for (int i = 0; i < kAfc; i++) v[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kAfc));
        fft_->forward(v.data());
        std::vector<float> m((size_t)kAfc);
        float best = 0; int bk = 0;
        for (int k = 0; k < kAfc; k++) { m[(size_t)k] = std::norm(v[(size_t)k]); if (m[(size_t)k] > best) { best = m[(size_t)k]; bk = k; } }
        std::vector<float> sorted(m);
        std::nth_element(sorted.begin(), sorted.begin() + kAfc / 2, sorted.end());
        const float med = sorted[(size_t)kAfc / 2] + 1e-20f;
        if (best < 30.f * med) return;                       // no line: not BPSK, or too weak
        // parabolic refinement
        const float a = std::log(m[(size_t)((bk + kAfc - 1) % kAfc)] + 1e-20f), b = std::log(best + 1e-20f), c = std::log(m[(size_t)((bk + 1) % kAfc)] + 1e-20f);
        const float den = a - 2 * b + c;
        const float off = den < 0 ? clampf(0.5f * (a - c) / den, -0.5f, 0.5f) : 0.f;
        double cyc = ((double)bk + off) / kAfc;
        if (cyc >= 0.5) cyc -= 1.0;
        const double f = kPi * cyc;                           // half of 2 pi * cyc (radians per sample)
        if (!haveAfc_) { afc_ = (float)f; haveAfc_ = true; freq_ = 0; }
        else afc_ += 0.5f * ((float)f - afc_);
    }
    float interp(double t) const {
        const double r = t - (double)base_;
        const long i = (long)std::floor(r);
        if (i < 0 || i + 1 >= (long)hist_.size()) return 0.f;
        const float fr = (float)(r - (double)i);
        return hist_[(size_t)i] * (1.f - fr) + hist_[(size_t)i + 1] * fr;
    }
    void gardner() {
        while (next_ + 1.0 < (double)base_ + (double)hist_.size() && next_ - sps_ / 2 > (double)base_) {
            const double t = next_;
            const float s = interp(t), m = interp(t - sps_ / 2);
            const float e = clampf(m * (s - prevS_), -1.f, 1.f);
            // a small proportional step and a slower correction of the symbol period
            next_ = t + sps_ - 0.03 * e * sps_;
            sps_ = clampf((float)(sps_ - 0.0003 * e), (float)(spsNom_ * 0.99), (float)(spsNom_ * 1.01));
            symbol(s);
            prevS_ = s;
        }
        const long drop = (long)std::floor(next_ - sps_ * 3) - base_;
        if (drop > 0 && (size_t)drop < hist_.size()) { hist_.erase(hist_.begin(), hist_.begin() + drop); base_ += drop; }
    }
    void symbol(float s) {
        const int lev = s > 0 ? 1 : 0;
        hdlcN_.bit(lev);
        const int d = lev ^ (int)((scr_ >> 11) & 1u) ^ (int)((scr_ >> 16) & 1u);
        scr_ = ((scr_ << 1) | (uint32_t)lev) & 0x1FFFFu;
        hdlcG_.bit(d);
        const int db = lev ^ prevLev_;
        prevLev_ = lev;
        asm_.bit(db);
        text_.bit(db);
        ao40_.push(s * prevSoft_);
        more_.push(s);
        moreDiff_.push(s);
        prevSoft_ = s;
    }

    int baud_, D_ = 1;
    double fsd_ = 0, sps_ = 0, spsNom_ = 0;
    dmr::Fir<cf32> lp_;
    std::vector<cf32> dec_, sq_;
    std::unique_ptr<Fft> fft_;
    float afc_ = 0;
    bool haveAfc_ = false;
    double ph_ = 0;
    float freq_ = 0, alpha_ = 0, beta_ = 0, agc_ = 0.f, qi_ = 0.5f, qq_ = 0.5f, lockQ_ = 0;
    bool locked_ = false;
    std::vector<float> hist_;
    long base_ = 0;
    double next_ = 0;
    float prevS_ = 0, prevSoft_ = 0;
    int prevLev_ = 0;
    uint32_t scr_ = 0;
    int64_t pos_ = 0;
    ax25::HdlcRx hdlcN_, hdlcG_;
    HdlcOut outN_, outG_;
    AsmHunter asm_;
    TextHunter text_;
    Ao40Hunter ao40_;
    MoreHunter more_, moreDiff_;
    uint64_t asmHeard_ = 0, m5_ = 0, cc_ = 0, texts_ = 0, ao40Ok_ = 0, moreOk_ = 0;
};

// ======================================================= CW
const char* const kMorse[] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.", ".-.-.-", "--..--", "..--..", "-..-.", "-...-", "-....-", ".-.-.", "---...", ".--.-."};
const char kMorseChar[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?/=-+:@";

class CwChain : public Chain {
public:
    CwChain() {
        lp_.design(dmr::lowpassTaps(900.0, 1000.0, kFs, 50, 9), 48);        // 2 kHz
        fft_ = std::make_unique<Fft>(1024);
    }
    int mod() const override { return ModCw; }
    int baud() const override { return 0; }
    double snrDb() const override { return snr_; }
    void reset() override { lp_.reset(); buf_.clear(); runs_.clear(); ph_ = 0; f0_ = 0; env_ = 0; on_ = false; runLen_ = 0; pk_ = 0; nf_ = 0; state_ = 0; warm_ = 0; }
    void process(const cf32* y, size_t n) override {
        dec_.clear();
        lp_.process(y, n, dec_);
        for (const cf32& x : dec_) one(x);
        pos_ += (int64_t)n;
    }
    void stats(std::vector<TrialStats>& out) const override { out.push_back({"CW / OOK beacon, Morse", ModCw, 0, heard_, valid_}); }
private:
    static constexpr double kEnvRate = 2000.0;
    void one(const cf32& x) {
        // find the tone (the carrier is within a few hundred Hz of the channel centre) every 0.5 s, mix it to DC
        buf_.push_back(x);
        if (buf_.size() == 1024) {
            std::vector<cf32> v(buf_);
            buf_.clear();
            for (int i = 0; i < 1024; i++) v[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(2 * kPi * i / 1024));
            fft_->forward(v.data());
            float best = 0; int bk = 0;
            std::vector<float> m(1024);
            for (int k = 0; k < 1024; k++) { m[(size_t)k] = std::norm(v[(size_t)k]); if (m[(size_t)k] > best) { best = m[(size_t)k]; bk = k; } }
            std::vector<float> s(m);
            std::nth_element(s.begin(), s.begin() + 512, s.end());
            if (best > 60.f * (s[512] + 1e-20f)) {
                double cyc = (double)bk / 1024.0;
                if (cyc >= 0.5) cyc -= 1.0;
                const double f = cyc * kEnvRate;
                f0_ = f0_ == 0 ? f : f0_ + 0.7 * (f - f0_);
            }
        }
        ph_ -= 2 * kPi * f0_ / kEnvRate;
        if (ph_ < -kPi) ph_ += 2 * kPi;
        const cf32 z = x * cf32((float)std::cos(ph_), (float)std::sin(ph_));
        zr_ += 0.2f * (z.real() - zr_);
        zi_ += 0.2f * (z.imag() - zi_);
        const float e = std::sqrt(zr_ * zr_ + zi_ * zi_);
        env_ = e;
        if (e > pk_) pk_ += 0.1f * (e - pk_); else pk_ -= 0.00008f * (pk_ - e);
        if (e < nf_ || nf_ == 0) nf_ += 0.05f * (e - nf_); else nf_ += 0.00003f * (e - nf_);
        const float thr = nf_ + 0.5f * (pk_ - nf_);
        snr_ = 20.0 * std::log10((pk_ + 1e-12) / (nf_ + 1e-12));
        if (warm_ < 500) warm_++;
        const bool keyed = warm_ >= 500 && pk_ > 6.f * nf_ && snr_ > 15;
        if (!keyed) {
            if (!runs_.empty() && ++idle_ > (int)(3 * kEnvRate)) flush();
            if (runs_.empty()) { on_ = false; runLen_ = 0; }
            return;
        }
        idle_ = 0;
        const bool on = on_ ? e > nf_ + 0.35f * (pk_ - nf_) : e > thr;
        if (on == on_) { runLen_++; }
        else {
            if (runLen_ > 0 && (!runs_.empty() || on_)) runs_.emplace_back(on_, runLen_ / kEnvRate);
            on_ = on; runLen_ = 1;
        }
        // a long key-up gap ends the message
        if (!on_ && runLen_ > (int)(2.5 * kEnvRate) && !runs_.empty()) flush();
        if (runs_.size() > 300) flush();
    }
    void flush() {
        if (!runLen_ && runs_.empty()) return;
        std::vector<std::pair<bool, double>> r;
        r.swap(runs_);
        runLen_ = 0; on_ = false; idle_ = 0;
        if (r.size() < 8) return;
        double unit = 0;
        if (getenv("DECT2_CW_DEBUG")) { for (auto& q : r) fprintf(stderr, "%c%.3f ", q.first ? '#' : '_', q.second); fprintf(stderr, "\n"); }
        const std::string t = morseDecode(r, &unit);
        if (t.empty()) { heard_++; return; }
        valid_++;
        if (!sink) return;
        ChainFrame f; f.framing = kFramingCw; f.framingName = "CW beacon"; f.mod = ModCw; f.baud = unit > 0 ? (int)std::lround(1.2 / unit * 5) : 0;   // words per minute x 5
        f.how = "Morse structure"; f.text = t; f.payload.assign(t.begin(), t.end()); f.data = f.payload; f.pos = pos_;
        sink(f);
    }

    dmr::Fir<cf32> lp_;
    std::vector<cf32> dec_, buf_;
    std::unique_ptr<Fft> fft_;
    std::vector<std::pair<bool, double>> runs_;
    double ph_ = 0, f0_ = 0, snr_ = 0;
    float zr_ = 0, zi_ = 0, env_ = 0, pk_ = 0, nf_ = 0;
    bool on_ = false;
    int runLen_ = 0, idle_ = 0, state_ = 0, warm_ = 0;
    int64_t pos_ = 0;
    uint64_t heard_ = 0, valid_ = 0;
};

} // namespace

std::string morseDecode(const std::vector<std::pair<bool, double>>& runsIn, double* unitOut) {
    // an unkeyed carrier in front of the message is no dash: leading and trailing key-down runs of more than 1.5 s are dropped
    std::vector<std::pair<bool, double>> runs;
    for (const auto& r : runsIn) {          // key-down glitches under 15 ms are noise: they join the key-up time around them
        const bool on = r.first && r.second >= 0.015;
        if (!runs.empty() && runs.back().first == on) runs.back().second += r.second; else runs.emplace_back(on, r.second);
    }
    while (!runs.empty() && !runs.front().first) runs.erase(runs.begin());
    while (!runs.empty() && runs.front().first && runs.front().second > 1.0) runs.erase(runs.begin());
    while (!runs.empty() && runs.back().first && runs.back().second > 1.0) runs.pop_back();
    std::vector<double> on;
    for (const auto& r : runs) if (r.first) on.push_back(r.second);
    if (on.size() < 4) return "";
    std::vector<double> s(on);
    std::sort(s.begin(), s.end());
    // the dot length: the median of the shorter cluster (split where the sorted durations jump by more than 1.7x)
    size_t split = s.size();
    for (size_t i = 1; i < s.size(); i++) if (s[i] > 1.9 * s[i - 1] && s[i] > 2.0 * s[0]) { split = i; break; }
    std::vector<double> dots(s.begin(), s.begin() + (long)std::min(split, s.size()));
    if (split == s.size()) {
        // one cluster only: all dots, or all dashes (the key-up gaps then tell)
        std::vector<double> off;
        for (const auto& r : runs) if (!r.first) off.push_back(r.second);
        if (off.empty()) return "";
        std::sort(off.begin(), off.end());
        const double u = off[off.size() / 4];
        dots.assign(1, u);
        if (s[s.size() / 2] > 2.2 * u) return "";       // dashes only: not text
    }
    std::sort(dots.begin(), dots.end());
    double u = dots[dots.size() / 2];
    if (u < 0.025 || u > 0.4) return "";               // 50 wpm .. 3 wpm
    if (unitOut) *unitOut = u;
    std::string text, sym;
    int bad = 0, total = 0;
    auto endChar = [&]() {
        if (sym.empty()) return;
        char c = '?';
        for (size_t i = 0; i < sizeof(kMorse) / sizeof(kMorse[0]); i++) if (sym == kMorse[i]) { c = kMorseChar[i]; break; }
        if (c == '?') bad++;
        total++;
        text += c;
        sym.clear();
    };
    for (const auto& r : runs) {
        const double d = r.second;
        if (r.first) {
            if (d > 6 * u) return "";
            sym += d < 2.0 * u ? '.' : '-';
            if (sym.size() > 7) return "";
        } else {
            if (d >= 5.0 * u) { endChar(); if (!text.empty() && text.back() != ' ') text += ' '; }
            else if (d >= 2.0 * u) endChar();
        }
    }
    endChar();
    while (!text.empty() && text.back() == ' ') text.pop_back();
    size_t alnum = 0;
    for (char c : text) if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) alnum++;
    if (total < 4 || bad * 5 > total || alnum * 10 < text.size() * 6) return "";
    return text;
}

std::unique_ptr<Chain> makeFskChain(int baud) { return std::make_unique<FskChain>(baud); }
std::unique_ptr<Chain> makeAfskChain() { return std::make_unique<AfskChain>(); }
std::unique_ptr<Chain> makeBpskChain(int baud) { return std::make_unique<BpskChain>(baud); }
std::unique_ptr<Chain> makeCwChain() { return std::make_unique<CwChain>(); }

} // namespace cubesat
} // namespace dect2
