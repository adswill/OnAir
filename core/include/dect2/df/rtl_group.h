// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hardware plumbing for a KrakenSDR / KerberosSDR hub: N RTL-SDR dongles on one USB hub that share one clock and one switchable noise source.
// Pure logic behind small interfaces (RtlLink, NoiseSwitch) so it is tested with fakes; the real RtlLink (librtlsdr) is makeNativeRtlLink() in
// core/src/source/source_rtlsdr.cpp. Verified only against fakes: no hub has been connected.
#pragma once
#include "dect2/df/group.h"
#include <memory>
#include <string>

namespace dect2::df {

struct RtlInfo { int index = 0; std::string serial, product; };

// One opened dongle
class RtlLink {
public:
    virtual ~RtlLink() = default;
    virtual bool configure(double freqHz, double rateHz, int gainTenthsDb) = 0;   // gain < 0 = automatic
    virtual int readSync(uint8_t* buf, int bytes) = 0;                            // blocking interleaved u8 IQ; bytes read, <= 0 = stopped
    virtual bool setGpio0(bool on) = 0;                                           // the "bias tee" pin; false when the library cannot
};

// Switches the hub's noise source. Public KrakenSDR documentation: the noise source is toggled with a GPIO line of the first dongle (the one the
// RTL library exposes as bias-tee); that mapping is from memory of the docs and UNVERIFIED. Other hubs: implement this interface.
class NoiseSwitch {
public:
    virtual ~NoiseSwitch() = default;
    virtual bool set(bool on) = 0;
};
class GpioNoiseSwitch : public NoiseSwitch {
public:
    explicit GpioNoiseSwitch(RtlLink* first) : l_(first) {}
    bool set(bool on) override { return l_ && l_->setGpio0(on); }
private:
    RtlLink* l_;
};

// Pulls the devices of a hub out of an enumeration: KrakenSDR / KerberosSDR dongles are flashed with the serials 1000..1004 (public docs) or
// say so in the product string. Sorted by serial so channel 0 is serial 1000. Fewer than 2 matches = no hub.
std::vector<RtlInfo> findHub(const std::vector<RtlInfo>& all);
std::string hubLabel(const std::vector<RtlInfo>& hub);   // "KrakenSDR (5 ch)" / "KerberosSDR (4 ch)"

// Streams one link as complex samples (u8 -> (x-127.5)/127.5)
std::unique_ptr<ChannelFeed> makeRtlFeed(RtlLink* link);

struct HubStart { double freqHz = 100e6, rateHz = 1.024e6; int gainTenthsDb = 200; };
struct Hub {
    std::vector<std::unique_ptr<RtlLink>> links;   // owned; the group's feeds point at them
    std::unique_ptr<GpioNoiseSwitch> noise;
    std::unique_ptr<CoherentGroup> group;
};
// Configures every link identically (same frequency, rate, gain) and builds the group; false when any link refuses.
bool startHub(std::vector<std::unique_ptr<RtlLink>> links, const HubStart& s, Hub& out);
// Full calibration: noise source on, group.calibrate(), noise source off (also when the calibration fails). False when the switch or the calibration fails.
bool calibrateHub(Hub& h, NoiseSwitch& sw, size_t n = 16384, int maxLag = 64);

// Real dongles (librtlsdr). Empty / null when the library is missing.
std::vector<RtlInfo> listNativeRtl();
std::unique_ptr<RtlLink> makeNativeRtlLink(int index);

}   // namespace dect2::df
