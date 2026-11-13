// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SCADA radio telemetry (x40 variant Scada, docs/SCADA-4.1.md): the physical layer. Receive only; there is no transmit path in this mode.
//
// Two FM front ends (dect2::FmFront, the LTR / EDACS front end) give the discriminator at 96 kHz: a narrow channel (+-8 kHz) for the 1200 baud
// audio FSK and the 4800 bit/s modes, a wide one (+-16 kHz) for 9600 and 19200 bit/s. Seven demodulator chains run side by side on them:
//   audio FSK 1200 baud: the discriminator audio at 24 kHz is mixed down by 1700 Hz (the centre of both Bell 202, 1200 / 2200 Hz, and V.23,
//     1300 / 2100 Hz) and put through a second discriminator, so the mark / space decision does not depend on the audio level or tilt;
//     the steady tone frequencies name Bell 202 or V.23;
//   direct 2-FSK at 4800, 9600 and 19200 bit/s and 4-FSK at 4800, 9600 and 19200 bit/s (2400, 4800, 9600 symbols/s), straight from the
//     discriminator.
// Each chain: a boxcar of 3/4 of a symbol, the clock loop of the packet modem (Afsk1200 in packet_demod.h: the phase is pulled towards the
// level crossings, a symbol is read half a symbol after), and a decision-directed centre (carrier offset and drift) and deviation (LMS) with
// an eye-quality lock; 4-FSK takes its levels from the eye's envelope until it locks. The bits go to async framers (8N1, 8E1, 8O1, both polarities) and HDLC framers (NRZ both polarities, NRZI). A burst of
// bytes is cut at an idle gap of about 3 character times, when the chain loses lock, or at 1024 bytes, and searched for Modbus RTU and DNP3
// frames (scada_proto.cpp). Which chain and framer gave the valid frames names the link type, baud rate and framing; the chain's measured
// levels give the deviation. A burst the chain locked on that held no frame is kept as raw bits ("not recognised"); nothing is guessed.
#include "x40_int.h"
#include "dect2/x40/scada_proto.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/common/diag.h"
#include <array>
#include <deque>
#include <iterator>

