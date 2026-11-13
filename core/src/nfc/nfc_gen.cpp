// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Synthetic NFC / RFID test signal (see nfc_gen.h). Nothing is transmitted.
#include "dect2/nfc/nfc_gen.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_proto.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::nfc {

namespace {
constexpr double kPi = 3.14159265358979323846;

// append `samples` of idle field (carrier on, amplitude amp) to a real-valued base envelope
void idle(std::vector<float>& env, size_t samples, double amp) { for (size_t i = 0; i < samples; i++) env.push_back((float)amp); }

// append a Type A reader (PCD) frame: 100% ASK modified Miller. The field is at amp, with pauses (near zero) where the symbols say.
// Symbol boundaries use the exact fractional samples-per-etu (matching the receiver's grid), so long frames stay aligned.
void readerFrameA(std::vector<float>& env, const std::vector<uint8_t>& bytes, int bits, double fs, double amp) {
    const std::vector<uint8_t> seq = bytesToMillerSeq(bytes, bits);
    const double sp = fs * kEtu;
    const size_t base = env.size();
    const size_t total = (size_t)std::llround((double)seq.size() * sp);
    for (size_t i = 0; i < total; i++) env.push_back((float)amp);
    const size_t pw = (size_t)std::max(2.0, sp * 0.3);
    for (size_t k = 0; k < seq.size(); k++) {
        const uint8_t sym = seq[k];
        double c = 0;
        if (sym == 1) c = (k + 0.75) * sp;        // X: pause in 2nd half
        else if (sym == 2) c = (k + 0.25) * sp;   // Z: pause in 1st half
        else continue;                            // Y: no pause
        const size_t start = base + (size_t)std::llround(c);
        for (size_t i = 0; i < pw; i++) if (start + i < env.size()) env[start + i] = (float)(amp * 0.02);
    }
}

// append a Type A / ISO 15693 tag (PICC) answer: carrier stays on, a subcarrier is added on the active Manchester half-bits.
// subHz: subcarrier frequency; etuSamples: samples per bit. Includes a full-on SOF etu for grid alignment. Exact fractional
// half-bit boundaries keep it aligned with the receiver's Manchester grid.
void tagFrameManchester(std::vector<float>& sub, std::vector<float>& field, size_t fieldStart,
                        const std::vector<uint8_t>& bits, double fs, double amp, double depth, double subHz, double etuSamples) {
    (void)fieldStart;
    std::vector<uint8_t> halves;
    halves.push_back(1); halves.push_back(1);   // SOF: one full-on etu
    for (uint8_t b : bits) { halves.push_back(b == 1 ? 1 : 0); halves.push_back(b == 0 ? 1 : 0); }   // 1: sub in first half, 0: second
    const double half = etuSamples / 2.0;
    const size_t total = (size_t)std::llround((double)halves.size() * half);
    double ph = 0;
    for (size_t i = 0; i < total; i++) {
        const size_t h = (size_t)((double)i / half);
        const bool on = h < halves.size() && halves[h];
        sub.push_back(on ? (float)(depth * amp * std::cos(ph)) : 0.f);
        ph += 2 * kPi * subHz / fs;
    }
    while (field.size() < fieldStart + total) field.push_back((float)amp);
}
}  // namespace

