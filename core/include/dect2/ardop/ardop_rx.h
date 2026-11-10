// ARDOP (Amateur Radio Digital Open Protocol, HF ARQ modem of Winlink) receiver, monitor use only.
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front), then the upper sideband becomes 8 kHz audio
// (hfdig::UsbAudio) and a 1200 .. 1800 Hz band-pass keeps the 200 Hz channel ARDOP uses for every frame header. Every millisecond the
// last 20 ms are measured at the two leader tones (1475 and 1525 Hz, shifted by seven trial tuning errors of -45 .. +45 Hz): a leader is
// a run of high two-tone energy ending in the dip where the sync symbol (the leader with its phase reversed) cuts the window in half;
// that dip places the first frame type symbol to 1 ms. The tuning error is then refined from the leader and the symbol timing from the
// ten frame type symbols (4FSK at 50 baud: 1425 .. 1575 Hz in 50 Hz steps; the type byte, its XOR with the session ID, one parity symbol each).
// The type is the table entry with the smallest soft distance. Frames with a body (ID, connect request, ping: callsigns and locator with
// Reed-Solomon; connect and ping acknowledge; 4FSK data at 50 and 100 baud with byte count, CRC-16 and Reed-Solomon) are read to the end;
// PSK, QAM and 600 baud frames are listed by their header only. Calls go to the station list, data bytes that pass their CRC to the text.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/audio/hf_demod.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct ArdopTelemetry;   // ardop_tel.h

// One frame read by the receiver (setFrameTap): the Winlink ARDOP link reads its frames here
struct ArdopFrameEvent {
    int code = 0;
    int sid = -1;
    bool bodyOk = false;            // the body passed its check (frames without a body: false)
    std::string from, to;
    std::vector<uint8_t> payload;   // data frames: the bytes that passed the CRC
    double timeSec = 0;
};

class ArdopReceiver {
public:
    ArdopReceiver();
    ~ArdopReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune: keeps the frames and stations
    void feed(const cf32* x, size_t n);
    bool telemetry(ArdopTelemetry& out, uint64_t lastSeq);
    void clearResults();                 // frames, stations and text
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setFrameTap(std::function<void(const ArdopFrameEvent&)> cb);   // every frame with a good header; called from the receiver thread
    void flush();                        // tests: report what is pending (the end of a recording)
    // "Listen" (off by default): the upper sideband, 200 .. 3800 Hz above the dial, on the sound card
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setListenTap(std::function<void(const float* x, size_t n)> cb);
    void setDemod(int d);                // HfDemod (hf_demod.h): USB (default), LSB, NFM, AM; Auto counts as USB
    int demod() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ardopTuning();

struct ArdopPreset { const char* name; double mhz; };
const ArdopPreset* ardopPresets(int& count);   // dial frequencies in the digital part of each band (upper sideband); gateways publish their own

} // namespace dect2
