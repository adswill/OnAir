// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat receiver (the "Enhanced Sub-GHz Chat" app and the firmware's CLI chat, esubghz_proto.h). The channel is mixed to
// DC, low-passed and decimated to about 200 ksps; an energy detector marks each packet's burst, which is FM demodulated, corrected for the
// carrier offset measured on the preamble, sampled at the bit timing found on the preamble, searched for the sync word (both frequency
// polarities), its length and its CRC-16. Packets that follow each other within 0.5 s are joined into one message, the way the app does it,
// then decrypted with the key when one is set (AES-256-GCM, replay guard) or read as plain text.
// Engine contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/esubghz/esubghz_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct EsubghzTelemetry;   // esubghz_tel.h

class EsubghzReceiver {
public:
    EsubghzReceiver();
    ~EsubghzReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);           // where the channel sits from the radio's centre
    bool ready() const;
    void reset();                              // the front end only: the chat, the packets and the replay guard are kept
    void clearResults();                       // forget the chat, the packets and a message half received (the replay guard is kept)
    void setKey(const esubghz::Key& k);        // !k.on: no encryption
    // A line OnAir sent (the chat screen calls it once the radio has sent it), shown in the chat in order with what was heard
    void noteSent(const std::string& line, bool encrypted, size_t octets);
    void feed(const cf32* x, size_t n);
    bool telemetry(EsubghzTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning esubghzTuning();

} // namespace dect2
