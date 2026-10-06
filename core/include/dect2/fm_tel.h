// FM receiver telemetry
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {

struct FmTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 searching, 1 tracking, 2 locked
    double cfoHz = 0;           // carrier frequency offset
    float snrDb = 0;            // signal-to-noise ratio
    float devDb = 0;            // FM deviation in dB (peak deviation from center)
    bool stereo = false;        // 19 kHz pilot detected
    bool rdsSync = false;       // RDS synchronization acquired
    float pilotLockDb = 0;      // 19 kHz pilot lock strength
    float rdsSnrDb = 0;         // RDS subcarrier SNR
    
    // RDS data
    std::string psName;         // programme service name (station name)
    std::string radioText;      // radio text message
    std::string ptyText;        // programme type text
    bool trafficAlert = false;  // traffic alert active
    bool trafficProgram = false;// traffic programme flag
    int piCode = 0;             // programme identification code
};

} // namespace dect2
