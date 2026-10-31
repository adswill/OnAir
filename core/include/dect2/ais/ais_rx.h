// AIS receiver: AIS 1 and 2 (161.975 / 162.025 MHz) and the long-range channels 75 and 76 (156.775 / 156.825 MHz, message 27), all that the
// capture holds at the same time (a 6 Msps capture at 159.4 MHz holds all four). A satellite mode widens the channel filters for the Doppler shift
// of a receiver in low earth orbit and decodes collided bursts (ais_sat.h); docs/SATELLITE_AIS.md.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/geo/passes.h"
#include "ais_alerts.h"
#include "ais_phy.h"
#include "ais_tel.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AisReceiver {
public:
    AisReceiver();
    ~AisReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    // The radio's centre frequency (0 = not known). Known, the four channels are placed by their own frequencies and every pair the capture holds
    // is decoded (AIS 1 + 2, 75 + 76); not known, AIS 1 and 2 sit at -+25 kHz of the signal offset as before (a recording, the test signal).
    void setCenterHz(double hz);
    void setChannelMask(int mask);                       // the channels to decode at most (bit c = channel c of AisTelemetry; default 15 = all four)
    // Satellite mode: channel filters wide enough for +-4.5 kHz of Doppler shift, the collision-tolerant decoder (frequency candidates, parts of
    // bursts, successive cancellation) and Doppler tracking. Off by default (a coast receiver does not need it and it costs CPU).
    void setSatelliteMode(bool on);
    // The satellite the IQ comes from (satellite mode): the expected Doppler shift of every ship heard with a position becomes a frequency candidate.
    void setSatelliteTle(const Tle* tle);                // nullptr: none
    // Coast mode: when a burst fails or leaves a strong remainder, decode again after cancelling the burst already decoded and with the carrier
    // moved by the few hundred Hz two transmitters differ by (a weaker message under a stronger one in a busy port). Default on; the satellite mode
    // always has its own, wider version of it.
    void setCollisionCancel(bool on);
    // CPA and geofence alerts (ais_alerts.h): off until the config has a limit or a fence. Alerts go to the automations (ais.cpa, ais.geofence)
    // and to AisTelemetry::alerts. May be set at any time.
    void setAlertConfig(const AisAlertConfig& c);
    void setTimeBase(double unixSec);                    // UNIX time of the first sample (0 = the computer's clock when the samples start)
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the stations and sentences stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(AisTelemetry& out, uint64_t lastSeq);
    // The station table and the sentence list stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setPhyConfig(const AisPhyConfig& c);               // before configure(); the defaults are the tuned values
    // every frame with a good FCS: channel 'A' / 'B', the payload bits, signal time, SNR in 48 kHz, carrier offset (VDES ASM listens through it)
    void setFrameCallback(std::function<void(char, const ais::Bits&, double, double, double)> cb);
    void setForwardFeeds(bool on);                          // the data feeds' AIS output (default on)
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning aisTuning();

} // namespace dect2
