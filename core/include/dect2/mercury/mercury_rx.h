// Mercury (Rhizomatica HERMES HF modem) receiver, monitor use only: nothing is transmitted, acknowledged or answered.
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front); the upper sideband from 1500 Hz +- 1.4 kHz above the dial
// becomes 8 kHz complex samples with 1500 Hz at 0 Hz, which mercury::OfdmReceiver searches for the bursts of every OFDM data mode at
// once (DATAC0 / 1 / 3 / 4 / 13 / 14 / 15 / 16 / 17, QAM16C2; see mercury_phy.h) and decodes; mercury::Monitor reads the frames
// (mercury_monitor.h). The MFSK floor of the ARQ ladder is not decoded (its bursts are not listed).
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

struct MercuryTelemetry;   // mercury_tel.h

class MercuryReceiver {
public:
    MercuryReceiver();
    ~MercuryReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune: keeps the frames, sessions and files
    void feed(const cf32* x, size_t n);
    bool telemetry(MercuryTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();                        // tests: decode what is pending (the end of a recording)
    void setModeMask(uint32_t mask);     // OfdmModeId bits to search for (all by default)
    void setKnownCalls(const std::vector<std::string>& calls);   // callsigns that name called stations (only their CRC is on the air)
    // a reassembled file or session stream, for viewing or for saving on the user's request
    bool fileBytes(int id, std::vector<uint8_t>& data, std::string& name) const;
    bool sessionBytes(int key, int dir, std::vector<uint8_t>& data) const;
    // "Listen" (off by default): the upper sideband, 200 .. 3800 Hz above the dial, on the sound card
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setListenTap(std::function<void(const float* x, size_t n)> cb);
    void setDemod(int d);                // HfDemod: USB (default), LSB, NFM, AM; only the Listen audio follows it
    int demod() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning mercuryTuning();

struct MercuryPreset { const char* name; double mhz; };
const MercuryPreset* mercuryPresets(int& count);   // dial frequencies to jump to; Mercury has no calling frequency

} // namespace dect2