namespace dect2::x40 {

namespace {

using namespace dect2::scada;

constexpr double kWork = 96000, kAudio = 24000, kAfskCentre = 1700;
constexpr double kNarrowHz = 8000, kWideHz = 16000;
constexpr int kGapBits = 28;             // idle mark bits that end a burst (Modbus RTU: 3.5 characters between frames, at most 1.5 inside one)
constexpr size_t kMaxBurst = 1024;
constexpr double kDupSec = 0.6;

struct ByteRec { uint8_t b = 0; bool err = false, locked = false; double t = 0; };   // t: the end of its stop bit

struct Uart {
    int parity = 0;                      // 0 none, 1 even, 2 odd
    bool inv = false;
    int state = 0, nbit = 0, idle = 0, ones = 0;
    uint16_t data = 0;
    std::vector<ByteRec> buf;
    void reset() { state = nbit = idle = ones = 0; data = 0; buf.clear(); }
};

struct Hdlc {
    bool nrzi = false, inv = false;
    int prev = 0, ones = 0, nb = 0;
    uint8_t sr = 0, cur = 0;
    bool in = false;
    std::vector<uint8_t> fr;
    void reset() { prev = ones = nb = 0; sr = cur = 0; in = false; fr.clear(); }
};

struct Chain {
    int kind = 0;                        // 0 audio FSK, 1 2-FSK, 2 4-FSK
    bool wide = false;
    double rate = kWork, sym = 1200, bitRate = 1200;
    double nomH = 0, hMin = 0, hMax = 0;
    int L = 1;
    std::vector<float> box;
    size_t bpos = 0;
    double bsum = 0;
    double ph = -0.5, step = 0;
    bool prevSide = false;
    double c = 0, h = 0, evm = 1;
    double envHi = 0, envLo = 0;         // the eye's envelope at the symbol instants (4-FSK while unlocked: a decision-directed fit can settle on a wrong level with an offset)
    bool locked = false;
    uint64_t nSym = 0, nSamp = 0, lockSym = 0;
    int lastSym = 0;
    double steadyHi = 0, steadyLo = 0;
    bool haveHi = false, haveLo = false;
    std::vector<Uart> uart;
    std::vector<Hdlc> hdlc;
    double lockStart = 0, evmSum = 0;
    int evmN = 0;
    std::vector<uint8_t> bitLog;
    uint64_t frames = 0;
    double t() const { return (double)nSamp / rate; }
};

struct Recent { std::vector<uint8_t> bytes; double t = 0; uint64_t id = 0; int rank = 0; };
struct UnknownCand { double t0 = 0, t1 = 0, evm = 1; std::string link; std::vector<uint8_t> bits; size_t nbits = 0; };

class ScadaDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs;
        okN_ = frontN_.configure(fs, kWork, kNarrowHz);
        okW_ = frontW_.configure(fs, kWork, kWideHz);
        if (!okN_ || !okW_) {
            DIAG_DEBUG(498099, "The SCADA receiver could not set up its FM front ends at %.0f samples per second, so it stays idle.", fs);
            return false;
        }
        if (!audioDec_.configure(kWork, 4, 3500)) {
            DIAG_DEBUG(156285, "The SCADA receiver could not set up its audio decimator from %.0f Hz, so it stays idle.", kWork);
            return false;
        }
        for (int i = 0; i < 2; i++) { lpR_[i].lowpass(1300, kAudio, i ? 1.3066 : 0.5412); lpI_[i].lowpass(1300, kAudio, i ? 1.3066 : 0.5412); }
        chains_.clear();
        addChain(0, false, kAudio, 1200, 1, 450, 250, 800);
        addChain(1, false, kWork, 4800, 1, 2000, 800, 4500);
        addChain(1, true, kWork, 9600, 1, 3000, 1200, 6000);
        addChain(1, true, kWork, 19200, 1, 4800, 2000, 9000);
        addChain(2, false, kWork, 2400, 2, 1800, 800, 4000);
        addChain(2, true, kWork, 4800, 2, 2400, 1000, 5500);
        addChain(2, true, kWork, 9600, 2, 4000, 1600, 8000);
        reset();
        return true;
    }
    void reset() override {
        frontN_.reset(); frontW_.reset(); audioDec_.reset();
        for (int i = 0; i < 2; i++) { lpR_[i].clear(); lpI_[i].clear(); }
        nco_ = cf32(1, 0); prevA_ = cf32(0, 0); dcX_ = dcY_ = 0; afDc_ = 0; afPow_ = 0;
        for (Chain& ch : chains_) resetChain(ch);
        recent_.clear(); cands_.clear();
        lastAccept_ = -1e9;
    }
    void feed(const cf32* x, size_t n) override {
        if (!okN_ || !okW_) return;
        discN_.clear(); discW_.clear();
        frontN_.feed(x, n, discN_);
        frontW_.feed(x, n, discW_);
        Chain& af = chains_[0];
        for (float v : discN_) {
            // carrier offset and FM deviation of the audio modem, measured while it is locked
            if (af.locked) { afDc_ += 0.0005 * (v - afDc_); }
            const double y = v - dcX_ + 0.995 * dcY_;     // DC block (the carrier offset) before the tone discriminator
            dcX_ = v; dcY_ = y;
            if (af.locked) afPow_ += 0.0005 * (y * y - afPow_);
            const cf32 in((float)y, 0.f);
            audioDec_.process(&in, 1, [&](cf32 a) { audio(a.real()); });
            for (size_t k = 1; k < chains_.size(); k++) if (!chains_[k].wide) push(chains_[k], v);
        }
        for (float v : discW_) for (size_t k = 1; k < chains_.size(); k++) if (chains_[k].wide) push(chains_[k], v);
        flushCands(false);
    }
    void fill(X40Telemetry& t) const override {
        Store& st = store();
        std::lock_guard<std::mutex> lk(st.mu);
        const Tracker::Counts& c = st.tr.counts;
        const size_t nDev = st.tr.devices().size();
        const Chain* best = nullptr;
        for (const Chain& ch : chains_) if (ch.locked && (!best || ch.evm < best->evm)) best = &ch;
        const uint64_t total = c.modbus + c.dnp3 + c.unknown;
        if (total) t.headline = fmt("%s: %llu messages (%llu Modbus RTU, %llu DNP3, %llu not recognised), %zu devices", lastLink_.empty() ? "SCADA telemetry" : lastLink_.c_str(),
                                    (unsigned long long)total, (unsigned long long)c.modbus, (unsigned long long)c.dnp3, (unsigned long long)c.unknown, nDev);
        else if (best) t.headline = fmt("signal on %s, no frame yet", chainName(*best).c_str());
        else t.headline = "listening for Bell 202 / V.23 audio FSK at 1200 baud and 2-FSK / 4-FSK at 4800, 9600 and 19200 bit/s";
        t.facts.push_back({"link", st.phy.empty() ? std::string("nothing decoded yet") : st.phy});
        if (best) t.facts.push_back({"locked now", fmt("%s, eye error %.2f", chainName(*best).c_str(), best->evm)});
        t.facts.push_back({"frames", fmt("%llu Modbus RTU, %llu DNP3, %llu not recognised, %llu CRC failed, %llu pairs, %llu DNP3 authentication (not decoded)",
                                         (unsigned long long)c.modbus, (unsigned long long)c.dnp3, (unsigned long long)c.unknown, (unsigned long long)c.crcBad,
                                         (unsigned long long)c.paired, (unsigned long long)c.encrypted)});
        t.facts.push_back({"devices", fmt("%zu (Devices tab)", nDev)});
        t.facts.push_back({"note", "receive only; not verified on a live signal (simulated signals only)"});
    }

private:
    double fs_ = 0;
    bool okN_ = false, okW_ = false;
    FmFront frontN_, frontW_;
    std::vector<float> discN_, discW_;
    Decimator audioDec_;
    FmBiquad lpR_[2], lpI_[2];
    cf32 nco_{1, 0}, prevA_{0, 0};
    double dcX_ = 0, dcY_ = 0, afDc_ = 0, afPow_ = 0;
    std::vector<Chain> chains_;
    std::deque<Recent> recent_;
    std::deque<UnknownCand> cands_;
    double lastAccept_ = -1e9;
    std::string lastLink_;

