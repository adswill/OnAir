// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound-card IQ source: WebSDR, VAC, or a radio's speaker/line into the computer (not a replacement for an SDR when you need MHz of bandwidth).
// Reuses AudioIn (CoreAudio / miniaudio). The microphone is never opened until start().
#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "dect2/audio/audioin.h"
#include "dect2/source/audio_if.h"
#include "dect2/source/audio_push.h"
#include "dect2/dsp/resampler.h"
#include "dect2/source/source.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using Clock = std::chrono::steady_clock;
using dect2::math::kPi;
constexpr int kHilN = AudioHilbert::N;
constexpr int kHilDelay = (kHilN - 1) / 2;

const float* hilbertTaps() {
    static float h[kHilN];
    static bool once = false;
    if (once) return h;
    for (int i = 0; i < kHilN; i++) {
        const int t = i - kHilDelay;
        if (t % 2 == 0) { h[i] = 0.f; continue; }   // even lags (and the centre) are zero
        const double w = 0.54 + 0.46 * std::cos(kPi * t / (kHilDelay + 1));   // Hamming
        h[i] = (float)(2.0 / (kPi * t) * w);
    }
    once = true;
    return h;
}

class AudioSource : public IqSource {
public:
    explicit AudioSource(DeviceInfo d) : dev_(std::move(d)) {}
    ~AudioSource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        stop();
        cfg_ = audioIfConfigOf(s);
        push_ = dev_.serial.rfind("push", 0) == 0;
        if (push_) {   // the platform records and pushes the frames in (audio_push.h: Android's AudioRecord); nothing is opened here
            if (audioPushRate() <= 0) { DIAG_ERROR(832227, "The sound-card input is not recording (no microphone permission?), so the audio source does not start."); err = "The sound-card input is not recording (no microphone permission?)"; return false; }
            audioPushReset();
            live_ = true;
            pace_ = 1.0;
            rate_ = audioPushRate();
            st_ = AudioIfState();
            cfg_.rate = (int)rate_;
            factor_ = audioOutFactor(rate_, cfg_.minOutRate);
            baseRate_ = rate_;
            rate_ = baseRate_ * factor_;
            ring_ = &ring;
            run_ = true;
            th_ = dect2::namedThread("onair-srcaud", [this] { loop(); });
            return true;
        }
        in_ = std::make_unique<AudioIn>();
        AudioIn::Kind k = AudioIn::Mic;
        std::string arg = dev_.serial;
        double toneHz = 1000;
        if (dev_.serial.rfind("tone", 0) == 0) {
            k = AudioIn::Tone;
            if (dev_.serial.size() > 5 && dev_.serial[4] == ':') toneHz = atof(dev_.serial.c_str() + 5);
            arg.clear();
        } else if (dev_.serial.rfind("file:", 0) == 0) {   // a WAV file as the sound card (tests, and replaying a recorded IF)
            k = AudioIn::File;
            arg = dev_.serial.substr(5);
        } else if (dev_.serial == "silence") {
            k = AudioIn::Silence;
            arg.clear();
        }
        if (!in_->start(k, cfg_.rate, arg, toneHz, err)) {
            in_.reset();
            return false;
        }
        live_ = k == AudioIn::Mic;
        pace_ = live_ ? 1.0 : std::max(0.1, std::min(200.0, radioNumber(s, "audio_pace", 1)));   // a file can play faster than real time (tests)
        rate_ = in_->sampleRate() > 0 ? in_->sampleRate() : 48000;
        st_ = AudioIfState();
        cfg_.rate = rate_;
        factor_ = audioOutFactor(rate_, cfg_.minOutRate);
        baseRate_ = rate_;
        rate_ = baseRate_ * factor_;
        ring_ = &ring;
        run_ = true;
        th_ = dect2::namedThread("onair-srcaud", [this] { loop(); });
        return true;
    }

    void stop() override {
        run_ = false;
        if (th_.joinable()) th_.join();
        if (in_) { in_->stop(); in_.reset(); }
        ring_ = nullptr;
        st_.peak = 0;
    }

    bool retune(const TuneSettings& s, std::string&) override {
        const int rate = cfg_.rate;
        AudioIfConfig c = audioIfConfigOf(s);   // the sound card is not tuned; the IQ mapping, IF offset, channel, gain and the dial change
        c.rate = rate;   // a new sample rate needs a restart
        std::lock_guard<std::mutex> lk(cfgMu_);
        cfg_ = c;
        const int f = audioOutFactor(baseRate_, c.minOutRate);   // another mode needs another rate: the engine sees the new one and restarts its receivers
        if (f != factor_) { factor_ = f; rate_ = baseRate_ * f; }
        return true;
    }
    double sampleRate() const override { std::lock_guard<std::mutex> lk(cfgMu_); return rate_; }
    bool realtimeHardware() const override { return live_; }   // a card is; a WAV file standing in for it is a recording (slots found from the signal, a slow receiver is waited for)

