// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT for the engine: IQ in, files out. kaft::FrameReceiver finds the frames of all three widths at once; the control frames and the data bursts go to a
// kaft::Receiver session (OPEN, START, data, FINISH), and a file whose hash checks is written to the folder the user chose. This receiver only listens: it
// never sends ACK, ACCEPT or VERIFIED (OnAir has no half-duplex turnaround), so a sender that waits for replies cannot finish through it; the transmit
// option of OnAir sends one-way (OPEN, data, FINISH, repeated), and a receiver that joins late or loses blocks completes on a later pass. Family 125,
// engine standard 127, telemetry 126.
//
// File names come from the air and are not trusted: only a plain file name is used (no path, no leading dot, odd characters replaced), an existing
// file is never overwritten (a number is added), and nothing is opened or run. Written to the engine's contract: configure() once the sample rate is
// known, feed() from the analysis thread, telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct KaftTelemetry;   // kaft_tel.h

class KaftLiveReceiver {
public:
    KaftLiveReceiver();
    ~KaftLiveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}              // the channel sits on the tuned frequency
    void setCenterHz(double) {}
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(KaftTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSaveFolder(const std::string& folder);   // "": received files are not kept
    void clearFiles();                               // empties the list of received files (the files on disk stay)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning kaftTuning();

} // namespace dect2
