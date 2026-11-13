// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu System Fusion receiver (WIP): C4FM 4800 symbols/s in a 12.5 kHz channel, frame sync D471C9634D, FICH, header / communication /
// terminator frames, callsigns and slow data. Written from Yaesu's public Amateur Radio Digital Specs (revision 1.01).
//
// The channel is mixed to 0 Hz, decimated and resampled to 48 kHz (10 samples per symbol), FM-demodulated and smoothed. Frames are found
// by correlation with the 20 sync symbols; levels and offset are fitted from the sync and then from the frame's decided symbols (a
// mirrored spectrum is accepted), and a frame counts when its FICH checks (Golay words and CRC-16). The next frame is looked for 100 ms
// later. Callsigns and slow data come from the DCH units that pass their CRC-16.
//
// Voice: no speech codec is in OnAir. The voice sub-frames go to a user-installed codec plug-in (codec id "ysf", dect2/codec/speech_codec.h)
// when one is set with setSpeechCodec(); the frame is handed over as it was sent (V/D mode 1: 9 bytes = the 72 channel bits, V/D mode 2:
// 7 bytes = the 49 voice bits after the majority vote, voice FR: 18 bytes = the 144 channel bits). Without a codec everything else is shown
// and there is no sound. Nothing is encrypted in this system and nothing is decrypted here.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/codec/speech_codec.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
namespace voice { class Monitor; }

struct FusionTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 no input, 1 listening, 2 a transmission is being received
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;         // locked on a transmission
    uint64_t blocksOk = 0;          // frames whose FICH checked
    uint64_t blocksBad = 0;         // sync found but the FICH did not check
    uint64_t headers = 0, comms = 0, terminators = 0, tests = 0;
    uint64_t dchOk = 0, dchBad = 0;
    uint64_t voiceSubFrames = 0;    // voice sub-frames received
    uint64_t voiceDecoded = 0;      // ... that the codec turned into sound
    bool inverted = false;          // the spectrum was mirrored
    // the transmission being received (or the last one)
    int frameType = -1;             // FI of the last frame: 0 header, 1 communication, 2 terminator, 3 test
    int dataType = -1;              // DT: 0 V/D mode 1, 1 data FR, 2 V/D mode 2, 3 voice FR
    int callMode = -1;              // CM: 0 group / CQ, 3 individual
    int routing = -1;               // MR: 0 direct, 1 downlink (uplink idle), 2 downlink (uplink busy)
    int voip = -1;                  // 0 local, 1 via internet
    int squelch = -1;               // 0 off, 1..126 code, 127 break-out
    int fn = 0, ft = 0, bn = 0, bt = 0;
    std::string dest, src, down, up;            // the callsign fields (empty: not received yet)
    std::string rem[4];                         // supplementary fields (radio IDs), printable or hex
    std::string slowText;                       // the slow data bytes received, printable characters only
    bool hasGps = false; double gpsLat = 0, gpsLon = 0; std::string radioModel;   // GPS block of the slow data
    bool haveCsd[3] = {false, false, false};    // CSD1 (dest, src), CSD2 (down, up), CSD3 (rem) seen
    // voice
    bool codecReady = false;
    std::string codecText;          // what the voice needs, shown while no codec is loaded
    double inputRate = 0;
    double centerHz = 0;
};

class FusionReceiver {
public:
    FusionReceiver();
    ~FusionReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(FusionTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    // The user-installed speech codec (nullptr: none). Takes ownership; the text is what the panel shows while there is none.
    void setSpeechCodec(std::unique_ptr<codec::SpeechCodec> c, const std::string& text = std::string());
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                                        // decode but do not open the sound device
    void setVoiceMonitor(voice::Monitor* m);   // the monitor that picks and records the call (voice_monitor.h); default voice::monitor("ysf") while not silent
    void setAudioTap(std::function<void(const float* x, size_t n, int sampleRate)> cb);   // tests: the decoded voice

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning fusionTuning();
std::string fusionSummary(const FusionTelemetry& t);

} // namespace dect2
