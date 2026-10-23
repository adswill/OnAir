// PACTOR-I monitor receiver (experimental, receive only; PACTOR-II, -III and -IV are proprietary and not supported, see docs/pactor.md).
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front), then the upper sideband becomes 8 kHz audio
// (hfdig::UsbAudio). A 4096-point spectrum every 0.25 s looks for the two FSK tones 200 Hz apart (any audio frequency 150 .. 3600 Hz, a
// tuning error of the radio just moves the pair); a stable pair is locked. The energy of the two tones every millisecond gives the bursts.
// Every burst is walked from its start: at each position a packet (100 baud, 96 bits, or 200 baud, 192 bits: both tried at timing offsets
// of +-30 ms in 1 ms steps and both tone polarities, because the real system swaps the polarity with every cycle) is accepted when its
// CRC-16 passes; otherwise a 12-bit control signal is tried (CS1 .. CS4, one bit error allowed) which is accepted only when the rest of the
// burst is a packet or nothing; the control signal of the receiving station is sent right before the next packet of the sending station, so
// the two often form one burst. Packets that fail are kept as soft bits and added to the next copy of the same packet (memory ARQ).
// Identical packets are marked as repeats (ARQ retries, or the repetitions of an unproto / FEC broadcast with no control signals).
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

struct PactorTelemetry;   // pactor_tel.h

class PactorReceiver {
public:
    PactorReceiver();
    ~PactorReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune: keeps the packets and stations
    void feed(const cf32* x, size_t n);
    bool telemetry(PactorTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
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

ModeTuning pactorTuning();

struct PactorPreset { const char* name; double mhz; };
const PactorPreset* pactorPresets(int& count);   // starting points in the digital part of each band (upper sideband dial)

} // namespace dect2
