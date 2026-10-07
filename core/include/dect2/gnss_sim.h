// A simulated sky for the GNSS test signal: a constellation with synthetic but self-consistent orbits (they have the shape of real GPS orbits;
// they are NOT the orbits of today's satellites), the geometry to a receiver on the ground, the navigation messages and the rendering of the
// signals as complex baseband at any sample rate. Nothing is transmitted: the samples only exist in memory or in a file.
// The orbit, delay and message code here is written separately from the receiver's (gnss_nav.cpp, gnss_solve.cpp), so that a position that comes out
// of the receiver is not just the generator's own arithmetic read back.
#pragma once
#include "gnss_nav.h"
#include "gnss_tel.h"
#include "ring.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct GnssSimConfig {
    unsigned systems = 1;            // bit mask of the systems (GnssSystem bits); only GPS (bit 0) for now
    double latDeg = 25.2, lonDeg = 55.36, heightM = 10;   // the receiver (Dubai)
    double cn0Top = 44;              // C/N0 of a satellite at the zenith, dB-Hz; lower satellites are weaker (range and antenna pattern)
    int maxSats = 0;                 // 0 = every satellite above the mask (8 to 11); otherwise the highest ones
    double maskDeg = 5;              // elevation mask of the simulated antenna
    bool warmStart = true;           // the signal starts 22 s into a navigation frame: subframe 1 begins 8 s later, so the whole ephemeris has been sent 26 s after the start;
                                     // otherwise it starts 13 s into the frame (cold: subframe 1 begins after 17 s and the ephemeris is complete after 35 s)
    double cfoHz = 0;                // receiver oscillator error as seen at L1: the signals appear this many Hz higher; the sample clock follows it
    double sroPpm = 0;               // extra sample clock error
    bool jammer = false;             // a continuous wave 400 kHz above the centre, 15 dB above the noise power in 2 MHz
    double dcOffset = 0;             // added to I and Q (the radio's DC spike), full scale = 1
    double noiseRms = 0.14;          // per component, full scale = 1
    bool realAtmosphere = true;      // ionosphere and troposphere delays on (the receiver corrects them with the broadcast model, so a residual stays)
    unsigned seed = 1;               // the constellation and its parameters
    unsigned noiseSeed = 0;          // the noise (0 = the same as seed)
};

struct GnssSimSat {
    int sys = 0, prn = 0;
    GpsEphemeris eph;                // what is broadcast, as the decoded values of the transmitted integers
    GpsAlmanac alm;
    bool transmitted = false;        // above the mask: its signal is in the stream
    double cn0 = 0;
    double azDeg = 0, elDeg = 0;     // at the start
};

class GnssSim {
public:
    GnssSim(const GnssSimConfig& cfg, double sampleRate);
    ~GnssSim();
    double sampleRate() const;
    void generate(cf32* out, size_t n);          // the next n samples
    const std::vector<GnssSimSat>& sats() const; // the whole constellation (30 GPS satellites); `transmitted` marks the visible ones
    const double* receiverEcef() const;         // the true position
    int week() const;                            // the GPS week of the start (full week number)
    double startTow() const;                     // GPS time of week of the first sample, true time
    GpsIono iono() const;                        // the broadcast Klobuchar parameters
    GpsUtc utc() const;
    // true geometry for tests: position of satellite `i` (index into sats()) at transmit time tTx and the propagation time to the receiver
    // when the signal arrives at true GPS time tRx (geometric only, in seconds)
    double geometricDelay(size_t i, double tRx) const;
    // the delays that the simulation really applied at tRx, seconds: ionosphere (code), troposphere
    void atmosphereDelays(size_t i, double tRx, double* ionoS, double* tropoS) const;
    // the true GPS time of week at which the receiver's sample clock reads `rxSeconds` (seconds since the first sample)
    double trueTime(double rxSeconds) const;
    // the satellite's own clock reading (GPS time of week scale) at the transmission of the signal that arrives at true time tRx: what a perfect receiver would measure
    double svTransmitTime(size_t i, double tRx) const;
    // a 300 bit subframe as the satellite sends it (for the tests and the tool): subframe index since the start of the week
    void subframeBits(size_t i, uint32_t subframeIndexInWeek, uint8_t* bits300) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
