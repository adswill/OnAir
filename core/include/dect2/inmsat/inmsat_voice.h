// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat all-carriers mode: voice of the Aero C channels (circuit-mode voice, an AMBE-family vocoder).
// What is here: the C-channel receiver feeds pushCFrame() with the coded voice frames of every 500 ms C-channel frame (aero_cchan.h: 25 frames of
// 12 bytes, found and decoded after JAERO); a mixer with one channel per active C carrier (mute / solo per channel), a call recorder (a WAV per call,
// and the raw coded frames in a .bits file so a capture is not wasted when the codec is missing), a call list, and the user-installed codec
// (plug-in id "ambe", codec_registry.h): OnAir ships no patented codec; docs/CODECS.md. Without the codec the output is silence and status() says what
// to install. Verified so far: the framing and decoding against a real recording from JAERO's sample set and synthetic signals; no real call has been
// played through a codec yet. Unencrypted traffic only; there is no key handling and no decryption here.
// Thread-safe: pushFrame() comes from the receiver thread, mix() from the audio thread.
#pragma once
#include "dect2/codec/speech_codec.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::inmsat {

// what the C-channel receiver knows about one 500 ms frame
struct CFrameInfo {
    int id = 0;                   // the carrier id of the detector
    double freqHz = 0;
    double timeSec = 0;           // stream seconds
    double wallUnix = 0;          // wall clock, for the call list
    bool haveCall = false;        // this frame holds a call-progress signal unit
    uint32_t aesId = 0;           // aircraft (24-bit address), from the call-progress unit
    uint8_t gesId = 0;            // ground station
    float ber = 0;                // channel bit error rate before decoding (re-encoded decisions)
    float ebn0Db = 0;
    int uwErrors = 0;
    bool voiceUsed = true;        // false: all 25 voice frames identical (an idle channel)
};

struct VoiceChannelInfo {
    int id = 0;                   // the carrier id of the detector
    double freqHz = 0;
    uint32_t aesId = 0;           // 0 until a call-progress unit was heard on this call
    uint8_t gesId = 0;
    float ber = 0, ebn0Db = 0;    // last frame
    uint64_t cFrames = 0;         // 500 ms frames of this call
    double callWall = 0;          // wall clock at the call start
    bool active = false;          // a call is going (a frame in the last second)
    bool mute = false, solo = false;
    uint64_t frames = 0, decoded = 0;
    float level = 0;              // last output level 0..1
    double callStart = 0, lastFrame = 0;   // stream seconds
    std::string recording;        // WAV path of the current call ("" when not recording or no codec)
    std::string rawFile;          // .bits path of the current call
};

struct CallRecord {              // a finished call
    int id = 0;
    double freqHz = 0, startWall = 0, endWall = 0;
    uint32_t aesId = 0;
    uint8_t gesId = 0;
    uint64_t frames = 0, decoded = 0;
    float avgBer = 0;
    std::string recording, rawFile;
};

struct VoiceStatus {
    bool codecReady = false;
    std::string message;          // what to show: "" when the codec works, else the plain-words install help
    int activeCalls = 0;
    std::vector<VoiceChannelInfo> channels;
    std::vector<CallRecord> history;   // newest last, up to 100
};

class VoiceMixer {
public:
    using DecoderFactory = std::function<std::unique_ptr<codec::SpeechCodec>()>;
    VoiceMixer();
    ~VoiceMixer();
    // factory: a decoder per channel; default pluginVocoder("ambe"). codecFolder is only used in the install message.
    void setDecoderFactory(DecoderFactory f);
    void setCodecFolder(const std::string& folder);
    void setRecording(const std::string& dir);          // "" = off. A call is written to dir/inmarsat_<kHz>_<n>.wav (and .bits)
    void setMute(int id, bool on);
    void setSolo(int id, bool on);
    void pushFrame(int id, double freqHz, const std::vector<uint8_t>& coded, double timeSec);   // one coded voice frame (12 bytes for an Aero C channel)
    // a whole C-channel frame: the call details, then its 25 coded voice frames of 12 bytes (voice300) in time order
    void pushCFrame(const CFrameInfo& info, const uint8_t* voice300);
    void endIdle(double timeSec);                       // closes calls with no frame for 3 s (files are finished)
    size_t mix(int16_t* out, size_t n);                 // 8 kHz mono, mixed with mute / solo; silence when nothing plays
    VoiceStatus status() const;
    int sampleRate() const { return 8000; }
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::inmsat
