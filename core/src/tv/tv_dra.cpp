// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRA sound of a TV service through the user's add-on (tv_dra.h).
#include "dect2/tv/tv_dra.h"
#include <algorithm>
#include <cmath>
#include "dect2/common/diag.h"

namespace dect2 {

namespace {
constexpr int kStuckFrames = 80;          // frames in without a single sound frame out: the decoder is rebuilt (as the CDR path does)
constexpr size_t kMaxBuf = 96 * 1024;     // never keep more than this waiting for a frame that does not complete
}

std::string draTvNote(bool addonFound, const std::string& problem) {
    if (addonFound) return std::string();
    if (!problem.empty()) return "DRA audio: the DRA add-on cannot be used (" + problem + ")";
    return "DRA audio: install the DRA add-on to hear this service";
}

size_t draFrameLength(const uint8_t* p, size_t n, bool* needMore) {
    if (needMore) *needMore = false;
    if (n < 2 || p[0] != 0x7F || p[1] != 0xFF) return 0;
    if (n < 6) { if (needMore) *needMore = true; return 0; }
    const uint32_t v = (uint32_t)p[2] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5];
    const size_t words = (v >> 31) ? (v >> 17) & 0x1FFF : (v >> 21) & 0x3FF;
    const size_t bytes = words * 4;
    if (words < 2) return 0;
    if (bytes > n) { if (needMore) *needMore = true; return 0; }
    return bytes;
}

TvDraDecoder::TvDraDecoder(const cdr::DraApi* api, int outRate) : explicitApi_(api), outRate_(outRate > 0 ? outRate : 48000) {}
TvDraDecoder::~TvDraDecoder() { drop(); }

void TvDraDecoder::drop() {
    if (h_ && api_) api_->destroy(h_);
    h_ = nullptr;
}

bool TvDraDecoder::open() {
    if (h_) return true;
    api_ = explicitApi_ ? explicitApi_ : cdr::draApi();     // the add-on may have been installed while running
    if (!api_) { DIAG_DEBUG(792632, "The user-installed DRA audio add-on is not available, so DRA sound is not decoded."); return false; }
    h_ = api_->create();
    return h_ != nullptr;
}

bool TvDraDecoder::usable() { return explicitApi_ || cdr::draApi(); }

void TvDraDecoder::reset() {
    drop();
    buf_.clear();
    nextPts_ = pendPts_ = -1; pendOff_ = 0;
    sincePcm_ = 0;
    rsPos_ = 0; haveLast_ = false; lastL_ = lastR_ = 0;
}

void TvDraDecoder::emit(const int16_t* s, int ch, int frames, int rate, double pts, std::vector<DraPcm>& out) {
    std::vector<float> in((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const float L = s[i * ch] / 32768.f;
        in[(size_t)i * 2] = L;
        in[(size_t)i * 2 + 1] = ch > 1 ? s[i * ch + 1] / 32768.f : L;
    }
    DraPcm c;
    c.pts = pts; c.rate = outRate_;
    if (rate == outRate_) {
        c.stereo = std::move(in);
    } else {                                   // linear resampling, the phase carried from chunk to chunk
        if (!haveLast_) { lastL_ = in[0]; lastR_ = in[1]; rsPos_ = 0; haveLast_ = true; }
        const double step = (double)rate / outRate_;
        const double N = frames;
        c.stereo.reserve((size_t)(frames / step * 2) + 8);
        for (; rsPos_ <= N - 1; rsPos_ += step) {
            const int i = (int)std::floor(rsPos_);
            const double f = rsPos_ - i;
            const float aL = i < 0 ? lastL_ : in[(size_t)i * 2], aR = i < 0 ? lastR_ : in[(size_t)i * 2 + 1];
            const float bL = in[(size_t)(i + 1) * 2], bR = in[(size_t)(i + 1) * 2 + 1];
            c.stereo.push_back((float)(aL + (bL - aL) * f));
            c.stereo.push_back((float)(aR + (bR - aR) * f));
        }
        rsPos_ -= N;
        lastL_ = in[(size_t)(frames - 1) * 2]; lastR_ = in[(size_t)(frames - 1) * 2 + 1];
    }
    pcmFrames += c.stereo.size() / 2;
    if (!c.stereo.empty()) out.push_back(std::move(c));
}

int TvDraDecoder::feed(const uint8_t* es, size_t n, double pts, std::vector<DraPcm>& out) {
    if (!es || !n) return 0;
    const size_t before = out.size();
    // the time of the first frame that starts in this payload: remember where the payload begins in the buffer (kept until a frame uses it)
    if (pts >= 0) { pendPts_ = pts; pendOff_ = buf_.size(); }
    buf_.insert(buf_.end(), es, es + n);
    size_t at = 0;
    while (at < buf_.size()) {
        if (buf_[at] != 0x7F || (at + 1 < buf_.size() && buf_[at + 1] != 0xFF)) { at++; continue; }      // look for the sync word
        bool more = false;
        const size_t fb = draFrameLength(buf_.data() + at, buf_.size() - at, &more);
        if (!fb) {
            if (more) break;            // the frame goes on in the next payload
            at++;                       // 0x7FFF inside other data, or a bad header
            continue;
        }
        // the time of this frame: the PES time when this is the first frame of the payload that carries it (kept smooth against the running time)
        double t = nextPts_;
        if (pendPts_ >= 0 && at >= pendOff_) {
            t = (nextPts_ >= 0 && std::fabs(pendPts_ - nextPts_) < 0.04) ? nextPts_ : pendPts_;
            pendPts_ = -1;
        }
        framesIn++;
        if (!open()) { at += fb; continue; }
        if (api_->send(h_, buf_.data() + at, (int)fb) != 0) { framesBad++; at += fb; continue; }
        at += fb;
        if (++sincePcm_ > kStuckFrames) {                 // fed and fed, nothing comes out: a latched decoder
            drop();
            sincePcm_ = 0; resets++;
            continue;
        }
        for (;;) {
            unsigned char* pcm = nullptr;
            int len = 0;
            if (api_->recv(h_, &pcm, &len) != 0) break;
            cdr::DraFrameInfo info;
            if (api_->info(h_, &info) != 0 || info.nSampleRate < 8000 || info.nChannels < 1 || !pcm || len < 4) continue;
            const int ch = std::min(info.nChannels, 2);
            const int frames = len / (2 * ch);
            if (frames < 1) continue;
            sincePcm_ = 0;
            sourceRate = info.nSampleRate; sourceChannels = info.nChannels;
            emit(reinterpret_cast<const int16_t*>(pcm), ch, frames, info.nSampleRate, t, out);
            if (t >= 0) { t += (double)frames / info.nSampleRate; nextPts_ = t; }
        }
    }
    // keep only the unfinished frame (from the last sync word that may still complete)
    if (at >= buf_.size()) { buf_.clear(); pendOff_ = 0; }
    else { buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)at); pendOff_ = pendOff_ > at ? pendOff_ - at : 0; }
    if (buf_.size() > kMaxBuf) { buf_.clear(); pendPts_ = -1; pendOff_ = 0; }
    return (int)(out.size() - before);
}

} // namespace dect2
