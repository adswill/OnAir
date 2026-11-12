// NDB (non-directional beacon) receiver. Receive only.
//
// The usual band is the long-wave beacon band, 190-535 kHz (some beacons sit higher, up to about 1750 kHz; the tuner is not
// limited to the beacon band). The carrier is AM. The ident is Morse, about 7 words per minute: a 400 Hz or 1020 Hz tone
// keyed on and off, or the carrier itself keyed (A1A, common on European long-wave beacons). A mark longer than about a
// second is taken as the long dash between idents and is not allowed to pull the speed estimate.
//
// Speed and letters come from the CW timing decoder (cw_morse.h): PARIS timing, dot = 1200 / WPM milliseconds, 5 to 50 WPM.
// 7 WPM is inside that range (a dot is about 171 ms). Family 29, engine standard 31: the unused number between ISM and VOR.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks
// on the sound device beyond a short write), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct NdbTelemetry;

class NdbReceiver {
public:
    NdbReceiver();
    ~NdbReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // the radio sits this far from the carrier (engine passes -tuneOffsetHz)
    bool ready() const;
    void reset();                         // the demodulator; confirmed idents stay until clearResults()
    void feed(const cf32* x, size_t n);
    bool telemetry(NdbTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // 0 auto (400 Hz, 1020 Hz or a keyed carrier, whichever is being keyed), 1 force 400 Hz, 2 force 1020 Hz, 3 force keyed carrier
    void setToneMode(int mode);
    // 4.0 weak-signal path (default on; off = the 3.9 receiver, for before / after measurements)
    void setWeakSignalPath(bool on);
    // 4.0 ident folding (default on, with the weak-signal path): a beacon repeats the same ident. The keying depth of each path is kept for 3 minutes, the
    // ident period is found from its autocorrelation, the last periods are averaged (the keying adds up, the noise averages down) and the average is read
    // as Morse. Needs the ident to have repeated at least 3 times on a steady carrier (docs/SENSITIVITY-4.0-X1.md).
    void setIdentFolding(bool on);
    // "Listen": the keyed tone (a 400 Hz tone gated by the carrier when the beacon sends no modulating tone). Off by default
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setListenTap(std::function<void(const float* x, size_t n)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ndbTuning();

} // namespace dect2
