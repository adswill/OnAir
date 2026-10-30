// Inmarsat all-carriers mode, stage 3: voice plumbing for the Aero C channels (circuit-mode voice, an AMBE-family vocoder).
// What is here: a mixer with one channel per active C carrier (mute / solo per channel), a call recorder (a WAV per call, and the raw coded
// frames in a .bits file so a capture is not wasted when the codec is missing), and the user-installed codec (plug-in id "aeroambe", codec_registry.h:
// OnAir ships no patented codec; docs/CODECS.md). Without the codec the output is silence and status() says what to install.
// What feeds pushFrame(): the C-channel decoders of the manager (inmsat_manager.h, AeroDemod at 8400 bit/s: aero_demod.h, aero_phy.h), one per
// wide carrier, 25 coded voice frames of 12 bytes per 0.5 s frame. Unencrypted traffic only; there is no key handling and no decryption here.
// Thread-safe: pushFrame() comes from the receiver thread, mix() from the audio thread.
#pragma once
#include "dect2/codec/speech_codec.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::inmsat {

struct VoiceChannelInfo {
    int id = 0;                   // the carrier id of the detector
    double freqHz = 0;
    bool active = false;          // a call is going (a frame in the last second)
    bool mute = false, solo = false;
    uint64_t frames = 0, decoded = 0;
    float level = 0;              // last output level 0..1
    double callStart = 0, lastFrame = 0;   // stream seconds
    std::string recording;        // WAV path of the current call ("" when not recording or no codec)
    std::string rawFile;          // .bits path of the current call
};

struct VoiceStatus {
    bool codecReady = false;
    std::string message;          // what to show: "" when the codec works, else the plain-words install help
    int activeCalls = 0;
    std::vector<VoiceChannelInfo> channels;
};

class VoiceMixer {
public:
    using DecoderFactory = std::function<std::unique_ptr<codec::SpeechCodec>()>;
    VoiceMixer();
    ~VoiceMixer();
    // factory: a decoder per channel; default pluginVocoder("aeroambe"). codecFolder is only used in the install message.
    void setDecoderFactory(DecoderFactory f);
    void setCodecFolder(const std::string& folder);
    void setRecording(const std::string& dir);          // "" = off. A call is written to dir/inmarsat_<kHz>_<n>.wav (and .bits)
    void setMute(int id, bool on);
    void setSolo(int id, bool on);
    void pushFrame(int id, double freqHz, const std::vector<uint8_t>& coded, double timeSec);
    void endIdle(double timeSec);                       // closes calls with no frame for 3 s (files are finished)
    size_t mix(int16_t* out, size_t n);                 // 8 kHz mono, mixed with mute / solo; silence when nothing plays
    VoiceStatus status() const;
    int sampleRate() const { return 8000; }
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::inmsat
