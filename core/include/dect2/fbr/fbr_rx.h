// Fleet Broadcast monitor (UHF SATCOM downlink, 250.450 / 250.550 / 250.650 MHz): receive only, framing and channel activity, nothing decoded.
//
// What the public description says (r00t.cz, "Fleet Broadcast Decoder v0.5", 2009): a 16 channel 75 Bd multiplex, differential BPSK at 1200 Bd,
// channel 0 carries the synchronisation, the other channels are usually pseudo-random (encrypted) data, sometimes KG-84 traffic, no plain text
// ever seen. 16 channels x 75 Bd = 1200 Bd: one frame is 16 bits, one bit per channel. A US Navy shore-terminal description (FLTSATCOM) agrees on
// 75 bit/s subchannels sent time-division multiplexed at 1200 bit/s. (That page's software is "personal use only" and binary: nothing of it is used.)
//
// What is NOT public (and so is not claimed here): the sync pattern of channel 0, the pulse shape, the bit polarity, the framing of any message
// inside a channel. So this receiver does: carrier acquisition (squaring), 1200 Bd symbol clock, differential bits, finds the one column of the
// 16-bit frame that repeats and takes it for channel 0, and classifies every channel as sync / reversals / idle / random-looking. It never decodes
// text and never tries to decrypt anything: a random-looking channel is shown as encrypted-looking hex.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct FbrTelemetry;   // fbr_tel.h

class FbrReceiver {
public:
    FbrReceiver();
    ~FbrReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);      // the signal's nominal place from the radio's centre (the engine passes the tuning offset)
    void setCenterHz(double hz);          // the radio's centre, for the absolute frequency and the carrier name (0: unknown)
    void setInvert(bool on);              // flip the bit polarity (the real polarity is not published)
    bool ready() const;
    void reset();
    void clearResults();                  // the bits, frames and log
    void feed(const cf32* x, size_t n);
    bool telemetry(FbrTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning fbrTuning();

namespace fbr {
struct Carrier { const char* name; double mhz; };
const Carrier* carriers(int& count);                      // 250.450, 250.550, 250.650 MHz (r00t.cz)
std::string carrierNear(double mhz, double tolKhz = 5);   // "" none
constexpr double kBaud = 1200.0;       // differential BPSK symbol rate (r00t.cz)
constexpr int kChannels = 16;          // 16 channels of 75 Bd
constexpr double kChannelBaud = 75.0;
constexpr double kMaxCarrierHz = 4500; // acquisition range around the nominal place (satellite Doppler is a few kHz)
} // namespace fbr

} // namespace dect2
