// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KiwiSDR wire pieces without a socket (source_kiwisdr.cpp, tests/source/test_kiwisdr.cpp): the text commands, the server's MSG and SND
// binary messages, and the interpolator that takes the Kiwi's 12 kHz IQ up to a rate the receivers accept.
//
// Clean-room from protocol FACTS read in the public kiwiclient sources (jks-prv/kiwiclient, kiwi/client.py) and the KiwiSDR documentation;
// no code was copied. VERIFIED = read in kiwiclient; ASSUMED = from the task brief / memory, not seen in a source.
//  - Every server message is one binary WebSocket message that starts with a 3-byte tag: "MSG", "SND", "W/F", "EXT"        (VERIFIED)
//  - MSG: byte 3 skipped, then space separated name=value tokens, values URL-encoded (%XX); a token without '=' is a flag      (VERIFIED)
//  - SND: [0..2] "SND", [3] flags, [4..7] sequence u32 LE, [8..9] S-meter u16 BE (RSSI dBm = 0.1 * v - 127), then the payload     (VERIFIED)
//    flags: 0x02 ADC overflow, 0x08 stereo (IQ), 0x10 compressed (IMA ADPCM; IQ is never compressed), 0x80 little-endian samples (camp only)
//    IQ payload: a 10-byte GPS block [gpsStatus u8, unused u8, gpssec u32 LE, gpsnsec u32 LE], then interleaved s16 big-endian samples (VERIFIED);
//    I first, Q second (ASSUMED; a setting swaps them)
//  - W/F: [3] skipped, [4..7] x_bin_server u32 LE, [8..11] flags_x_zoom u32 LE, [12..15] sequence u32 LE, then one byte per pixel (VERIFIED)
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace kiwi {

constexpr double kPi = 3.14159265358979323846;
constexpr double kIqRate = 12000.0;   // nominal; the server also names sample_rate (a float close to it)

// ---- MSG -----------------------------------------------------------------------------------------------------------------------------------
inline std::string urlDecode(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else r += s[i];
    }
    return r;
}
inline std::string urlEncode(const std::string& s) {   // for ident_user and the password in a SET command (spaces would split the token)
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
    }
    return r;
}

// A MSG body into name/value pairs. false: not a MSG (tag), or shorter than the tag; at most 64 pairs and 4 KiB of text are read.
inline bool parseMsg(const unsigned char* p, size_t n, std::vector<std::pair<std::string, std::string>>& kv) {
    kv.clear();
    if (n < 4 || memcmp(p, "MSG", 3) != 0) return false;
    if (n > 4096 + 4) n = 4096 + 4;
    std::string tok;
    auto flush = [&] {
        if (tok.empty() || kv.size() >= 64) { tok.clear(); return; }
        const size_t e = tok.find('=');
        if (e == std::string::npos) kv.emplace_back(tok, std::string());
        else kv.emplace_back(tok.substr(0, e), urlDecode(tok.substr(e + 1)));
        tok.clear();
    };
    for (size_t i = 4; i < n; i++) {
        const char c = (char)p[i];
        if (c == ' ' || c == '\n' || c == '\0') flush();
        else tok += c;
    }
    flush();
    return true;
}

// ---- SND -----------------------------------------------------------------------------------------------------------------------------------
enum SndFlags { kOverflow = 0x02, kStereo = 0x08, kCompressed = 0x10, kLittleEndian = 0x80 };

struct Snd {
    uint8_t flags = 0;
    uint32_t seq = 0;
    uint16_t smeter = 0;
    double rssiDbm = -127;
    bool iq = false;
    uint8_t gpsStatus = 0;
    uint32_t gpsSec = 0, gpsNsec = 0;
    size_t sampleOff = 0, samples = 0;   // complex samples at p + sampleOff
};
enum class SndResult { Ok, NotSnd, TooShort, Compressed, NotIq, OddSize };