std::vector<cf32> renderSession(const GenConfig& g) {
    const double fs = g.fs;
    const double sp = fs * kEtu;
    const size_t gap = (size_t)(sp * 20);     // idle carrier between frames (> the segmenter's idle threshold)
    std::vector<float> field;                 // real field amplitude timeline
    std::vector<float> sub(0);                // subcarrier addition timeline (same index base as field)

    auto readerA = [&](const std::vector<uint8_t>& b, int bits) {
        idle(field, gap, g.amp);
        while (sub.size() < field.size()) sub.push_back(0.f);
        readerFrameA(field, b, bits, fs, g.amp);
        while (sub.size() < field.size()) sub.push_back(0.f);
    };
    auto tagA = [&](const std::vector<uint8_t>& b, int bits) {
        idle(field, gap, g.amp);
        while (sub.size() < field.size()) sub.push_back(0.f);
        const std::vector<uint8_t> bs = bytesToBits(b, bits);
        std::vector<float> s; tagFrameManchester(s, field, field.size(), bs, fs, g.amp, g.loadDepth, kSubA, sp);
        for (float v : s) sub.push_back(v);
        while (field.size() < sub.size()) field.push_back((float)g.amp);
    };

    if (g.session == 0) {   // ISO/IEC 14443 Type A
        const std::vector<uint8_t> uid = {0x04, 0xA1, 0xB2, 0xC3};   // example 4-byte UID
        uint8_t bcc = 0; for (uint8_t v : uid) bcc ^= v;
        readerA({0x26}, 7);                                          // REQA
        tagA({0x04, 0x00}, 16);                                     // ATQA (4-byte UID)
        readerA({0x93, 0x20}, 16);                                  // anticollision CL1
        tagA({uid[0], uid[1], uid[2], uid[3], bcc}, 40);            // UID + BCC
        {   // SELECT CL1 (with CRC_A)
            std::vector<uint8_t> sel = {0x93, 0x70, uid[0], uid[1], uid[2], uid[3], bcc};
            const uint16_t c = crcA(sel.data(), sel.size()); sel.push_back((uint8_t)(c & 0xFF)); sel.push_back((uint8_t)(c >> 8));
            readerA(sel, (int)sel.size() * 8);
        }
        {   // SAK (ISO 14443-4 not set) with CRC_A
            std::vector<uint8_t> sak = {0x00};
            const uint16_t c = crcA(sak.data(), sak.size()); sak.push_back((uint8_t)(c & 0xFF)); sak.push_back((uint8_t)(c >> 8));
            tagA(sak, (int)sak.size() * 8);
        }
        {   // READ block 4, then a Type 2 memory answer with an NDEF URI "http://nfc.example"
            std::vector<uint8_t> rd = {0x30, 0x04};
            const uint16_t c = crcA(rd.data(), rd.size()); rd.push_back((uint8_t)(c & 0xFF)); rd.push_back((uint8_t)(c >> 8));
            readerA(rd, (int)rd.size() * 8);
            // NDEF: TLV 0x03, len, record (SR, well-known "U"), URI id 0x03 ("http://") + "nfc.example"
            const std::string host = "nfc.example";
            std::vector<uint8_t> ndef = {0x03, 0,  0xD1, 0x01, 0, 'U', 0x03};
            for (char ch : host) ndef.push_back((uint8_t)ch);
            ndef.push_back(0xFE);                                    // terminator TLV
            ndef[1] = (uint8_t)(ndef.size() - 3);                    // NDEF message length (between len byte and terminator)
            ndef[4] = (uint8_t)(host.size() + 1);                    // record payload length
            std::vector<uint8_t> ans(ndef);
            const uint16_t cc = crcA(ans.data(), ans.size()); ans.push_back((uint8_t)(cc & 0xFF)); ans.push_back((uint8_t)(cc >> 8));
            tagA(ans, (int)ans.size() * 8);
        }
    } else if (g.session == 1) {   // ISO/IEC 15693 inventory
        // reader inventory request (1-of-4 PPM is approximated by the matched sparse-pause decoder)
        {
            std::vector<uint8_t> inv = {0x26, 0x01, 0x00};   // flags (inventory, high rate), Inventory, mask length 0
            const uint16_t c = crcB(inv.data(), inv.size()); inv.push_back((uint8_t)(c & 0xFF)); inv.push_back((uint8_t)(c >> 8));
            // render as a sparse reader PPM: one pause per symbol at the value's slot
            idle(field, gap, g.amp); while (sub.size() < field.size()) sub.push_back(0.f);
            const double symLen = sp * 8.0, pw = std::max(2.0, sp * 0.3);
            auto two = [&](int v) {
                const size_t base = field.size();
                for (size_t i = 0; i < (size_t)symLen; i++) field.push_back((float)g.amp);
                const size_t c = base + (size_t)(sp + v * sp * 2.0);
                for (size_t i = 0; i < (size_t)pw; i++) if (c + i < field.size()) field[c + i] = (float)(g.amp * 0.02);
            };
            for (uint8_t by : inv) { two(by & 3); two((by >> 2) & 3); two((by >> 4) & 3); two((by >> 6) & 3); }
            while (sub.size() < field.size()) sub.push_back(0.f);
        }
        {   // tag inventory response: flags 0x00, DSFID 0x00, UID (LSB first on the air)
            std::vector<uint8_t> uidMsb = {0xE0, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};   // NXP (0x04) example
            std::vector<uint8_t> resp = {0x00, 0x00};
            for (auto it = uidMsb.rbegin(); it != uidMsb.rend(); ++it) resp.push_back(*it);   // UID LSB first
            const uint16_t c = crcB(resp.data(), resp.size()); resp.push_back((uint8_t)(c & 0xFF)); resp.push_back((uint8_t)(c >> 8));
            idle(field, gap, g.amp); while (sub.size() < field.size()) sub.push_back(0.f);
            std::vector<uint8_t> bits; for (uint8_t by : resp) for (int k = 0; k < 8; k++) bits.push_back((by >> k) & 1);
            std::vector<float> s; tagFrameManchester(s, field, field.size(), bits, fs, g.amp, g.loadDepth, kSubV, sp * 4.0);
            for (float v : s) sub.push_back(v); while (field.size() < sub.size()) field.push_back((float)g.amp);
        }
    } else {   // ISO/IEC 14443 Type B (REQB / ATQB), reader frame rendered as 100% ASK Miller for the segmenter
        {
            std::vector<uint8_t> req = {0x05, 0x00, 0x00};   // REQB, AFI 0, 1 slot
            const uint16_t c = crcB(req.data(), req.size()); req.push_back((uint8_t)(c & 0xFF)); req.push_back((uint8_t)(c >> 8));
            readerA(req, (int)req.size() * 8);   // the demod reads the bytes; Type B PHY differs but the bytes match
        }
        {
            std::vector<uint8_t> atqb = {0x50, 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, 0, 0, 0};
            const uint16_t c = crcB(atqb.data(), atqb.size()); atqb.push_back((uint8_t)(c & 0xFF)); atqb.push_back((uint8_t)(c >> 8));
            tagA(atqb, (int)atqb.size() * 8);
        }
    }
    idle(field, gap, g.amp); while (sub.size() < field.size()) sub.push_back(0.f);

    // combine into complex baseband at the carrier offset
    const size_t n = field.size();
    std::vector<cf32> out(n);
    double ph = 0; const double step = 2 * kPi * g.carrierHz / fs;
    for (size_t i = 0; i < n; i++) {
        const double a = field[i] + (i < sub.size() ? sub[i] : 0.f);
        out[i] = cf32((float)(a * std::cos(ph)), (float)(a * std::sin(ph)));
        ph += step; if (ph > kPi) ph -= 2 * kPi;
    }
    return out;
}

