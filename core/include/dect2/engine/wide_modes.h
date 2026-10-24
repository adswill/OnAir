// Whole-span decoding for the narrowband channel modes: one WideBank per mode (DMR, M17, D-STAR, NXDN, dPMR, P25, pagers, APRS / packet,
// the MPT1327 / LTR / EDACS trunking control channels and Yaesu Fusion). The engine routes the span to the bank of the running mode when the
// switch is on, and takes its telemetry (the selected carrier in detail, every carrier's calls merged in, plus the carriers table and the
// bank's status) in place of the single receiver's.
//
// The modes whose single receiver already decodes every channel of the span (ACARS, VDL2, HFDL, airband, TETRA, AIS) need no bank.
// Known channels of the mode (the built-in frequency database) inside the span are found at a lower threshold (CarrierCfg::priorHz).
#pragma once
#include "dect2/dsp/carrierfind.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct RxTelemetry;
struct WideStatus;

// The settings of the whole-span detector the user can change (the carriers table's settings)
struct WideTuning {
    double onDb = 9, offDb = 5;       // threshold above the noise floor, and where a carrier counts as quiet again (hysteresis)
    double minSnrDb = 0;              // a new carrier must reach this SNR
    double dwellSec = -1;             // it must stay on this long before it is reported (< 0: the mode's own)
    double holdSec = -1;              // it stays this long after it went quiet (< 0: the mode's own)
    int maxCarriers = 8;              // decoded at once at most (the hard cap)
    double cpuBudget = 0.8;           // share of real time the carriers' work may take (0: no limit)
    bool usePriors = true;            // known channels (frequency database) at a lower threshold
    bool priorsOnly = false;          // only the known channels
};

class WideModes {
public:
    WideModes();
    ~WideModes();
    static bool supports(int stdMode);           // engine standard: 10 DMR, 32 M17, 34 D-STAR, 39 NXDN, 46 dPMR, 51 P25, 24 pagers, 25 APRS / packet,
                                                 // 128 MPT1327, 129 Yaesu Fusion, 130 LTR, 131 EDACS
    static const std::vector<int>& modes();      // all of them
    static CarrierCfg cfgFor(int stdMode);       // the raster and limits of the mode
    static std::vector<std::string> freqDbModes(int stdMode);   // the frequency database's mode ids of the mode
    // Sets the span rate; gridOriginHz: the tuned channel's offset from the span centre (the raster passes through it)
    void configure(int stdMode, double spanRate, double gridOriginHz);
    void reset();
    void clearResults();
    void feed(int stdMode, const cf32* x, size_t n);
    // fills t (standard, seq, state, the mode's telemetry, t.wide, t.wideStatus) when something changed since lastSeq
    bool telemetry(int stdMode, RxTelemetry& t, uint64_t& lastSeq);
    void setSelected(int carrierId);
    void setMuted(bool m);
    void setVolume(float v);                                // the sound level of the carrier that plays (all carrier receivers, new ones too)
    // The radio's centre frequency: carriers still inside the span after a retune keep running; those that have gone keep their absolute
    // frequency in the logs (any thread)
    void setCenterHz(double hz);
    void setPagerOptions(int protocols, int pocsagBauds);   // the single receiver's POCSAG / FLEX and baud selection, for the carrier receivers
    void setNarrowChannels(bool pager, bool packet);        // 12.5 kHz channel filter in the pager / packet carrier receivers (25 kHz carrier spacing stays)
    void setPacketOptions(int fixBits, int emphasis);       // the packet receiver's "fix bits" and audio emphasis, for every carrier receiver
    void setTuning(const WideTuning& t);                    // every bank
    const WideTuning& tuning() const;
    // more known channels (absolute Hz, e.g. the band plan's raster in the span) added to the frequency database's
    void setExtraPriors(const std::vector<double>& absHz);
    // Only these channels (absolute Hz) are decoded, every other carrier is left alone; none: no carrier at all. For a trunking follower: a voice
    // channel gets a decoder when its call is granted, not because it is on the air. off: the search as before.
    void setFollowOnly(bool on, const std::vector<double>& absHz);
    // IQ of one carrier of the running mode to a WAV file in dir (returns the path, "" on failure); stop: closes it
    std::string startRecording(int stdMode, int carrierId, const std::string& dir);
    void stopRecording(int stdMode, int carrierId);
    size_t liveCount(int stdMode) const;
    std::vector<double> liveHz(int stdMode) const;
    void setLogCallback(std::function<void(const std::string&)> cb);
    // test hook: rate of the span the bank of this mode was configured with
    bool ready(int stdMode) const;
    // test hook: the load the CPU budget sees (< 0: measure it)
    void setLoadOverride(int stdMode, double load);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
