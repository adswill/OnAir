// DGPS differential beacons (ITU-R M.823): MSK at 25 - 200 bit/s carrying RTCM SC-104 version 2 messages (the GPS word format of IS-GPS-200: 30-bit words, 24 data bits
// and 6 parity bits that depend on the last two bits of the previous word). Receive only.
// Sources: RTCM SC-104 2.x as described in the public "RTCM SC-104 transmission characteristics of marine differential GPS stations" (word format, parity equations,
// message types 1, 3, 5, 6, 7, 9 and 16, with decoded example messages) and ITU-R M.823; the type 7 and type 5 bit positions were compared with gpsd's driver_rtcm2.c
// (BSD-2-Clause, credited in THIRD_PARTY.md). The parity equations are the ones OnAir's GPS LNAV decoder uses on real satellites.
// Demodulation works on baseband IQ with the beacon at 0 Hz after tuning; the bit clock is followed continuously across blocks.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dect2::dgps {

constexpr double kBitRate = 200.0;       // bit/s (the default; beacons also use 100, 50 and 25)
constexpr double kDeviationHz = 50.0;    // MSK at 200 bit/s: the tones sit at the carrier +/- 50 Hz (a quarter of the bit rate; modulation index 0.5)
constexpr int kWordBits = 30;            // RTCM SC-104 word: 24 data bits and 6 parity bits

// The two words that open an RTCM message.
struct Header {
    int type = 0;        // message type, 6 bits
    int stationId = 0;   // reference station ID, 10 bits
    int zcount = 0;      // modified Z-count, 13 bits (0.6 s units)
    int seq = 0;         // frame sequence number, 3 bits
    int length = 0;      // number of data words after the header, 5 bits
    int health = 0;      // station health, 3 bits (0..5: UDRE scale factor 1.0, 0.75, 0.5, 0.3, 0.2, 0.1; 6 not monitored; 7 station not working)
};

struct SatCorr {         // types 1 and 9: one satellite
    int sat = 0;         // 1 - 32
    int udre = 0;        // 0 - 3
    bool sf = false;     // scale factor bit: PRC in 0.32 m units and RRC in 0.032 m/s instead of 0.02 m and 0.002 m/s
    int prcRaw = 0;      // 16 bit two's complement
    int rrcRaw = 0;      // 8 bit two's complement
    int iod = 0;
    double prc() const { return prcRaw * (sf ? 0.32 : 0.02); }    // metres
    double rrc() const { return rrcRaw * (sf ? 0.032 : 0.002); }  // metres per second
};

struct SatHealth {       // type 5: one satellite
    int sat = 0, dataHealth = 0, cn0 = 0, timeToUnhealthy = 0;   // cn0: 0 not tracked, else 24 + value dB; time in 5 minute units
    bool iodLink = false, healthEnable = false, newNavData = false, lossWarning = false;
};

struct Almanac {         // type 7: one beacon
    double latitude = 0, longitude = 0;   // degrees
    int rangeKm = 0;
    double frequencyKhz = 0;
    int health = 0, stationId = 0, bitRate = 0, modMode = 0, syncType = 0, encoding = 0;
};

struct Message {
    Header h;
    bool complete = false;           // all N data words passed their parity
    int wordsBad = 0;                // data words that failed
    std::vector<SatCorr> sats;       // types 1, 9
    std::vector<SatHealth> satHealth; // type 5
    std::vector<Almanac> almanac;    // type 7
    bool hasPosition = false;        // type 3
    double x = 0, y = 0, z = 0;      // ECEF metres
    double lat = 0, lon = 0, height = 0;   // degrees, degrees, metres above the WGS-84 ellipsoid
    std::string text;                // type 16
    uint64_t bitIndex = 0;           // position in the bit stream (a rough timestamp: bit / bit rate)
};

struct Stats {
    double carrierOffsetHz = 0;   // the carrier (midpoint of the two tones) from the nominal frequency
    double levelDb = -200;        // mean power of the last block, dB relative to full scale (1.0)
    double bitRate = kBitRate;
    int framesOk = 0;             // headers whose preamble and parity hold (both header words)
    int messagesComplete = 0;     // messages whose data words all passed
    int wordsBad = 0;             // data words that failed their parity
    std::set<int> typesSeen;      // message types seen so far
    Header last;                  // the last valid header
    bool inverted = false;        // the polarity at which the last header was found (a whole-channel inversion decodes the same, so this stays false in practice)
    std::vector<Message> messages;   // the most recent messages, oldest first (at most kKeep)
    uint64_t bitsSeen = 0;
};
constexpr size_t kKeep = 40;

// Receiver: MSK demodulation (integrate and dump on the instantaneous frequency, the bit clock followed across blocks) followed by the RTCM word, header and message
// parse. Blocks may be any length; about a second or more gives the best carrier estimate.
class Receiver {
public:
    explicit Receiver(double bitRate = kBitRate) : bitRate_(bitRate) { st_.bitRate = bitRate; }
    void process(const std::complex<float>* iq, size_t n, double fs);
    const Stats& stats() const { return st_; }
    void reset();

private:
    Stats st_;
    double bitRate_;
    std::vector<uint8_t> pending_;   // demodulated bits not yet scanned
    std::vector<double> fbuf_;       // instantaneous frequency (Hz) of samples not yet cut into bits
    double pos_ = 0;                 // where the next bit starts in fbuf_ (samples)
    bool timed_ = false;             // the bit clock has been found
    std::complex<float> prev_{0, 0};
    bool havePrev_ = false;
    bool haveCarrier_ = false;
    uint64_t consumedBits_ = 0;      // bits removed from pending_ so far
};

// ECEF (metres) to WGS-84 latitude, longitude (degrees) and ellipsoidal height (metres)
void ecefToGeodetic(double x, double y, double z, double& latDeg, double& lonDeg, double& h);

// Test and tooling helpers: build the channel bits of an RTCM message and the MSK waveform of a bit stream.
// encodeMessage: header words (preamble, type, station; zcount, seq, length, health) then the 24-bit data words
// of 'extra', each with its 6 parity bits; one bit per entry (0 or 1). pol inverts the whole channel, since the
// beacon's polarity is not known in advance. The length field is written as h.length (not checked against extra.size() so that tests can lie).
std::vector<uint8_t> encodeMessage(const Header& h, const std::vector<uint32_t>& extra, int pol = 0);
// The 24-bit data words of the bodies of each message type (the length goes into the header)
std::vector<uint32_t> bodyType1(const std::vector<SatCorr>& sats);        // also type 9
std::vector<uint32_t> bodyType3(double x, double y, double z);
std::vector<uint32_t> bodyType5(const std::vector<SatHealth>& sats);
std::vector<uint32_t> bodyType7(const std::vector<Almanac>& beacons);
std::vector<uint32_t> bodyType16(const std::string& text);
// rateScale: bit clock error (1.001 sends 0.1 % faster); bitRate sets the deviation (a quarter of it)
std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, float amp = 0.5f, double rateScale = 1.0, double bitRate = kBitRate);

}  // namespace dect2::dgps