    void addChain(int kind, bool wide, double rate, double sym, int bps, double nomH, double hMin, double hMax) {
        Chain ch;
        ch.kind = kind; ch.wide = wide; ch.rate = rate; ch.sym = sym; ch.bitRate = sym * bps;
        ch.nomH = nomH; ch.hMin = hMin; ch.hMax = hMax;
        ch.L = std::max(1, (int)std::lround(0.75 * rate / sym));
        ch.step = sym / rate;
        for (int par = 0; par < 3; par++)
            for (int inv = 0; inv < (kind == 0 ? 1 : 2); inv++) { Uart u; u.parity = par == 0 ? 1 : par == 1 ? 2 : 0; u.inv = inv; ch.uart.push_back(u); }   // parity framers first: they win a tie
        for (int inv = 0; inv < (kind == 0 ? 1 : 2); inv++) { Hdlc hd; hd.inv = inv; ch.hdlc.push_back(hd); }
        { Hdlc hd; hd.nrzi = true; ch.hdlc.push_back(hd); }
        chains_.push_back(std::move(ch));
    }
    void resetChain(Chain& ch) {
        ch.box.assign((size_t)ch.L, 0.f); ch.bpos = 0; ch.bsum = 0;
        ch.ph = -0.5; ch.prevSide = false;
        ch.c = 0; ch.h = ch.nomH; ch.evm = 1; ch.envHi = ch.nomH; ch.envLo = -ch.nomH; ch.locked = false; ch.nSym = ch.nSamp = ch.lockSym = 0;
        ch.lastSym = 0; ch.haveHi = ch.haveLo = false; ch.steadyHi = ch.steadyLo = 0;
        for (Uart& u : ch.uart) u.reset();
        for (Hdlc& hd : ch.hdlc) hd.reset();
        ch.bitLog.clear(); ch.evmSum = 0; ch.evmN = 0;
    }

