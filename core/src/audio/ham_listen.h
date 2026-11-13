// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The sound of the ham decoders (HF digital, PSK, JS8Call): the upper sideband as 8 kHz audio, and "Listen", which plays that audio on
// the sound card. Listening is off unless switched on; when on it only copies the samples into the sound card's lock-free ring (audio
// the device has not taken in time is dropped), so the decoders never wait for it.
#pragma once
#include "dect2/audio/audioout.h"
#include "dect2/audio/hf_demod.h"
#include "dect2/marine/marine_dsp.h"
#include "../hfdig/hfdig_usb.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace dect2 {
namespace hfdig {

// The sideband (or FM / AM) audio: 24 kHz complex (the dial frequency at 0 Hz) -> real audio at 8 kHz.
// USB (the default): 200 .. 3800 Hz above the dial, with a slow AGC. LSB: the same below the dial. NFM: a 12.5 kHz channel, the
// frequency discriminator (5 kHz deviation gives 0.5, no de-emphasis: SSTV and AFSK data are sent flat) and a 300 .. 3000 Hz audio
// band. AM: a 7 kHz channel, the envelope over its carrier (the demodulator of the general receiver, general_rx.cpp), 300 .. 3000 Hz.
// setDemod() (HfDemod, dect2/audio/hf_demod.h; Auto is resolved by the caller) from the thread that calls process().
class UsbAudio {
public:
    static constexpr int kTaps = 383;          // about 270 Hz of transition at 24 kHz: the lower sideband is gone 200 Hz below the dial
    static constexpr int kDecim = 3;           // 24 kHz -> 8 kHz
    static constexpr int kChanTaps = 95, kAudTaps = 255;
    UsbAudio() { setDemod(HfDemodUsb); }
    int demod() const { return demod_; }
    void setDemod(int d) {
        if (d < HfDemodUsb || d > HfDemodAm) d = HfDemodUsb;
        if (d == demod_) return;
        demod_ = d;
        const double fs = marine::kBaseRate;
        if (d == HfDemodUsb || d == HfDemodLsb) {
            const auto h = usbTaps(kTaps, fs);
            taps_.resize(kTaps);
            for (int j = 0; j < kTaps; j++) {   // the history is kept oldest first; LSB: the mirror image
                const cf32 v = h[(size_t)(kTaps - 1 - j)];
                taps_[(size_t)j] = d == HfDemodLsb ? std::conj(v) : v;
            }
            hist_.assign(2 * kTaps, cf32(0.f, 0.f));
        } else {
            const auto lp = lowpass(kChanTaps, (d == HfDemodNfm ? 6250.0 : 3500.0) / fs, 7.0);   // symmetric: no reversal needed
            taps_.assign(lp.begin(), lp.end());
            hist_.assign(2 * kChanTaps, cf32(0.f, 0.f));
            // 300 .. 3000 Hz band-pass (real), evaluated every kDecim-th sample
            const auto a = lowpass(kAudTaps, 0.5 * (3000.0 - 300.0) / fs, 7.0);
            const double w0 = 2 * kPi * 0.5 * (3000.0 + 300.0) / fs, c = (kAudTaps - 1) * 0.5;
            aud_.resize(kAudTaps);
            for (int k = 0; k < kAudTaps; k++) aud_[(size_t)k] = (float)(2 * a[(size_t)k] * std::cos(w0 * (k - c)));
            ahist_.assign(2 * kAudTaps, 0.f);
        }
        reset();
    }
    void reset() {
        std::fill(hist_.begin(), hist_.end(), cf32(0.f, 0.f)); std::fill(ahist_.begin(), ahist_.end(), 0.f);
        pos_ = apos_ = 0; phase_ = 0; env_ = 0; prev_ = cf32(0.f, 0.f); car_ = 0; carInit_ = false;
    }
    // appends to out; adds |band|^2 of every output sample to power (before the AGC)
    void process(const cf32* x, size_t n, std::vector<float>& out, double& power) {
        if (demod_ == HfDemodNfm || demod_ == HfDemodAm) { processFmAm(x, n, out, power); return; }
        for (size_t i = 0; i < n; i++) {
            hist_[(size_t)pos_] = x[i]; hist_[(size_t)(pos_ + kTaps)] = x[i];   // twice, so the window never wraps
            if (++pos_ >= kTaps) pos_ = 0;
            if (++phase_ < kDecim) continue;
            phase_ = 0;
            const cf32* w = &hist_[(size_t)pos_];   // the last kTaps samples, oldest first
            float ar = 0, ai = 0;
            for (int k = 0; k < kTaps; k++) {
                const cf32 v = w[k], t = taps_[(size_t)k];
                ar += t.real() * v.real() - t.imag() * v.imag();
                ai += t.real() * v.imag() + t.imag() * v.real();
            }
            const float mag2 = ar * ar + ai * ai;
            power += mag2;
            const float mag = std::sqrt(mag2);
            env_ += (mag > env_ ? 0.01f : 0.0001f) * (mag - env_);   // fast attack, slow release
            const float g = 0.5f / (env_ + 1e-9f);
            out.push_back(std::max(-1.f, std::min(1.f, ar * g)));
        }
    }
private:
    void processFmAm(const cf32* x, size_t n, std::vector<float>& out, double& power) {
        const int T = kChanTaps, A = kAudTaps;
        const bool fm = demod_ == HfDemodNfm;
        const double fs = marine::kBaseRate;
        const float fmK = (float)(fs / (2 * kPi) / 5000.0 * 0.5);   // 5 kHz deviation -> 0.5
        const float carK = (float)(1 - std::exp(-1.0 / (0.05 * fs)));
        for (size_t i = 0; i < n; i++) {
            hist_[(size_t)pos_] = x[i]; hist_[(size_t)(pos_ + T)] = x[i];
            if (++pos_ >= T) pos_ = 0;
            const cf32* w = &hist_[(size_t)pos_];
            float zr = 0, zi = 0;
            for (int k = 0; k < T; k++) { const float t = taps_[(size_t)k].real(); zr += t * w[k].real(); zi += t * w[k].imag(); }
            const cf32 z(zr, zi);
            float s;
            if (fm) {   // the discriminator, as in the general receiver
                const cf32 d = z * std::conj(prev_);
                prev_ = z;
                s = std::atan2(d.imag(), d.real()) * fmK;
            } else {    // AM: the envelope over its carrier
                const float e = std::abs(z);
                if (!carInit_) { car_ = e; carInit_ = true; }
                car_ += carK * (e - car_);
                s = (e - car_) / std::max(car_, 1e-9f) * 0.5f;
            }
            ahist_[(size_t)apos_] = s; ahist_[(size_t)(apos_ + A)] = s;
            if (++apos_ >= A) apos_ = 0;
            if (++phase_ < kDecim) continue;
            phase_ = 0;
            const float* v = &ahist_[(size_t)apos_];
            float y = 0;
            for (int k = 0; k < A; k++) y += aud_[(size_t)k] * v[k];   // symmetric taps
            power += std::norm(z);
            out.push_back(std::max(-1.f, std::min(1.f, y)));
        }
    }
    std::vector<cf32> taps_, hist_;
    std::vector<float> aud_, ahist_;
    int demod_ = -1;
    int pos_ = 0, apos_ = 0, phase_ = 0;
    float env_ = 0;
    cf32 prev_{0.f, 0.f};
    float car_ = 0;
    bool carInit_ = false;
};

// 8 kHz mono -> 48 kHz stereo for the sound card (polyphase interpolation by 6, pass band up to 3.6 kHz)
class Upsample6 {
public:
    static constexpr int kUp = 6, kPerPhase = 24;
    Upsample6() {
        const auto lp = lowpass(kUp * kPerPhase, 3600.0 / (kUp * 8000.0), 7.0);
        for (int p = 0; p < kUp; p++)
            for (int k = 0; k < kPerPhase; k++) taps_[p][k] = (float)(kUp * lp[(size_t)(p + kUp * k)]);
        reset();
    }
    void reset() { for (auto& v : hist_) v = 0; pos_ = 0; }
    void process(const float* x, size_t n, std::vector<float>& stereo) {
        for (size_t i = 0; i < n; i++) {
            hist_[pos_] = x[i]; hist_[pos_ + kPerPhase] = x[i];
            if (++pos_ >= kPerPhase) pos_ = 0;
            const float* w = &hist_[pos_];   // oldest first: w[kPerPhase - 1] is x[i]
            for (int p = 0; p < kUp; p++) {
                float s = 0;
                for (int k = 0; k < kPerPhase; k++) s += taps_[p][k] * w[kPerPhase - 1 - k];
                stereo.push_back(s); stereo.push_back(s);
            }
        }
    }
private:
    float taps_[kUp][kPerPhase] = {};
    float hist_[2 * kPerPhase] = {};
    int pos_ = 0;
};

// "Listen": the setters from any thread, play() / playBase() / reset() from the receiver's feed thread
class Listen {
public:
    std::atomic<bool> on{false}, silent{false}, muted{false};
    std::atomic<float> volume{1.f};

