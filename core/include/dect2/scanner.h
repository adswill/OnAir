// Channel scanner: steps through a frequency range, rejects empty channels from the spectrum, then tries to lock a
// DVB-T2 signal (P1 -> L1) and optionally reads the service list.
#pragma once
#include "engine.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <memory>

namespace dect2 {

struct ScanConfig {
    double startMHz = 474, stopMHz = 858, stepMHz = 8;
    double bwMhz = 8;
    bool atsc = false;             // scan for ATSC 8-VSB (6 MHz channels, centre frequencies of the US/Korea raster)
    bool isdbt = false;            // scan for ISDB-T (6 MHz channels, 13 segments)
    bool autoBandwidth = true;     // measure the width of each signal found and decode it with the matching channel bandwidth
    bool identifyServices = true;
    double occupancyDb = 5.0;      // in-band power must exceed the out-of-band floor by this much
    double lockTimeoutSec = 5.0;   // time allowed to lock (the receiver alternates DVB-T2 and DVB-T searches)
    double serviceTimeoutSec = 9;  // time allowed to read the service list (identify = true)
    TuneSettings tune;             // gain / amp / filter settings
};

struct ScanResult {
    double freqMHz = 0, bwMhz = 8;
    double levelDbfs = -120;       // mean in-band power per bin (dBFS)
    double occupancyDb = 0;        // in-band minus out-of-band floor
    bool occupied = false;
    bool t2 = false;               // a DVB-T2 or DVB-T signal was locked (see `standard`)
    std::string standard = "DVB-T2"; // "DVB-T2" or "DVB-T"
    bool l1post = false;
    std::string note;
    std::string mode;              // e.g. "T2-Base SISO 32K 1/8 PP8 ext"
    std::string plpInfo;           // e.g. "64-QAM 2/3 rotated, 1 PLP"
    float snrDb = 0;
    int cellId = 0, networkId = 0;
    std::string networkName;
    std::vector<std::string> services;
    std::vector<std::string> unsupported; // modes found that the receiver cannot decode (MISO, TI type 1, sub-slicing, ...)
    double muxKbps = 0;
    double fecGood = 0;            // fraction of FEC blocks decoded
};

struct ScanProgress {
    bool running = false;
    int index = 0, total = 0;
    double currentMHz = 0;
    std::string phase;
};

class Scanner {
public:
    Scanner();
    ~Scanner();
    // Whether this radio can scan with these settings (so the caller can find out before stopping the receiver).
    static bool check(const DeviceInfo& dev, const ScanConfig& cfg, std::string& err);
    bool start(const DeviceInfo& dev, const ScanConfig& cfg, std::string& err);
    void stop();
    ScanProgress progress() const;
    std::vector<ScanResult> results() const;
    static std::vector<double> channels(const ScanConfig& cfg);

private:
    void run();
    bool sleepOrCancel(double seconds);
    mutable std::mutex mu_;
    std::vector<ScanResult> results_;
    ScanProgress prog_;
    ScanConfig cfg_;
    DeviceInfo dev_;
    std::thread th_;
    std::atomic<bool> cancel_{false};
    std::unique_ptr<Engine> eng_;
};

} // namespace dect2