    // the second discriminator of the audio modem, at 24 kHz
    void audio(float a) {
        const cf32 m = cf32(a, 0.f) * nco_;
        static const double kStep = -2 * kPi * kAfskCentre / kAudio;
        static const cf32 rot((float)std::cos(kStep), (float)std::sin(kStep));
        nco_ *= rot;
        if (std::abs(std::norm(nco_) - 1.f) > 1e-3f) nco_ /= std::abs(nco_);
        double re = lpR_[0].run(m.real()), im = lpI_[0].run(m.imag());
        re = lpR_[1].run(re); im = lpI_[1].run(im);
        const cf32 y((float)re, (float)im);
        const float f = std::arg(y * std::conj(prevA_)) * (float)(kAudio / (2 * kPi));
        prevA_ = y;
        push(chains_[0], f);
    }

    std::string chainName(const Chain& ch) const {
        if (ch.kind == 0) {
            if (ch.haveHi && ch.haveLo) {
                const double mark = kAfskCentre + ch.steadyLo, space = kAfskCentre + ch.steadyHi;
                const bool bell = std::fabs(mark - 1200) + std::fabs(space - 2200) <= std::fabs(mark - 1300) + std::fabs(space - 2100);
                return bell ? "Bell 202 AFSK 1200 Bd" : "V.23 AFSK 1200 Bd";
            }
            return "AFSK 1200 Bd (Bell 202 or V.23)";
        }
        if (ch.kind == 1) return fmt("2-FSK %.0f bit/s", ch.bitRate);
        return fmt("4-FSK %.0f bit/s (%.0f symbols/s)", ch.bitRate, ch.sym);
    }
    std::string phyLine(const Chain& ch) const {
        std::string s = chainName(ch);
        if (ch.kind == 0) {
            if (ch.haveHi && ch.haveLo) s += fmt(", tones %.0f / %.0f Hz", kAfskCentre + ch.steadyLo, kAfskCentre + ch.steadyHi);
            s += fmt(", FM deviation about %.1f kHz, carrier offset %+.0f Hz", std::sqrt(2 * std::max(0.0, afPow_)) / 1000, afDc_);
        } else {
            const double dev = ch.haveHi && ch.haveLo ? 0.5 * (ch.steadyHi - ch.steadyLo) : ch.h;
            s += fmt(", deviation %s%.1f kHz, carrier offset %+.0f Hz", ch.kind == 2 ? "(outer) " : "", dev / 1000, ch.c);
        }
        s += fmt(", eye error %.2f", ch.evm);
        return s;
    }

    void push(Chain& ch, float v) {
        ch.bsum += (double)v - ch.box[ch.bpos];
        ch.box[ch.bpos] = v;
        if (++ch.bpos == ch.box.size()) ch.bpos = 0;
        ch.nSamp++;
        if ((ch.nSamp & 0xFFFFF) == 0) { double s = 0; for (float b : ch.box) s += b; ch.bsum = s; }
        const double y = ch.bsum / ch.L;
        const bool side = y > ch.c;
        if (side != ch.prevSide) ch.ph *= ch.locked ? 0.85 : 0.6;
        ch.prevSide = side;
        ch.ph += ch.step;
        if (ch.ph >= 0.5) { ch.ph -= 1.0; symbol(ch, y); }
    }

