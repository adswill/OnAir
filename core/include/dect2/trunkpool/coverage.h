// Single-radio trunking (docs/TRUNK1-3.9.md): which voice channels one capture can hold together with the control channel, the span a
// channel outside it would need, the channels a site has used (learned from its grants), and the centre and sample rate that cover the
// control channel and the most used voice channels within what the radio can deliver. Plain logic, no DSP; shared by every trunking mode.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace tpool {

// The part of a capture the voice slices may use (the flat part of the radio's decimation filter), as a fraction of the sample rate. The same
// figure as CaptureVoice::captureSpanHz.
constexpr double kUsableFraction = 0.84;

// The fastest sample rate a radio delivers without losing samples. name: the driver or the device's name as shown ("RTL-SDR", "HackRF One",
// "airspy", "SDRplay RSP1A" ...), any case. 0: not known. These are the usual figures for the radio on a normal USB link; the device's own
// report (DeviceInfo::maxRateHz) wins when the host has it.
double radioMaxRateHz(const std::string& name);

// A channel at hz inside the capture (centre, rate) with room for its own width: |hz - centre| <= usable span / 2 - edgeGuardHz
bool insideCapture(double hz, double centreHz, double rateHz, double edgeGuardHz = 12500);

// The usable span that holds every frequency in hz (the control channel and the voice channels), with edgeGuardHz on each side; and the
// sample rate that gives it (span / kUsableFraction)
double spanToCoverHz(const std::vector<double>& hz, double edgeGuardHz = 12500);
double rateToCoverHz(const std::vector<double>& hz, double edgeGuardHz = 12500);

// "outside the capture (needs 3.4 MHz)": the words the trunking modes show for a call they cannot follow. controlHz 0: the span for the
// channel alone around the current centre. maxRateHz > 0 adds "; this radio reaches 2.4 MHz" when the needed rate is above it.
std::string outsideText(double hz, double controlHz, double centreHz, double edgeGuardHz = 12500, double maxRateHz = 0);

// The voice channels a site has used: every grant counts for its channel (kept per frequency, the most used first)
struct ChannelUse { double hz = 0; uint64_t grants = 0; double lastSec = 0; };
class ChannelUsage {
public:
    void add(double hz, double nowSec);
    void clear() { v_.clear(); }
    const std::vector<ChannelUse>& channels() const { return v_; }   // most used first
    size_t size() const { return v_.size(); }
private:
    std::vector<ChannelUse> v_;
};

// The coverage plan: where to put the centre and which rate to use so the control channel and as many of the most used voice channels as
// fit are inside the capture, none of them on the radio's DC spike.
struct CoverageInput {
    double controlHz = 0;                    // must be inside (0: none, the voice channels only)
    std::vector<ChannelUse> channels;        // the learned list (ChannelUsage::channels)
    double maxRateHz = 0;                    // the radio's fastest rate (0: not known, take what covers everything)
    double minRateHz = 0;                    // the slowest rate worth using (0: 1 Msps, or the max when it is lower)
    double edgeGuardHz = 12500;              // half a channel plus margin at the edges of the usable span
    double dcGuardHz = 20000;                // no channel this close to the centre (the DC spike and the radio's LO leakage)
};
struct CoveragePlan {
    bool ok = false;                         // a plan was made (a control channel or a channel was given)
    double centreHz = 0, rateHz = 0;         // where to tune and the rate to use
    double spanAllHz = 0;                    // the usable span that would hold the control channel and every learned channel
    double rateAllHz = 0;                    // ... and the rate that gives it
    int covered = 0, total = 0;              // learned channels inside the plan, of all
    uint64_t grantsCovered = 0, grantsTotal = 0;   // their share of the grants heard
    bool controlInside = false;
    bool dcClear = true;                     // no channel within dcGuardHz of the centre
    std::vector<double> outside;             // learned channels left outside, most used first
    std::string summary;                     // one line for the user
};
CoveragePlan planCoverage(const CoverageInput& in);

} // namespace tpool
} // namespace dect2