    void setTap(std::function<void(const float*, size_t)> cb) { std::lock_guard<std::mutex> lk(mu_); tapSet_ = std::move(cb); tapReq_ = true; }
    void reset() { usb_.reset(); up_.reset(); if (out_) out_->flush(); }
    // 8 kHz audio the receiver already has
    void play(const float* x, size_t n) {
        if (!check() || n == 0) return;
        if (tap_) tap_(x, n);
        if (silent.load()) return;
        pcm_.clear();
        up_.process(x, n, pcm_);
        if (!out_) { out_ = std::make_unique<AudioOut>(); out_->start(48000); out_->setStartThreshold(48000 / 10); }
        out_->setVolume(volume.load()); out_->setMuted(muted.load());
        if (out_->bufferedFrames() > 48000 / 2) out_->flush();   // the radio's clock runs ahead of the sound card: catch up
        if (!out_->playing()) out_->setStartThreshold(48000 / 10);   // after an underrun (a stop, a retune) start again at 0.1 s: the default 0.8 s is above the 0.5 s kept here
        out_->write(pcm_.data(), (int)(pcm_.size() / 2));
    }
    // the 24 kHz complex baseband (dial at 0 Hz): the sideband is only worked out while listening
    void playBase(const cf32* x, size_t n) {
        if (!check()) return;
        aud_.clear();
        double pw = 0;
        usb_.process(x, n, aud_, pw);
        play(aud_.data(), aud_.size());
    }
private:
    bool check() {   // listening now? (a fresh start when it was just switched on, the queued sound gone when off)
        if (tapReq_.exchange(false)) { std::lock_guard<std::mutex> lk(mu_); tap_ = tapSet_; }
        const bool o = on.load();
        if (o != was_) { was_ = o; reset(); }
        return o;
    }
    std::mutex mu_;
    std::function<void(const float*, size_t)> tapSet_, tap_;
    std::atomic<bool> tapReq_{false};
    bool was_ = false;
    UsbAudio usb_;
    Upsample6 up_;
    std::vector<float> aud_, pcm_;
    std::unique_ptr<AudioOut> out_;
};

} // namespace hfdig
} // namespace dect2