    void symbol(Chain& ch, double y) {
        ch.envHi += (y > ch.envHi ? 0.3 : 0.02) * (y - ch.envHi);
        ch.envLo += (y < ch.envLo ? 0.3 : 0.02) * (y - ch.envLo);
        if (ch.kind == 2 && !ch.locked) { ch.c = 0.5 * (ch.envHi + ch.envLo); ch.h = std::min(ch.hMax, std::max(ch.hMin, 0.5 * (ch.envHi - ch.envLo))); }
        const double u = (y - ch.c) / ch.h;
        int s;
        double ideal;
        if (ch.kind == 2) { s = u > 2.0 / 3 ? 3 : u > 0 ? 1 : u > -2.0 / 3 ? -1 : -3; ideal = s / 3.0; }
        else { s = u > 0 ? 1 : -1; ideal = s; }
        const double e = y - (ch.c + ch.h * ideal);
        ch.h = std::min(ch.hMax, std::max(ch.hMin, ch.h + 0.03 * e * ideal));
        ch.c += 0.01 * e;
        if (ch.kind == 0) ch.c = std::min(250.0, std::max(-250.0, ch.c));   // the audio tones' centre never moves far from 1700 Hz
        const double half = ch.kind == 2 ? ch.h / 3 : ch.h;
        ch.evm += (std::min(2.0, std::fabs(e) / half) - ch.evm) / 32;
        ch.nSym++;
        const int top = ch.kind == 2 ? 3 : 1;
        if (s == ch.lastSym && s == top) { ch.steadyHi = ch.haveHi ? ch.steadyHi + 0.1 * (y - ch.steadyHi) : y; ch.haveHi = true; }
        if (s == ch.lastSym && s == -top) { ch.steadyLo = ch.haveLo ? ch.steadyLo + 0.1 * (y - ch.steadyLo) : y; ch.haveLo = true; }
        ch.lastSym = s;
        const double t = ch.t();
        if (!ch.locked && ch.nSym > 32 && ch.evm < 0.42) {
            ch.locked = true; ch.lockSym = ch.nSym; ch.lockStart = t; ch.bitLog.clear(); ch.evmSum = 0; ch.evmN = 0;
        } else if (ch.locked && ch.evm > 0.6) {
            ch.locked = false;
            unlocked(ch, t);
        }
        if (ch.locked) { ch.evmSum += ch.evm; ch.evmN++; }
        // the bits: higher frequency = 1 for direct FSK; mark (the lower tone) = 1 for audio FSK; 4-FSK dibits +3 01, +1 00, -1 10, -3 11
        int nb[2], ib[2], nbits = 1;
        if (ch.kind == 2) {
            auto dib = [](int q, int* b) { b[0] = q < 0; b[1] = q == 3 || q == -3; };
            dib(s, nb); dib(-s, ib); nbits = 2;
        } else if (ch.kind == 1) { nb[0] = s > 0; ib[0] = !nb[0]; }
        else { nb[0] = s < 0; ib[0] = !nb[0]; }
        for (int k = 0; k < nbits; k++) {
            if (ch.locked && ch.bitLog.size() < 8192) ch.bitLog.push_back((uint8_t)nb[k]);
            for (Uart& uf : ch.uart) uartBit(ch, uf, uf.inv ? ib[k] : nb[k], t);
            for (Hdlc& hf : ch.hdlc) hdlcBit(ch, hf, hf.inv ? ib[k] : nb[k], t);
        }
        if (ch.locked && ch.bitLog.size() >= 8192) { candidate(ch, t); ch.bitLog.clear(); ch.lockStart = t; ch.evmSum = 0; ch.evmN = 0; }
    }

    void unlocked(Chain& ch, double t) {
        for (Uart& u : ch.uart) if (!u.buf.empty()) flushUart(ch, u, t);
        candidate(ch, t);
        ch.bitLog.clear();
    }

    void candidate(Chain& ch, double t) {
        if (ch.bitLog.size() < (size_t)(64 * (ch.kind == 2 ? 2 : 1))) return;
        size_t changes = 0;
        for (size_t i = 1; i < ch.bitLog.size(); i++) changes += ch.bitLog[i] != ch.bitLog[i - 1];
        if (changes < std::max<size_t>(16, ch.bitLog.size() / 16)) return;   // an idle carrier, not a burst of data
        UnknownCand c;
        c.t0 = ch.lockStart; c.t1 = t; c.evm = ch.evmN ? ch.evmSum / ch.evmN : 1;
        if (c.evm > 0.35) return;
        c.link = chainName(ch);
        c.nbits = ch.bitLog.size();
        for (size_t i = 0; i + 7 < ch.bitLog.size(); i += 8) { uint8_t b = 0; for (int k = 0; k < 8; k++) b = (uint8_t)((b << 1) | ch.bitLog[i + k]); c.bits.push_back(b); }
        auto at = cands_.end();
        while (at != cands_.begin() && std::prev(at)->t0 > c.t0) --at;
        cands_.insert(at, std::move(c));
    }

