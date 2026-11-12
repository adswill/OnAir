// Chase GPS for the radiosonde screens (4.0): the live position of a moving receiver, so a chase car and the sonde show on the same map,
// and SondeHub gets the chase-car position. Sources: an NMEA 0183 stream (a serial GPS: 9600 baud raw on macOS/Linux, or a log file;
// GGA and RMC through the RFI mapper's parser, rfi.h) or gpsd (TCP, default port 2947: ?WATCH JSON, TPV reports). Off until chosen.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace dect2 {

struct ChaseFix { bool valid = false; double lat = 0, lon = 0, altM = 0; bool hasAlt = false; int sats = -1; double steadyT = 0; };

namespace chase {
// One NMEA line into the fix (true when the line was a GGA or RMC position, valid or not); GGA also gives the altitude (metres MSL).
bool nmeaLine(const std::string& line, ChaseFix& fix, double steadyNow);
// One gpsd JSON report into the fix (true for a TPV report): mode 2 or 3 is a fix; altMSL (or alt) in metres with mode 3.
bool gpsdLine(const std::string& line, ChaseFix& fix, double steadyNow);
}  // namespace chase

class ChaseGps {
public:
    enum Source { Off = 0, Nmea = 1, Gpsd = 2 };
    ~ChaseGps() { stop(); }
    void start(Source s, const std::string& where, int port = 2947);   // Nmea: the device or file path; Gpsd: the host
    void stop();
    Source source() const { return src_; }
    bool fix(ChaseFix& f, double maxAgeS = 10) const;   // the last valid fix, if it is younger than maxAgeS
    std::string status() const;
private:
    void runNmea(std::string path);
    void runGpsd(std::string host, int port);
    mutable std::mutex mu_;
    ChaseFix fix_;
    std::string status_ = "off";
    Source src_ = Off;
    std::atomic<bool> quit_{false};
    std::thread th_;
};

}  // namespace dect2
