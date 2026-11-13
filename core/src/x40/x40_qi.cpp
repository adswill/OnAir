// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Qi wireless charging, receiver-to-transmitter communication (Wireless Power Consortium Qi, baseline power profile, 110 - 205 kHz): the phone modulates the load
// of the power signal, a few percent of its amplitude, at 2 kbit/s (500 us, 2 kHz +- 4 %) in differential bi-phase (a level change at the start of every bit, one more
// in the middle of a 1). A byte is 11 bits: start 0, 8 data bits LSB first, odd parity, stop 1. A packet is a preamble of at least four 1 bits, a header byte (its value gives the
// message length), the message, and a checksum byte (XOR of header and message). The messages are named for the baseline profile (signal strength, end of power transfer,
// control error, received power, charge status, hold-off, configuration, identification); other headers are shown as hex. Receive only; the power signal itself is not touched.
#include "x40_ask.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 16000, kCut = 5000, kBit = 500e-6;

int msgBytes(unsigned h) {
    if (h < 0x20) return 1;
    if (h < 0x80) return 2 + (int)(h - 0x20) / 16;
    if (h < 0xE0) return 8 + (int)(h - 0x80) / 8;
    return 20 + (int)(h - 0xE0) / 4;
}

class QiDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); sl_.configure(rate_, 0.15, 0.22, 5.0);
        reset(); return true;
    }
    void reset() override {
        dec_.reset(); sl_.reset(); n_ = 0; est_ = kBit; last_ = 0; have_ = false; half_ = false; resetPacket(); ok_ = bad_ = 0; lastText_.clear();
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            n_++;
            sl_.push(std::abs(a), [&](double t, int before) { edge(t, before); });
            // the last bit of a packet is a 1 (the stop bit) and its second half has no change after it: a half interval followed by silence
            if (half_ && n_ / rate_ - last_ > 0.8 * est_) { half_ = false; bit(1); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!sl_.seen()) { t.headline = "waiting for data"; return; }
        t.headline = ok_ ? fmt("%d packets, last: %s", ok_, lastText_.c_str()) : "power signal found, no packet yet";
        t.facts.push_back({"power signal level", fmt("%.1f dB (relative)", toDb(sl_.carrier() * sl_.carrier()))});
        t.facts.push_back({"load modulation", sl_.carrier() > 0 && sl_.swing() > 0 ? fmt("%.1f %% of the amplitude", 100.0 * sl_.swing() / sl_.carrier()) : std::string("none seen")});
        t.facts.push_back({"packets", fmt("%d good, %d failed parity or checksum", ok_, bad_)});
        t.facts.push_back({"bit rate", fmt("%.0f bit/s", 1.0 / est_)});
    }
