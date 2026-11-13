// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Connects the TETRA unencrypted voice path (tetra_voice.h) to a user-installed speech codec (codec/speech_codec.h).
// OnAir bundles no speech codec: the codec is a plug-in the user installs, loaded only after the legal notice is accepted
// (that gate runs in the app before setCodec() is ever called). Encrypted or unknown-state calls never reach this class,
// because processTrafficBurst() refuses them before it touches a burst.
#pragma once
#include "dect2/codec/speech_codec.h"
#include "dect2/tetra/tetra_voice.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::tetra {

// Turns the 432 descrambled type-4 bits of a traffic burst (one bit per byte, still interleaved and channel coded) into the speech frames the codec
// plug-in takes: one or more frames of kTchSFrameBits (137) bytes each, one bit per byte in the order B1..B137 of EN 300 395-2 table 4, frames
// back to back (see tetra_traffic.h: makeTrafficFrameDecoder() is the decoder written from clause 5.5). Empty result: the burst is dropped.
// A result whose size is a multiple of 137 is split into frames and the plug-in is called once per frame; any other size is passed as one frame.
using TrafficFrameDecoder = std::function<std::vector<uint8_t>(const uint8_t* bits, size_t n)>;

class TetraVoiceSink final : public SpeechFrameSink {
public:
    using PcmOut = std::function<void(const int16_t* pcm, size_t n, int sampleRate)>;

    void setPcmOut(PcmOut f);
    void setFrameDecoder(TrafficFrameDecoder f);
    // The loaded codec (null: none; frames are then dropped and no audio is made). Call only after the notice gate said Ready.
    void setCodec(std::unique_ptr<codec::SpeechCodec> c);
    bool hasCodec() const;

    // Text for the TETRA panel about the voice output.
    std::string status() const;
    uint64_t framesDecoded() const;

    void onCodedFrame(const uint8_t* bits, size_t n) override;
    // A frame of the followed timeslot without a clear burst (SpeechFrameSink): one burst's worth (60 ms) of silence goes out, so that the pauses
    // and the losses of the air keep their length. A burst whose channel decoding fails, or that the codec gives nothing for, is treated the same.
    void onMissingBurst() override;
    // Called once per capture after its frames (SpeechFrameSink); forwarded to the function given here, on the receiver's thread like PcmOut
    void onCaptureEnd() override;
    void setCaptureEnd(std::function<void()> f);

private:
    void silence(double sec);   // mu_ held: PCM of zeros at the codec's rate (nothing without a codec or an output)
    mutable std::mutex mu_;
    std::unique_ptr<codec::SpeechCodec> codec_;
    PcmOut out_;
    std::function<void()> captureEnd_;
    TrafficFrameDecoder dec_;
    uint64_t frames_ = 0;
    uint64_t dropped_ = 0;
};

// The speech comes a capture (captureSec of air, TetraReceiver::kCaptureSec) at a time, all frames of a capture in one run. A call that
// begins in the middle of a capture gives only its tail, and the next piece comes a whole capture later: played at once, the sound ran
// dry before that piece arrived, a hole of up to 2 s a few seconds into every call. So the audio that starts from a silent output is held
// until the capture is decoded (captureEnd), then written with silence in front of it: as much as the capture lacked, plus kCushionSec for
// the capture-to-capture variation of the decoding time. Silence at the head of the held audio (a pause before the first burst, a capture
// lost before this one and filled with silence) only delays a stream that starts here: as much of it goes as keeps a capture's worth plus
// the cushion in front of the next capture. While the sound plays, the audio goes straight through. Samples are interleaved float,
// `channels` per frame, at `sampleRate`.
class CaptureLeadIn {
public:
    static constexpr double kCushionSec = 0.6;
    CaptureLeadIn(int sampleRate, int channels, double captureSec) : rate_(sampleRate), ch_(channels), capture_(captureSec) {}
    // Audio of one frame. idle: the output plays nothing and has nothing queued. out receives what is to be written now (nothing while held).
    void push(const float* samples, size_t frames, bool idle, std::vector<float>& out);
    // The capture is decoded: the held audio with its lead goes to out (nothing when nothing is held)
    void captureEnd(std::vector<float>& out);
    bool holding() const { return holding_; }
    double heldSec() const { return (double)(held_.size() / (size_t)ch_) / rate_; }
private:
    int rate_, ch_;
    double capture_;
    bool holding_ = false;
    std::vector<float> held_;
};

} // namespace dect2::tetra