private:
    void loop() {
        std::vector<float> st(2048 * 2);
        std::vector<cf32> iq(2048);
        auto t0 = Clock::now();
        uint64_t sent = 0;
        while (run_ && (in_ || push_)) {
            int n = 1024;
            if (push_) {   // paced by the platform's recorder, like a live card
                n = (int)audioPushRead(st.data(), 2048);
                if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            } else if (live_) {   // paced by the card's own clock: take what has been captured, wait when there is none (no zeros inserted, no frames repeated)
                n = in_->readAvailable(st.data(), 2048);
                if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            } else in_->read(st.data(), n);
            AudioIfConfig c;
            { std::lock_guard<std::mutex> lk(cfgMu_); c = cfg_; }
            audioIfProcess(st.data(), n, c, iq.data(), st_);
            int f;
            { std::lock_guard<std::mutex> lk(cfgMu_); f = factor_; }
            const cf32* outp = iq.data();
            size_t outN = (size_t)n;
            if (f > 1) {   // a receiver that wants more samples per second than a sound card gives: interpolated up, nothing added to the signal
                if (f != upFactor_) { up_.configure(baseRate_, baseRate_ * f); up_.reset(); upFactor_ = f; }
                upOut_.clear();
                up_.process(iq.data(), (size_t)n, upOut_);
                outp = upOut_.data(); outN = upOut_.size();
            } else upFactor_ = 1;
            if (live_) {
                ring_->write(outp, outN);   // a live tap drops when the receiver is slow (the capture ring would overflow anyway)
                continue;
            }
            while (run_ && ring_->capacity() - ring_->available() < outN) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!run_) break;
            ring_->write(outp, outN);
            sent += (uint64_t)n;
            auto due = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sent / (baseRate_ * pace_)));
            std::this_thread::sleep_until(due);
        }
    }

    DeviceInfo dev_;
    std::unique_ptr<AudioIn> in_;
    bool push_ = false;   // frames come from audio_push.h instead of AudioIn
    IqRing* ring_ = nullptr;
    double rate_ = 48000;
    bool live_ = true;
    double pace_ = 1;
    AudioIfState st_;
    AudioIfConfig cfg_;
    mutable std::mutex cfgMu_;
    int factor_ = 1;
    double baseRate_ = 48000;
    RationalResampler up_;
    int upFactor_ = 1;
    std::vector<cf32> upOut_;
    std::atomic<bool> run_{false};
    std::thread th_;
};
} // namespace

void audioStereoToIq(const float* stereo, int n, AudioIqMode mode, cf32* out, AudioHilbert& h) {
    if (n <= 0) return;
    const float* ht = hilbertTaps();
    for (int i = 0; i < n; i++) {
        const float l = stereo[2 * i], r = stereo[2 * i + 1];
        if (mode == AudioIqMode::StereoIq) { out[i] = cf32(l, r); continue; }
        const float reIn = mode == AudioIqMode::MonoReal ? 0.5f * (l + r) : l;
        if (mode == AudioIqMode::MonoReal) { out[i] = cf32(reIn, 0.f); continue; }
        h.x[h.p] = reIn;
        if (++h.p >= kHilN) h.p = 0;
        float im = 0;
        int idx = h.p;   // oldest first (the slot about to be overwritten)
        for (int k = kHilN - 1; k >= 0; k--) {
            im += ht[k] * h.x[idx];
            if (++idx >= kHilN) idx = 0;
        }
        const float re = h.x[(h.p + kHilN - 1 - kHilDelay) % kHilN];
        out[i] = mode == AudioIqMode::Lsb ? cf32(re, -im) : cf32(re, im);
    }
}

std::vector<DeviceInfo> listAudioDevices() {
    std::vector<DeviceInfo> out;
    for (const AudioInputDevice& a : listAudioInputs()) {
        DeviceInfo d;
        d.kind = DeviceInfo::Audio;
        d.serial = a.id;
        d.board = "audio";
        d.name = "Sound card: " + a.name;
        d.minRateHz = 8000;
        d.maxRateHz = 192000;   // the card resamples to the rate picked (audio_rate)
        d.steadyRateHz = 48000;
        out.push_back(std::move(d));
    }
    return out;
}

std::unique_ptr<IqSource> makeAudioSource(const DeviceInfo& d) { return std::make_unique<AudioSource>(d); }

} // namespace dect2
