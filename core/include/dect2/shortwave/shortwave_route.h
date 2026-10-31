// Shortwave mode, the routing part (pure logic: no DSP, no GUI, no clock, no radio).
//
// The Shortwave mode is the general receiver (AM / USB / LSB / CW listening on a click-to-tune channel) plus a strip of one-click
// decoders. This file knows the decoders (one table row each: name, group, sideband, where the signal sits in the audio, what it needs)
// and keeps the *set that is switched on*. Clicking a decoder starts it on the channel that is being listened to; retuning moves every
// running decoder with the channel (the decoder is never restarted by a retune, only its offset changes); clicking it again stops it;
// several can run on the same channel.
//
// Geometry. A decoder is one of OnAir's receivers (HF digital, CW, PSK, ALE, ...). Each takes the complex capture and a "dial
// offset": where its dial frequency sits relative to 0 Hz of the input, and listens to the upper sideband 200 .. 3800 Hz above that
// dial. The channel the user listens to (the click on the waterfall) is the *signal*, so the decoder's dial is put `pitchHz` below it
// (the signal then sits at `pitchHz` in the decoder's audio). A lower sideband decoder is fed the mirrored capture (conjugate) so the
// same receiver serves both. With `ch` the channel's offset from the input's 0 Hz:
//
//     USB:  dialOffset =  ch - pitch
//     LSB:  dialOffset = -ch - pitch      (the input is mirrored: the channel is at -ch)
//
// Tests: tests/shortwave/test_shortwave_route.cpp (no GUI), test_shortwave_path.cpp (the real receivers on synthetic IQ).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace swr {

enum class Dec : int {
    Rtty = 0, Cw, Psk, Ft8, Ft4, Wspr, Js8, Sstv, Olivia, Contestia, Mfsk, Thor, DominoEx, Hell, HamDrm, FreeDv,
    Ale, Ardop, Pactor, Mt63, Fsq, Ifkp, Navtex, Wefax, TimeSig, HfId, Count
};
constexpr int kDecCount = (int)Dec::Count;

// how a decoder presents itself
enum class Out : int { Text = 0, Lines, Picture, Status };   // a running text, one line per decode / call, a picture (plus a status line), a status line only

struct DecoderInfo {
    Dec id;
    const char* key;         // stable: preferences and the command line ("rtty")
    const char* name;        // on the button
    const char* group;       // "Text", "Weak signal", "Images", "Data links", "Other"
    const char* blurb;       // the tooltip
    Out out;
    int sideband;            // the sideband a click uses: +1 upper, -1 lower
    double pitchHz;          // where the clicked signal sits in the decoder's audio (the dial goes this far below it)
    double minRateHz;        // the narrowest capture it works on
    const char* needs;       // nullptr, or what the user must install ("the Codec2 library")
    const char* sigIdMode;   // the identifier's mode id / sub-mode id this decoder answers to (the General / Auto hook), "" none
    double defMhz;           // a typical frequency (for the band presets and the tests)
};

const DecoderInfo* decoderTable(int& count);
const DecoderInfo& decoderInfo(Dec d);
Dec decoderFromKey(const std::string& key, bool* ok = nullptr);
// The General / Auto hook (docs/SHORTWAVE-MODE.md): the identifier names a signal by mode id and sub-mode id ("hfdig" "rtty", "psk", "cw");
// the matching decoder, or Count when none is a shortwave decoder.
Dec decoderForSignalId(const std::string& modeId, const std::string& subId);

// per-decoder options, all "automatic" by default; the Advanced foldout of the UI edits them
struct Opts {
    int sideband = 0;        // 0 the decoder's default, +1 upper, -1 lower
    double pitchHz = -1;     // < 0: the decoder's default
    int rttyBaud = -1;       // -1 automatic, else an index of kRttyBaudTable
    int rttyShift = -1;      // -1 automatic, else an index of kRttyShiftTable
    bool rttyReverse = false;
    int pskMode = 0;         // kPskModes
    int variant = -1;        // Olivia / Contestia tones x bandwidth, MFSK / THOR / DominoEX speed: -1 the family's default
    bool operator==(const Opts& o) const {
        return sideband == o.sideband && pitchHz == o.pitchHz && rttyBaud == o.rttyBaud && rttyShift == o.rttyShift && rttyReverse == o.rttyReverse &&
               pskMode == o.pskMode && variant == o.variant;
    }
};

// What one running decoder must do now. offsetHz is for setSignalOffset() of its receiver; mirror: feed it the conjugate of the capture.
struct Plan {
    Dec id = Dec::Rtty;
    bool active = false;
    double dialOffsetHz = 0;
    bool mirror = false;
    double pitchHz = 0;
    Opts opts;
};

class Router {
public:
    // ---- the buttons
    bool start(Dec d);                       // false when it was already running
    bool stop(Dec d);                        // false when it was not
    bool toggle(Dec d);                      // returns the new state (true: running)
    void stopAll();                          // the "None" button
    bool active(Dec d) const { return on_[(int)d]; }
    int count() const;
    std::vector<Dec> running() const;        // in table order

    // ---- the channel being listened to, as an offset from the input's 0 Hz (the dial's offset plus the channel marker). A retune is this call.
    void setChannel(double offsetFromCentreHz);
    double channel() const { return ch_; }

    // ---- options
    void setOpts(Dec d, const Opts& o);
    const Opts& opts(Dec d) const { return opts_[(int)d]; }
    int effectiveSideband(Dec d) const;      // the option, else the decoder's default
    double effectivePitch(Dec d) const;

    // ---- what to do
    Plan plan(Dec d) const;
    std::vector<Plan> plans() const;         // running decoders only
    // The listening audio follows a running decoder: for an upper (lower) sideband demodulator the General channel is put pitch below (above) the
    // signal so the signal is heard at the pitch. 0 when no decoder runs or the demodulator is not a sideband one. demodIsUsb / demodIsLsb: GeneralDemod.
    double audioShiftHz(bool demodIsUsb, bool demodIsLsb) const;
    // The sideband the General demodulator should take when the last started decoder wants one: +1, -1, or 0 (no decoder).
    int wantedSideband() const;

    // ---- the General / Auto hook: a signal the identifier found lights the matching button (it does not start it)
    void setDetected(Dec d, float confidence);
    void clearDetected() { detected_ = Dec::Count; conf_ = 0; }
    Dec detected() const { return detected_; }
    float detectedConfidence() const { return conf_; }

    // changes with every start / stop / option change (not with a retune): the receiver rebuilds its decoders when it moves
    uint64_t revision() const { return rev_; }
    // changes with a retune too
    uint64_t moves() const { return moves_; }

private:
    bool on_[kDecCount] = {};
    Opts opts_[kDecCount];
    double ch_ = 0;
    Dec last_ = Dec::Count;
    Dec detected_ = Dec::Count;
    float conf_ = 0;
    uint64_t rev_ = 1, moves_ = 1;
};

// ---- text log helpers (pure)
// The new characters of a decoder's text since the last time: the decoders keep only the last few thousand characters, so the window slides.
// prev is what was returned the last time (the whole window then); cur the window now. Returns what to append to the log.
std::string textDelta(const std::string& prev, const std::string& cur);

}  // namespace swr
}  // namespace dect2