namespace {
class NfcSynth : public ModeSynth {
public:
    NfcSynth(const SynthConfig& s, double rate) : rate_(rate), rng_(1234) {
        GenConfig g; g.fs = rate; g.session = s.modeOpt[0]; g.snrDb = s.snrDb; g.cfoHz = s.cfoHz;
        g.carrierHz = 0;   // the engine already places the carrier via the signal offset; the loop stays at 0 Hz
        buf_ = renderSession(g);
        if (buf_.empty()) buf_.assign((size_t)rate / 100, cf32(0.3f, 0));
        snr_ = s.snrDb; cfo_ = s.cfoHz;
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::normal_distribution<float> nd(0.f, 1.f);
        const double noiseAmp = snr_ > 0 && snr_ < 200 ? 0.3 * std::pow(10.0, -snr_ / 20.0) : 0.0;
        const double step = 2 * kPi * cfo_ / rate_;
        for (size_t i = 0; i < n; i++) {
            cf32 v = buf_[pos_];
            if (++pos_ >= buf_.size()) pos_ = 0;
            if (step != 0) { v *= cf32((float)std::cos(phase_), (float)std::sin(phase_)); phase_ += step; if (phase_ > kPi) phase_ -= 2 * kPi; }
            if (noiseAmp > 0) v += cf32((float)(noiseAmp * nd(rng_)), (float)(noiseAmp * nd(rng_)));
            out[i] = v;
        }
    }
private:
    double rate_;
    std::vector<cf32> buf_;
    size_t pos_ = 0;
    double snr_ = 1e9, cfo_ = 0, phase_ = 0;
    std::mt19937 rng_;
};
}  // namespace

std::unique_ptr<ModeSynth> makeNfcSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<NfcSynth>(cfg, sampleRate);
}

}  // namespace dect2::nfc