    // Unknown bursts: once no chain can still give a frame for them (0.5 s after), keep the best-locked chain's bits of each burst when no
    // valid frame came in its time.
    void flushCands(bool all) {
        double now = 0;
        for (const Chain& ch : chains_) now = std::max(now, ch.t());
        while (!cands_.empty() && (all || now - cands_.front().t1 > 0.5)) {
            UnknownCand best = cands_.front();
            cands_.pop_front();
            double t0 = best.t0, t1 = best.t1;
            while (!cands_.empty() && cands_.front().t0 < t1 + 0.05) {   // the same burst seen by several chains: the cleanest eye names it
                t0 = std::min(t0, cands_.front().t0); t1 = std::max(t1, cands_.front().t1);
                if (cands_.front().evm < best.evm) best = cands_.front();
                cands_.pop_front();
            }
            if (lastAccept_ >= t0 - 0.3 && lastAccept_ <= t1 + 0.5) continue;
            bool hit = false;
            for (const Recent& r : recent_) if (r.t >= t0 - 0.3 && r.t <= t1 + 0.5) hit = true;
            if (hit) continue;
            Frame f;
            f.timeSec = best.t1; f.link = best.link; f.proto = Unknown; f.bytes = best.bits;
            f.note = fmt("%zu bits as sent (packed MSB first, higher frequency = 1)", best.nbits);
            commit(f, 0, nullptr, false);
        }
    }

    void uartBit(Chain& ch, Uart& u, int b, double t) {
        switch (u.state) {
        case 0: if (b) { u.state = 1; u.idle = 1; } break;
        case 1:
            if (b) { if (++u.idle == kGapBits && !u.buf.empty()) flushUart(ch, u, t); }
            else { u.state = 2; u.nbit = 0; u.data = 0; u.ones = 0; }
            break;
        default: {
            u.nbit++;
            if (u.nbit <= 8) { u.data |= (uint16_t)(b << (u.nbit - 1)); u.ones += b; break; }
            if (u.parity && u.nbit == 9) { u.ones += b; break; }
            bool err = !b;
            if (u.parity == 1 && (u.ones & 1)) err = true;
            if (u.parity == 2 && !(u.ones & 1)) err = true;
            u.buf.push_back(ByteRec{(uint8_t)u.data, err, ch.locked, t});
            if (b) { u.state = 1; u.idle = 0; } else u.state = 0;
            if (u.buf.size() >= kMaxBurst) flushUart(ch, u, t);
            break;
        }
        }
    }

    void flushUart(Chain& ch, Uart& u, double t) {
        size_t nLocked = 0;
        for (const ByteRec& r : u.buf) nLocked += r.locked;
        if (!nLocked) { u.buf.clear(); return; }
        std::vector<uint8_t> by(u.buf.size());
        for (size_t i = 0; i < by.size(); i++) by[i] = u.buf[i].b;
        size_t used = 0;
        for (const Found& f : findFrames(by.data(), by.size())) {
            size_t errs = 0, lk = 0;
            for (size_t i = f.start; i < f.start + f.len; i++) { errs += u.buf[i].err; lk += u.buf[i].locked; }
            used = f.start + f.len;
            if (f.crcOk && errs) continue;                 // a valid CRC read through framing or parity errors: another framer has these bytes right
            if (!u.buf[f.start + f.len - 1].locked && 2 * lk < f.len) continue;
            if (!f.crcOk && lk < f.len) continue;          // damaged frames only from a fully locked stretch
            const char* par = u.parity == 1 ? "8E1" : u.parity == 2 ? "8O1" : "8N1";
            accept(ch, std::string(par) + (u.inv ? ", inverted" : ""), u.parity ? 2 : 1, f, by.data(), u.buf[f.start + f.len - 1].t);
        }
        if (u.buf.size() >= kMaxBurst && used < u.buf.size()) {
            const size_t keep = std::min<size_t>(300, u.buf.size() - used);   // a frame may straddle the cut
            u.buf.erase(u.buf.begin(), u.buf.end() - (ptrdiff_t)keep);
        } else u.buf.clear();
    }

    void hdlcBit(Chain& ch, Hdlc& hd, int raw, double t) {
        int d = raw;
        if (hd.nrzi) { d = raw == hd.prev ? 1 : 0; hd.prev = raw; }
        hd.sr = (uint8_t)((hd.sr >> 1) | (d << 7));
        if (hd.sr == 0x7E) {
            if (hd.in && hd.nb == 7 && hd.fr.size() >= 6) hdlcFrame(ch, hd, t);
            hd.in = true; hd.fr.clear(); hd.nb = 0; hd.cur = 0; hd.ones = 0;
            return;
        }
        if (!hd.in) return;
        if (d) { if (++hd.ones >= 7) { hd.in = false; return; } }
        else { if (hd.ones == 5) { hd.ones = 0; return; } hd.ones = 0; }
        hd.cur |= (uint8_t)(d << hd.nb);
        if (++hd.nb == 8) { hd.fr.push_back(hd.cur); hd.cur = 0; hd.nb = 0; if (hd.fr.size() > 600) hd.in = false; }
    }

