// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Airband: one WAV per transmission (RTLSDR-Airband, SDRTrunk). Each squelch opening of a channel is saved as a short clip named with the channel,
// the frequency and the UTC time, plus a rolling 30 s of each channel for "replay the last 30 s". Fed by the receiver with each channel's 8 kHz audio
// after the squelch (zeros while it is closed). Thread safe: the receiver feeds, the interface reads the finished clips. docs/AIRBAND-CLIPS-3.9.md.
#pragma once
#include "dect2/voice/clip_history.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct AirbandClipChannel { double freqHz = 0; std::string name; };   // name: the label or the ICAO channel name

class AirbandClipWriter {
public:
    explicit AirbandClipWriter(std::string folder, int rate = 8000);
    void setEnabled(bool on);                                  // off: nothing is written (the 30 s replay is still kept)
    bool enabled() const;
    void setChannels(const std::vector<AirbandClipChannel>& ch);   // by the receiver's channel index
    void setMinSeconds(double s) { minSec_ = s; }             // shorter openings are not saved (a click of the squelch)
    void setCloseSeconds(double s) { closeSec_ = s; }         // this much silence ends a clip
    void setClock(std::function<int64_t()> wallUtc);           // tests
    // the receiver: audio of channel `chan` (n samples at the rate), zeros while the squelch is closed
    void feed(int chan, const float* x, size_t n);
    // ends the clips that are open (receiver stopped)
    void flush();
    // the clips finished since the last call (for the clips list)
    std::vector<voice::Clip> takeFinished();
    // the last 30 s of a channel as a WAV in the folder; "" when nothing was heard or it could not be written
    std::string saveReplay(int chan, std::string& err);
    const std::string& folder() const { return folder_; }
    static std::string fileName(const std::string& channelName, double freqHz, int64_t utc);   // "airband-TOWER-118.100-20261110T221530Z.wav"
    static constexpr double kReplaySec = 30;
private:
    struct Ch {
        bool open = false;
        std::vector<float> clip;
        size_t zeros = 0;
        int64_t startUtc = 0;
        std::deque<float> ring;
    };
    void finish(int chan, Ch& c);
    std::string writeWav(const std::string& path, const std::vector<float>& x, std::string& err) const;
    mutable std::mutex mu_;
    std::string folder_;
    int rate_;
    bool enabled_ = false;
    double minSec_ = 0.4, closeSec_ = 1.0;
    std::function<int64_t()> clock_;
    std::vector<AirbandClipChannel> chans_;
    std::vector<Ch> st_;
    std::vector<voice::Clip> done_;
};

}  // namespace dect2
