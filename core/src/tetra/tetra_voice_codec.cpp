// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tetra/tetra_voice_codec.h"
#include <algorithm>

namespace dect2::tetra {

void TetraVoiceSink::setPcmOut(PcmOut f) { std::lock_guard<std::mutex> lk(mu_); out_ = std::move(f); }
void TetraVoiceSink::setCaptureEnd(std::function<void()> f) { std::lock_guard<std::mutex> lk(mu_); captureEnd_ = std::move(f); }
void TetraVoiceSink::onCaptureEnd() { std::lock_guard<std::mutex> lk(mu_); if (captureEnd_) captureEnd_(); }
void TetraVoiceSink::setFrameDecoder(TrafficFrameDecoder f) { std::lock_guard<std::mutex> lk(mu_); dec_ = std::move(f); }

void CaptureLeadIn::push(const float* samples, size_t frames, bool idle, std::vector<float>& out) {
    if (!holding_ && idle) { holding_ = true; held_.clear(); }
    if (holding_) { held_.insert(held_.end(), samples, samples + frames * (size_t)ch_); return; }
    out.insert(out.end(), samples, samples + frames * (size_t)ch_);
}

void CaptureLeadIn::captureEnd(std::vector<float>& out) {
    if (!holding_) return;
    // silence at the head of the held audio (a pause before the first burst, or a capture lost before this one and filled with silence)
    // only delays a stream that starts here: as much of it goes as keeps a capture's worth plus the cushion in front of the next capture
    size_t zeros = 0;
    while (zeros < held_.size() && held_[zeros] == 0.f) zeros++;
    const size_t frame = (size_t)ch_;
    const size_t strip = std::min(zeros / frame, (size_t)std::max(0.0, (heldSec() - capture_) * rate_)) * frame;
    const double lead = kCushionSec + std::max(0.0, capture_ - heldSec() + (double)(strip / frame) / rate_);
    out.insert(out.end(), (size_t)(lead * rate_) * frame, 0.f);
    out.insert(out.end(), held_.begin() + (std::ptrdiff_t)strip, held_.end());
    held_.clear();
    holding_ = false;
}
void TetraVoiceSink::setCodec(std::unique_ptr<codec::SpeechCodec> c) { std::lock_guard<std::mutex> lk(mu_); codec_ = std::move(c); }
bool TetraVoiceSink::hasCodec() const { std::lock_guard<std::mutex> lk(mu_); return codec_ != nullptr; }
uint64_t TetraVoiceSink::framesDecoded() const { std::lock_guard<std::mutex> lk(mu_); return frames_; }

std::string TetraVoiceSink::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!codec_) return "speech codec not installed: no audio";
    if (!dec_) return "traffic channel decoder not set: no audio";
    return "speech codec ready";
}

constexpr double kBurstSec = 0.06;   // the speech of one TCH/S burst: two 30 ms frames

void TetraVoiceSink::silence(double sec) {
    if (!codec_ || !out_) return;
    const int rate = codec_->sampleRate();
    if (rate <= 0 || sec <= 0) return;
    const std::vector<int16_t> z((size_t)(rate * sec + 0.5), 0);
    out_(z.data(), z.size(), rate);
}

void TetraVoiceSink::onMissingBurst() {
    std::lock_guard<std::mutex> lk(mu_);
    silence(kBurstSec);
}

void TetraVoiceSink::onCodedFrame(const uint8_t* bits, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!codec_ || !dec_ || !out_ || !bits) { dropped_++; return; }
    const std::vector<uint8_t> frames = dec_(bits, n);
    if (frames.empty()) { dropped_++; silence(kBurstSec); return; }
    constexpr size_t kFrame = 137;   // kTchSFrameBits: a TCH/S burst holds two frames
    const bool split = frames.size() > kFrame && frames.size() % kFrame == 0;
    const size_t step = split ? kFrame : frames.size();
    const size_t count = frames.size() / step;
    for (size_t off = 0; off < frames.size(); off += step) {
        const std::vector<uint8_t> frame(frames.begin() + (std::ptrdiff_t)off, frames.begin() + (std::ptrdiff_t)(off + step));
        const std::vector<int16_t> pcm = codec_->decode(frame);
        if (pcm.empty()) { dropped_++; silence(kBurstSec / (double)count); continue; }
        frames_++;
        out_(pcm.data(), pcm.size(), codec_->sampleRate());
    }
}

} // namespace dect2::tetra