    void hdlcFrame(Chain& ch, Hdlc& hd, double t) {
        const size_t n = hd.fr.size();
        if (crcHdlc(hd.fr.data(), n - 2) != (hd.fr[n - 2] | (hd.fr[n - 1] << 8))) return;
        if (!ch.locked) return;
        const std::string lab = std::string(hd.nrzi ? "HDLC NRZI" : "HDLC") + (hd.inv ? ", inverted" : "");
        const std::vector<Found> fs = findFrames(hd.fr.data(), n - 2);
        if (fs.empty()) {
            Frame f; f.timeSec = t; f.link = chainName(ch) + ", " + lab; f.proto = Unknown;
            f.bytes.assign(hd.fr.begin(), hd.fr.end() - 2);
            f.note = "HDLC frame with a valid FCS whose content is neither Modbus RTU nor DNP3";
            commit(f, 3, &ch, true);
            return;
        }
        for (const Found& f : fs) accept(ch, lab, 3, f, hd.fr.data(), t);
    }

    void accept(Chain& ch, const std::string& framing, int rank, const Found& fd, const uint8_t* p, double t) {
        Frame f;
        f.timeSec = t; f.proto = fd.proto; f.crcOk = fd.crcOk; f.badBlock = fd.badBlock;
        f.bytes.assign(p + fd.start, p + fd.start + fd.len);
        f.link = chainName(ch) + ", " + framing;
        commit(f, rank, &ch, true);
    }

    void commit(const Frame& f, int rank, Chain* ch, bool valid) {
        while (!recent_.empty() && f.timeSec - recent_.front().t > 5) recent_.pop_front();
        Store& st = store();
        for (Recent& r : recent_) {
            if (r.bytes == f.bytes && std::fabs(f.timeSec - r.t) < kDupSec) {
                if (rank > r.rank) {                       // the same frame through a stricter framing (8E1 over 8N1): that framing is the link
                    std::lock_guard<std::mutex> lk(st.mu);
                    st.tr.relabel(r.id, f.link);
                    st.version++;
                    r.rank = rank;
                    lastLink_ = f.link;
                    if (ch) st.phy = phyLine(*ch) + ", " + f.link.substr(f.link.rfind(", ") + 2);
                }
                return;
            }
        }
        uint64_t id;
        Message m;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            id = st.tr.add(f);
            if (const Message* pm = st.tr.find(id)) m = *pm;
            if (ch && valid && f.proto != Unknown) st.phy = phyLine(*ch) + ", " + f.link.substr(f.link.rfind(", ") + 2);
            st.version++;
        }
        if (!id) return;
        recent_.push_back(Recent{f.bytes, f.timeSec, id, rank});
        if (f.proto != Unknown) { lastAccept_ = f.timeSec; lastLink_ = f.link; if (ch) ch->frames++; }
        static const char* const kind[3] = {"Modbus RTU", "DNP3", "not recognised"};
        std::string text = m.func + (m.summary.empty() ? "" : ": " + m.summary);
        if (m.crc == 0) text += " [CRC failed]";
        if (m.rttMs >= 0) text += fmt(" [%.0f ms]", m.rttMs);
        out->row(kind[f.proto], m.from + " > " + m.to, text, ch ? (float)(-20 * std::log10(std::max(0.01, ch->evm))) : 0.f);
        if (f.proto == Unknown) out->energy();
        else if (f.crcOk) out->good(ch ? (float)(-20 * std::log10(std::max(0.01, ch->evm))) : 0.f, ch ? (ch->kind == 0 ? afDc_ : ch->c) : 0.0);
        else out->bad();
    }
};

} // namespace

std::unique_ptr<Decoder> makeScada() { return std::make_unique<ScadaDec>(); }

} // namespace dect2::x40
