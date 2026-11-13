// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Phase 2: the active NFC reader transmitter (see nfc_tx.h). Nothing transmits here; the TX gate and a real radio do that.
#include "dect2/nfc/nfc_tx.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_proto.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

bool nfcTxFrequencyAllowed(double hz) { return hz >= kNfcIsmLoHz && hz <= kNfcIsmHiHz; }

namespace {
// A courtesy amplitude cap so the generator never asks the radio for full scale on this mode.
constexpr float kNfcAmpCap = 0.25f;

// Append one 100% ASK modified-Miller reader frame to a real envelope timeline at amplitude `amp`.
void readerFrame(std::vector<float>& env, const std::vector<uint8_t>& bytes, int bits, double fs, float amp) {
    const std::vector<uint8_t> seq = nfc::bytesToMillerSeq(bytes, bits);
    const double sp = fs * nfc::kEtu;
    const size_t base = env.size();
    const size_t total = (size_t)std::llround((double)seq.size() * sp);
    for (size_t i = 0; i < total; i++) env.push_back(amp);
    const double pw = std::max(2.0, sp * 0.3);
    // The pause edges are raised-cosine slopes centred on the pause's ends (1 us down, 1.5 us up), so that its width at half depth stays 0.3 etu
    // (2.8 us) and the bottom stays at zero for about 1.6 us: inside ISO/IEC 14443-2 Type A (t1 2-3 us, t2 >= 0.5 us, rise t3 <= 1.5 us). Hard
    // steps put the reader's sidebands at -30 dBc more than three occupied bandwidths away.
    const double fall = std::max(1.0, 1.0e-6 * fs), rise = std::max(1.0, 1.5e-6 * fs);
    auto slope = [](double x) { return x <= 0 ? 0.0 : x >= 1 ? 1.0 : 0.5 - 0.5 * std::cos(3.14159265358979 * x); };   // 0 -> 1 over x in [0, 1]
    for (size_t k = 0; k < seq.size(); k++) {
        double c = 0;
        if (seq[k] == 1) c = (k + 0.75) * sp;        // X: pause in 2nd half
        else if (seq[k] == 2) c = (k + 0.25) * sp;   // Z: pause in 1st half
        else continue;
        const double a = (double)base + std::round(c), b = a + pw;   // the pause's ends at half depth
        const size_t i0 = (size_t)std::max(0.0, std::floor(a - fall / 2)), i1 = (size_t)std::ceil(b + rise / 2);
        for (size_t i = i0; i <= i1 && i < env.size(); i++) {
            const double t = (double)i;
            const double g = std::max(slope((a + fall / 2 - t) / fall), slope((t - (b - rise / 2)) / rise));   // 1 outside, 0 in the middle
            env[i] = std::min(env[i], (float)(amp * g));
        }
    }
}

std::vector<uint8_t> withCrcA(std::vector<uint8_t> b) {
    const uint16_t c = nfc::crcA(b.data(), b.size());
    b.push_back((uint8_t)(c & 0xFF)); b.push_back((uint8_t)(c >> 8));
    return b;
}

// The reader command script: a continuous carrier with the standard poll / select / read sequence, looped.
class NfcReaderTx : public TxGenerator {
public:
    NfcReaderTx(double fs, float amp) : fs_(fs) {
        const double sp = fs * nfc::kEtu;
        const size_t fieldGap = (size_t)(sp * 100);      // continuous carrier between frames (the tag stays powered)
        std::vector<float> env;
        auto gap = [&] { for (size_t i = 0; i < fieldGap; i++) env.push_back(amp); };   // carrier on, no modulation
        const uint8_t uid1[4] = {0x88, 0x04, 0xA1, 0xB2};   // CL1 with cascade tag 0x88 (a double-size UID example)
        uint8_t bcc1 = 0; for (uint8_t v : uid1) bcc1 ^= v;
        gap();
        readerFrame(env, {0x26}, 7, fs, amp);                                           // REQA
        gap();
        readerFrame(env, {0x93, 0x20}, 16, fs, amp);                                    // anticollision CL1
        gap();
        readerFrame(env, withCrcA({0x93, 0x70, uid1[0], uid1[1], uid1[2], uid1[3], bcc1}), 0, fs, amp);   // SELECT CL1
        gap();
        readerFrame(env, {0x95, 0x20}, 16, fs, amp);                                    // anticollision CL2
        gap();
        readerFrame(env, withCrcA({0x30, 0x04}), 0, fs, amp);                           // READ block 4 (Type 2)
        gap();
        readerFrame(env, withCrcA({0x60, 0x00}), 0, fs, amp);                           // GET_VERSION (NTAG / Type 2)
        gap();
        buf_.resize(env.size());
        for (size_t i = 0; i < env.size(); i++) buf_[i] = cf32(env[i], 0.f);            // carrier at DC; the radio upconverts to 13.56 MHz
        if (buf_.empty()) buf_.assign(1, cf32(amp, 0.f));
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { for (size_t i = 0; i < n; i++) { out[i] = buf_[pos_]; if (++pos_ >= buf_.size()) pos_ = 0; } }
private:
    double fs_;
    std::vector<cf32> buf_;
    size_t pos_ = 0;
};
}  // namespace

std::unique_ptr<TxGenerator> makeNfcReaderTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    const double fs = rateHz > 0 ? rateHz : 4e6;
    if (fs < 2.4e6) { err = "the NFC reader needs at least 2.4 Msps so a tag's 847.5 kHz answer subcarrier fits"; return nullptr; }
    float amp = kNfcAmpCap;
    const double trim = r.o("out.trimDb", 0.0);   // the panel's output trim can only lower it further
    if (trim < 0) amp *= (float)std::pow(10.0, trim / 20.0);
    return std::make_unique<NfcReaderTx>(fs, amp);
}

}  // namespace dect2