private:
    // interval since the last level change: one bit (a 0), or half a bit (two of them make a 1)
    void edge(double t, int) {
        if (!have_) { have_ = true; last_ = t; return; }
        const double dt = t - last_; last_ = t;
        const double r = dt / est_;
        if (r >= 0.75 && r <= 1.4) {
            est_ += 0.05 * (dt - est_); half_ = false;      // a lone full interval after a half is a framing error
            bit(0);
        } else if (r >= 0.3 && r < 0.75) {
            if (half_) { est_ += 0.05 * (dt * 2 - est_); half_ = false; bit(1); } else half_ = true;
        } else { half_ = false; resetPacket(); est_ = kBit; return; }
        est_ = std::min(kBit * 1.12, std::max(kBit * 0.88, est_));
    }
    void resetPacket() { st_ = 0; ones_ = 0; bits_.clear(); bytes_.clear(); need_ = 0; }
    void bit(int b) {
        if (st_ == 0) {                       // preamble: ones, then a start bit 0
            if (b) { if (++ones_ > 40) ones_ = 40; }
            else if (ones_ >= 4) { st_ = 1; bits_.clear(); }
            else ones_ = 0;
            return;
        }
        if (st_ == 3) { if (b) { fail(); return; } st_ = 1; bits_.clear(); return; }   // the start bit 0 of the next byte
        bits_.push_back((uint8_t)b);          // the 10 bits after the start bit of this byte
        if (bits_.size() < 10) return;
        unsigned v = 0, par = 0; for (int i = 0; i < 8; i++) { v |= (unsigned)bits_[i] << i; par ^= bits_[i]; }
        const int p = bits_[8], stop = bits_[9];
        bits_.clear();
        if (!(p ^ par) || !stop) { fail(); return; }        // odd parity: data bits plus the parity bit hold an odd number of ones
        bytes_.push_back((uint8_t)v);
        if (bytes_.size() == 1) need_ = 1 + msgBytes(v) + 1;
        if ((int)bytes_.size() == need_) {
            uint8_t x = 0; for (size_t i = 0; i + 1 < bytes_.size(); i++) x ^= bytes_[i];
            if (x == bytes_.back()) packet(); else fail();
            resetPacket(); return;
        }
        st_ = 3;
    }

    void fail() { bad_++; resetPacket(); }
    void packet() {
        ok_++; out_good();
        const unsigned h = bytes_[0]; const size_t m = bytes_.size() - 2;
        const uint8_t* d = bytes_.data() + 1;
        std::string id, text; char b[64];
        auto hex = [&]() { std::string s; for (size_t i = 0; i < m; i++) { snprintf(b, sizeof b, "%02X ", d[i]); s += b; } if (!s.empty()) s.pop_back(); return s; };
        switch (h) {
        case 0x01: id = "Signal strength"; text = fmt("signal strength %u of 255: how well the coils are coupled", d[0]); break;
        case 0x02: {
            static const char* e[] = {"unknown", "charge complete", "internal fault", "over temperature", "over voltage", "over current", "battery failure", "reconfigure", "no response"};
            id = "End power transfer"; text = fmt("the phone asks to stop: %s (code %u)", d[0] < 9 ? e[d[0]] : "reserved", d[0]); break; }
        case 0x03: id = "Control error"; text = fmt("control error %d (negative: less power, positive: more)", (int)(int8_t)d[0]); break;
        case 0x04: id = "Received power"; text = fmt("received power value %u (the phone's measure of what it receives)", d[0]); break;
        case 0x05: id = "Charge status"; text = d[0] <= 100 ? fmt("battery charge %u %%", d[0]) : fmt("charge status %u (not a percentage)", d[0]); break;
        case 0x06: id = "Power control hold-off"; text = fmt("the phone asks the charger to wait %u ms before changing power", d[0]); break;
        case 0x51: id = "Configuration"; text = fmt("configuration: %s%s", hex().c_str(), m >= 1 ? fmt(" (power class %u, maximum power value %u)", d[0] >> 6, d[0] & 0x3F).c_str() : ""); break;
        case 0x71: id = "Identification";
            if (m >= 7) text = fmt("Qi version %u.%u, manufacturer code %04X, device ID %02X%02X%02X%02X%s", d[0] >> 4, d[0] & 0xF, (unsigned)(d[1] << 8 | d[2]), d[3] & 0x7F, d[4], d[5], d[6], (d[3] & 0x80) ? " (extended identification follows)" : "");
            else text = hex();
            break;
        default: id = fmt("Header %02X", h); text = fmt("%zu message bytes: %s", m, hex().c_str()); break;
        }
        lastText_ = id;
        out->row("Packet", id, text, toDb(sl_.carrier() * sl_.carrier()));
    }
    void out_good() { out->good(sl_.carrier() > 0 && sl_.swing() > 0 ? 20.f * std::log10(std::max(sl_.swing() / sl_.carrier(), 1e-6f)) : -99.f, 0); }
    Decimator dec_; AskSlicer sl_; uint64_t n_ = 0; double rate_ = 1, est_ = kBit, last_ = 0; bool have_ = false, half_ = false;
    int st_ = 0, ones_ = 0, need_ = 0; std::vector<uint8_t> bits_, bytes_; int ok_ = 0, bad_ = 0; std::string lastText_;
};
}

std::unique_ptr<Decoder> makeQi() { return std::make_unique<QiDec>(); }

} // namespace dect2::x40