inline SndResult parseSnd(const unsigned char* p, size_t n, Snd& o) {
    o = Snd();
    if (n < 3 || memcmp(p, "SND", 3) != 0) return SndResult::NotSnd;
    if (n < 10) return SndResult::TooShort;
    o.flags = p[3];
    o.seq = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    o.smeter = (uint16_t)(p[8] << 8 | p[9]);
    o.rssiDbm = 0.1 * o.smeter - 127.0;
    if (o.flags & kCompressed) return SndResult::Compressed;   // ADPCM audio: we ask for compression=0, and IQ is never compressed
    if (!(o.flags & kStereo)) return SndResult::NotIq;
    if (n < 20) return SndResult::TooShort;
    o.iq = true;
    o.gpsStatus = p[10];
    o.gpsSec = (uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
    o.gpsNsec = (uint32_t)p[16] | (uint32_t)p[17] << 8 | (uint32_t)p[18] << 16 | (uint32_t)p[19] << 24;
    o.sampleOff = 20;
    const size_t bytes = n - 20;
    o.samples = bytes / 4;
    return bytes % 4 ? SndResult::OddSize : SndResult::Ok;   // OddSize: the whole samples are still usable
}

// s16 pairs (big-endian unless the frame says little) to floats in +-1; swap: Q first
inline void decodeIq(const unsigned char* p, const Snd& s, float* out /* 2 floats per sample */, bool swapIq) {
    const bool le = s.flags & kLittleEndian;
    const unsigned char* b = p + s.sampleOff;
    for (size_t k = 0; k < s.samples; k++) {
        int16_t v[2];
        for (int c = 0; c < 2; c++) {
            const unsigned char* q = b + 4 * k + 2 * c;
            v[c] = le ? (int16_t)(q[0] | q[1] << 8) : (int16_t)(q[0] << 8 | q[1]);
        }
        out[2 * k] = v[swapIq ? 1 : 0] / 32768.f;
        out[2 * k + 1] = v[swapIq ? 0 : 1] / 32768.f;
    }
}

// ---- W/F -----------------------------------------------------------------------------------------------------------------------------------
struct Wf { uint32_t xBin = 0, flagsZoom = 0, seq = 0; size_t pixOff = 16, pixels = 0; };
inline bool parseWf(const unsigned char* p, size_t n, Wf& o) {
    o = Wf();
    if (n < 16 || memcmp(p, "W/F", 3) != 0) return false;
    auto u32 = [&](size_t i) { return (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24; };
    o.xBin = u32(4); o.flagsZoom = u32(8); o.seq = u32(12);
    o.pixels = n - 16;
    return true;
}

// ---- commands ------------------------------------------------------------------------------------------------------------------------------
inline std::string freqKHz(double hz) {
    char b[48];
    snprintf(b, sizeof b, "%.3f", hz / 1000.0);
    return b;
}
inline std::string authCmd(const std::string& password) { return "SET auth t=kiwi p=" + (password.empty() ? std::string("#") : urlEncode(password)); }   // '#': no password (ASSUMED, as the Kiwi's own page does)
inline std::string tuneCmd(double hz, int cutHz) {   // IQ mode: the passband is symmetric around the tuned frequency
    return "SET mod=iq low_cut=" + std::to_string(-cutHz) + " high_cut=" + std::to_string(cutHz) + " freq=" + freqKHz(hz);
}
inline std::string agcCmd(bool on, int manualGainDb) {
    return std::string("SET agc=") + (on ? "1" : "0") + " hang=0 thresh=-100 slope=6 decay=1000 manGain=" + std::to_string(manualGainDb);
}

// ---- the interpolator ----------------------------------------------------------------------------------------------------------------------
// 12 kHz complex IQ up by L (21: 252 kHz, the least that the receivers' 250 kHz minimum accepts; 84: 1.008 MHz for the general receiver, whose
// channel sits 250 kHz off the centre), polyphase windowed-sinc: passband to 5.5 kHz, stopband from 6.5 kHz (the images of the Kiwi's own edge). A
// mixer after it can place the Kiwi's centre at -shiftHz, where the engine expects the dial (the modes' tune offset).
class Upsampler {
public:
    static constexpr int kTaps = 36;   // per phase
    void configure(int L, double shiftHz) {
        L_ = L;
        const int N = kTaps * L;
        h_.assign((size_t)N, 0.f);
        const double fs = kIqRate * L, fc = 6000.0 / fs;   // cut-off in cycles per output sample
        const double beta = 4.5;
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double ib = i0(beta);
        for (int n = 0; n < N; n++) {
            const double m = n - (N - 1) / 2.0;
            const double x = 2 * kPi * fc * m;
            const double sinc = std::fabs(m) < 1e-9 ? 2 * fc : std::sin(x) / (kPi * m);
            const double r = 2.0 * n / (N - 1) - 1.0;
            const double w = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / ib;
            h_[(size_t)n] = (float)(sinc * w * L);   // gain L: zero-stuffing divides the amplitude by L
        }
        hist_.assign(kTaps, std::complex<float>(0, 0));
        pos_ = 0;
        shiftHz_ = shiftHz;
        step_ = std::polar(1.0, -2 * kPi * shiftHz / fs);   // the dial moves to -shift
        rot_ = std::complex<double>(1, 0);
    }
    int factor() const { return L_; }
    double outRate() const { return kIqRate * L_; }
    void setShift(double hz) { shiftHz_ = hz; step_ = std::polar(1.0, -2 * kPi * hz / outRate()); }
    // appends L outputs per input to out
    void process(const std::complex<float>* in, size_t n, std::vector<std::complex<float>>& out) {
        if (L_ <= 0) return;
        out.reserve(out.size() + n * (size_t)L_);
        for (size_t i = 0; i < n; i++) {
            pos_ = (pos_ + 1) % kTaps;
            hist_[(size_t)pos_] = in[i];
            for (int p = 0; p < L_; p++) {
                std::complex<float> acc(0, 0);
                for (int j = 0; j < kTaps; j++) acc += hist_[(size_t)((pos_ - j + kTaps) % kTaps)] * h_[(size_t)(j * L_ + p)];
                if (shiftHz_ != 0) {
                    acc *= std::complex<float>((float)rot_.real(), (float)rot_.imag());
                    rot_ *= step_;
                }
                out.push_back(acc);
            }
            if ((i & 255) == 255) rot_ /= std::abs(rot_);   // keep the mixer on the unit circle
        }
    }
private:
    int L_ = 0;
    std::vector<float> h_;
    std::vector<std::complex<float>> hist_;
    int pos_ = 0;
    double shiftHz_ = 0;
    std::complex<double> step_{1, 0}, rot_{1, 0};
};

} // namespace kiwi
} // namespace dect2
