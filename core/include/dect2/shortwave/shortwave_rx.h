// Shortwave receiver (engine standard 213, active 212): the general receiver plus one-click decoders on the same channel.
//
//   capture --> GeneralReceiver (AM / USB / LSB / CW listening on the channel marker: sound, S-meter, squelch, recording)
//           \-> per running decoder: [conjugate for a lower sideband] --> the decoder's own receiver (RTTY, CW, PSK, FT8, ...)
//
// The decoders are the receivers that already exist (HF digital, CW, PSK, ALE, JS8, ARDOP, PACTOR, MT63, FSQ, IFKP, marine NAVTEX / weather fax,
// time signals, the HF waveform identifier); this class only feeds them the shortwave channel: each gets the capture and a dial offset worked out
// by swr::Router (shortwave_route.h) from the channel marker, so a retune moves every running decoder with the marker and restarts none. Decoders
// are made when their button goes on and dropped when it goes off; their results stay while they run. Nothing here plays sound except the
// general receiver (every decoder is silent), so the ear and the decoders listen to the same channel.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks, except for the
// short lock that takes up a button press), telemetry() and the setters from any other thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/general/general_rx.h"
#include "dect2/shortwave/shortwave_route.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct ShortwaveTelemetry;   // shortwave_tel.h

class ShortwaveReceiver {
public:
    ShortwaveReceiver();
    ~ShortwaveReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                 // where the dial frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);                     // the radio's centre frequency (0: a file)
    void setDialHz(double hz);                       // the dial frequency, for the decoders that log it (ALE, time signals, NAVTEX)
    bool ready() const;
    void reset();                                    // a retune of the radio: the listening and the decoders forget the signal; what they decoded stays
    void feed(const cf32* x, size_t n);
    bool telemetry(ShortwaveTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    // ---- the listening side: the general receiver, driven by the screen as in the General mode
    GeneralReceiver& general();
    // the channel marker relative to the dial (the click on the waterfall) and the demodulator; the receiver adds the decoders' audio shift
    void setChannelOffsetHz(double hz);
    void setDemod(int demod);

    // ---- the decoders (any thread; taken up by the next feed())
    bool startDecoder(swr::Dec d);
    bool stopDecoder(swr::Dec d);
    bool decoderActive(swr::Dec d) const;
    void stopAllDecoders();                          // "None"
    void setDecoderOpts(swr::Dec d, const swr::Opts& o);
    swr::Opts decoderOpts(swr::Dec d) const;
    void clearDecoder(swr::Dec d);                    // empties its text
    std::vector<swr::Dec> running() const;
    // Text log: while on, every new character a decoder produces is appended to dir/<key>_<date>.txt (the file is made when the first text arrives)
    void setDecoderLog(swr::Dec d, bool on, const std::string& dir);
    bool decoderLogging(swr::Dec d) const;
    std::string decoderLogPath(swr::Dec d) const;
    // Save what the decoder holds now to a file (the pane's text); false when there is nothing or the file cannot be written
    bool saveDecoderText(swr::Dec d, const std::string& path, std::string* err = nullptr) const;

    // ---- Auto: tries the likely text decoders on the channel at once and keeps the one that produces valid text (swr::Dec in telemetry autoPick)
    void setAuto(bool on);
    bool autoOn() const;
    // ---- the General / Auto hook: a signal the identifier found lights the matching button
    void setDetected(swr::Dec d, float confidence);

    void setSilent(bool s);                          // tests: the general receiver never opens the sound device
    // tests: the router as it is now (a copy)
    swr::Router routerCopy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning shortwaveTuning();
std::string shortwaveSummary(const ShortwaveTelemetry& t);

}  // namespace dect2
