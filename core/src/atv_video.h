// Analog TV receiver, picture side: from the detected video and the complex baseband at the video rate to lines, fields and pictures.
//   sync slicer (levels found from the signal) -> line phase-locked loop -> field sync (broad pulses) -> line and row assignment
//   -> clamp and gain from the sync pulses -> luminance and colour decoding (PAL with delay-line averaging, NTSC) -> pictures
#pragma once
#include "dect2/atv_std.h"
#include "dect2/atv_tel.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AtvVideoParams {
    int forceSys = -1;               // -1 automatic, otherwise an AtvSys
    int forceColour = -1;            // -1 automatic, otherwise an AtvColourKind
    int deinterlace = 0;             // 0 weave, 1 bob
    int setupMode = -1;              // -1 by system (7.5 IRE for M), 0 none, 1 7.5 IRE
    double chromaDelayNs = -1;       // < 0: what the standard says (170 ns), otherwise this
    bool colourOn = true;
    float saturation = 1.f;
    float hueDeg = 0.f;
};

class AtvVideo {
public:
    AtvVideo();
    ~AtvVideo();
    void configure(double videoRate, bool colourCapable);
    void reset();                                    // forget the signal; counters keep running
    void setParams(const AtvVideoParams& p);
    void setSoundSpacing(double mhz);                // from the carrier search: tells B/G from I, D/K and N
    void setSlicerWidth(double us);                  // 1.0 normally, 2.4 for a weak signal; restarts the line search
    void setFmVideo(bool on);                        // FM video (FPV): follow the baseline wander of an AC coupled transmitter
    void setNoiseScale(double s);                    // C/N in 5 MHz = carrier power * s / noise power measured in the video-rate baseband: s = filter noise gain * input rate / 5 MHz
    void process(const float* v, const float* i, const float* q, size_t n);

    // what the radio side should do
    std::function<void(double rad, double trust)> carrierError;    // once per line
    std::function<void(std::shared_ptr<const AtvFrame>)> frameReady;
    std::function<void(const std::string&)> log;
    bool wantSyncDetector() const;                   // the carrier phase is locked: switch to the synchronous detector
    void syncDetectorChanged(bool on);               // tells the decoder which detector it is listening to (levels restart)
    bool lineLocked() const;
    bool fieldLocked() const;
    double secsSinceLineLock() const;                // 0 while locked
    double soundSpacingHint() const;

    // results
    struct Info {
        std::string system, colourSystem;
        int lines = 0;
        double lineHz = 0, lineErrPpm = 0, fieldHz = 0;
        bool colour = false, killer = false;
        float syncQuality = 0, snrDb = 0, syncTip = 0, whitePeak = 0, burstLevel = 0, chromaErrDeg = 0, syncCompressionPct = 0, carrierToNoiseDb = 0;
        double carrierLevel = 0;                      // sync tip carrier amplitude (input units)
        uint64_t lines_ = 0, fields = 0, frames = 0, fieldsOk = 0, fieldsBad = 0;
        int fieldNo = 0;
        bool state1 = false, state2 = false;
        std::vector<float> lineWave, vbiWave;
    };
    Info info() const;
    // for tests and the tool: the standard the decoder settled on
    AtvFormat format() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
