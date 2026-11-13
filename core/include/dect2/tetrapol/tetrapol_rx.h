// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL receiver: GMSK 8 kbit/s, 10 / 12.5 kHz channels (PAS 0001-2). Frame sync on the header of two frames in a row, the polarity
// (the downlink is the inverse of the uplink) and the scrambling constant and band coding (VHF / UHF) are found automatically.
// Control channel: data blocks, HDLC, D_SYSTEM_INFO (cell) and D_GROUP_ACTIVATION (group calls, encryption state).
// Traffic channel: voice frames go to the user-installed codec plug-in only when the call's own signalling says it is clear
// (tetrapol_voice.h). Receive only: nothing is decrypted, no key is used, no identity is logged or saved.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/codec/speech_codec.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct TetrapolTelemetry;

class TetrapolReceiver {
public:
    TetrapolReceiver();
    ~TetrapolReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    void setSpacingHz(int hz);            // 10000 or 12500
    // 0 automatic (both are tried; the one whose frames pass their CRC is kept), 1 normal, 2 inverted
    void setPolarity(int mode);
    // 0 automatic, 1 VHF, 2 UHF frame coding
    void setBand(int mode);
    // the speech codec for clear calls (the user's plug-in); null: voice frames are counted, no sound. Without one set here, the
    // receiver asks the codec registry (codec::pluginVocoder("tetrapol")) when a clear call starts.
    void setCodec(std::unique_ptr<codec::SpeechCodec> c);
    bool hasCodec() const;
    // decoded speech of clear calls (PCM at the codec's rate, 8000 Hz for the plug-in format)
    void setPcmOut(std::function<void(const int16_t* pcm, size_t n, int rate)> cb);
    void setListen(bool on);              // off: clear calls are not decoded (no sound, no recording)
    // one WAV per clear call into this folder (empty: off). The file names hold the date and time only.
    void setRecordFolder(const std::string& folder);
    bool ready() const;
    void reset();
    void clearCells();
    void feed(const cf32* x, size_t n);
    bool telemetry(TetrapolTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning tetrapolTuning();

} // namespace dect2
