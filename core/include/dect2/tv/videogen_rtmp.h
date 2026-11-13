// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RTMP server input of the digital video generator (Source::Rtmp, adswill/OnAir#490): OBS Studio, or any other program or PC that
// streams RTMP, pushes its stream to this PC (rtmp://<this PC>:1935/live, stream key "stream") and the generator encodes its picture and
// sound like a camera's. Nothing else is needed on this PC: FFmpeg's own RTMP protocol in listen mode is the server (handshake, connect,
// publish), the stream is FLV (H.264 or HEVC picture, AAC or MP3 sound). One sender at a time; when it stops, the input listens again.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2::vgen {

constexpr const char* kRtmpDefaultUrl = "rtmp://0.0.0.0:1935/live/stream";
constexpr int kRtmpDefaultPort = 1935;

// The address to listen on, "rtmp://[address][:port]/app[/stream key]": address 0.0.0.0 (or empty) = every network card of this PC, port
// 1935 when none is given. False with err when it is not such a URL. listenUrl is what FFmpeg opens.
bool parseRtmpListenUrl(const std::string& url, std::string& listenUrl, int& port, std::string& err);
// What to put in OBS (Settings > Stream, Service "Custom..."): the server "rtmp://<host>:<port>/<app>" and the stream key; host is the
// address of the URL, or `thisPc` when the URL listens on every card
void rtmpObsSettings(const std::string& url, const std::string& thisPc, std::string& server, std::string& key);
// This FFmpeg can be an RTMP server (the rtmp protocol and the FLV demuxer); false with why
bool rtmpSupported(std::string& why);

class RtmpInput {
public:
    RtmpInput();
    ~RtmpInput();   // stops
    RtmpInput(const RtmpInput&) = delete;
    RtmpInput& operator=(const RtmpInput&) = delete;

    // starts listening (in its own thread); the sound comes out as stereo float at sampleRate. False with err for a bad URL or an FFmpeg
    // without RTMP; a port that is taken shows in status() (and is tried again)
    bool start(const std::string& url, int sampleRate, std::string& err);
    void stop();
    bool connected() const;     // a sender is streaming
    std::string status() const; // "Waiting for a sender on port 1935" / "Receiving 1280x720 h264 + aac" / why listening failed
    // the newest picture, newer than `serial`: YUV 4:2:0 planes at the picture's own (even) size; false when there is none newer
    bool picture(std::vector<uint8_t>& y, std::vector<uint8_t>& u, std::vector<uint8_t>& v, int& w, int& h, uint64_t& serial) const;
    // n stereo samples (2n floats) of the sender's sound, a little behind so the clocks of the two PCs can differ; silence when there is none
    void readAudio(float* stereo, int n);

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::vgen
