// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "cdr_audio.h"
#include "dect2/cdr/cdr_dra.h"
#include <algorithm>

namespace dect2 {

namespace {
constexpr int kStuckUnits = 80;      // units in (about 1.7 s of sound) without a single sound frame out: the decoder is rebuilt
constexpr int kDeviceRetry = 40;     // sub-frames between attempts to open a sound device that failed

// The DRA frame at the start of `p` (0x7FFF sync, then the header's frame size in 32-bit words), its length in bytes; 0 if there is none.
size_t draFrameBytes(const uint8_t* p, size_t n) {
    if (n < 6 || p[0] != 0x7F || p[1] != 0xFF) return 0;
    const uint32_t v = (uint32_t)p[2] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5];
    const size_t words = (v >> 31) ? (v >> 17) & 0x1FFF : (v >> 21) & 0x3FF;
    const size_t bytes = words * 4;
    return words >= 2 && bytes <= n ? bytes : 0;
}
}

CdrAudio::CdrAudio(bool headless) : headless_(headless) {}
CdrAudio::~CdrAudio() { if (dra_ && api_) api_->destroy(dra_); }

void CdrAudio::resetDecoder() {
    if (dra_ && api_) api_->destroy(dra_);
    dra_ = nullptr;
    playingId_ = -1;
    unitsSincePcm_ = 0;
}
void CdrAudio::resetAll() { resetDecoder(); lastListen_ = -1; flush(); }
void CdrAudio::flush() { if (out_) out_->flush(); }

void CdrAudio::play(int want, uint16_t id, const std::vector<cdr::AudioUnit>& units, float volume, bool muted, CdrTelemetry& tel) {
    if (want == 0 || (int)id != want || units.empty()) return;
    if (lastListen_ != want || playingId_ != (int)id) {      // another service: nothing of the old one may reach the new sound
        resetDecoder();
        playingId_ = (int)id;
        lastListen_ = want;
        flush();
    }
    if (!dra_) {
        api_ = cdr::draApi();         // the add-on may have been installed while running ("Look again")
        if (!api_) {                  // no decoder: the units are only counted; the receiver says why (CdrTelemetry::draFound)
            for (const auto& u : units) if (!u.data.empty()) tel.draUnitsIn++;
            return;
        }
        dra_ = api_->create();
    }
    if (!dra_) return;
    for (const auto& u : units) {
        if (u.data.empty()) continue;
        tel.draUnitsIn++;
        // A unit is one DRA frame followed by extension bytes (the frame size in its header is shorter than the unit). The decoder would scan
        // those bytes for the next sync word, and they hold 0x7FFF now and then: it then reads a made-up frame over the start of the real next
        // one and drops it (about 1 unit in 10 on the Guangzhou and 89.3 MHz recordings: a click). Only the frames themselves go in.
        bool sent = true;
        {
            const uint8_t* d = u.data.data();
            const size_t n = u.data.size();
            size_t at = 0, fed = 0;
            for (size_t fb; (fb = draFrameBytes(d + at, n - at)) != 0; at += fb, fed++)
                if (api_->send(dra_, const_cast<unsigned char*>(d + at), (int)fb) != 0) { sent = false; break; }
            if (!fed && sent) sent = api_->send(dra_, const_cast<unsigned char*>(d), (int)n) == 0;      // no header found: the decoder decides
        }
        if (!sent) { tel.draUnitsBad++; continue; }
        if (++unitsSincePcm_ > kStuckUnits) {                // fed and fed, nothing comes out: a latched decoder (bad header, odd frame size)
            const int keep = playingId_;
            resetDecoder();
            playingId_ = keep;
            dra_ = api_->create();
            if (!dra_) return;
            tel.draResets++;
            continue;
        }
        for (;;) {
            unsigned char* pcm = nullptr;
            int len = 0;
            if (api_->recv(dra_, &pcm, &len) != 0) break;
            cdr::DraFrameInfo info;
            if (api_->info(dra_, &info) != 0 || info.nSampleRate < 8000 || info.nChannels < 1 || !pcm || len < 4) continue;
            // the decoder returns one frame of 1024 samples, mono or an interleaved stereo pair (a 5.1 stream is mixed down to the pair)
            const int ch = std::min(info.nChannels, 2);
            const int frames = std::min(len / (2 * ch), 1024);
            if (frames < 1) continue;
            unitsSincePcm_ = 0;
            tel.draPcmFrames += (uint64_t)frames;
            tel.draRateHz = info.nSampleRate;
            if (headless_) continue;
            if (!out_ || rate_ != info.nSampleRate) {
                out_ = std::make_unique<AudioOut>();
                out_->start(info.nSampleRate);
                out_->setStartThreshold(info.nSampleRate / 10);
                rate_ = info.nSampleRate;
            } else if (!out_->running() && ++deviceTries_ >= kDeviceRetry) {      // the device was busy or went away: try again
                deviceTries_ = 0;
                out_->start(info.nSampleRate);
            }
            out_->setVolume(volume);
            out_->setMuted(muted);
            // DRA+ PCM arrives in ~640 ms logical-frame bursts; do not flush around 0.5 s or playback drops every frame.
            if (!out_->playing()) out_->setStartThreshold(rate_ / 10);
            if (out_->bufferedFrames() > rate_ * 3 / 2) out_->flush();   // catch up when the radio clock runs ahead of the sound card
            std::vector<float> st((size_t)frames * 2);
            const int16_t* s = reinterpret_cast<const int16_t*>(pcm);
            for (int i = 0; i < frames; i++) {
                const float L = s[i * ch] / 32768.f;
                st[(size_t)i * 2] = L;
                st[(size_t)i * 2 + 1] = ch > 1 ? s[i * ch + 1] / 32768.f : L;
            }
            out_->write(st.data(), frames);
        }
    }
}

} // namespace dect2
