// FM receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct FmTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 searching (no carrier), 1 tracking (a weak carrier), 2 locked (a clean carrier)
    double cfoHz = 0;           // tuning error: the mean frequency of the carrier
    float snrDb = 0;            // audio signal-to-noise ratio, from the noise above the multiplex (0 - 60)
    float devKhz = 0;           // peak frequency deviation over the last moment (75 kHz = 100 % modulation)
    bool carrier = false;       // a constant-envelope carrier is in the channel (noise alone is not)
    float levelDbfs = -120;     // power of the selected channel after filtering
    bool stereo = false;        // the 19 kHz pilot is locked and the stereo decoder is on
    float pilotPct = 0;         // pilot injection in % of 75 kHz deviation (a healthy station: 8 - 10)
    bool rdsSync = false;       // RDS block synchronisation
    float rdsBlockOkPct = 0;    // share of RDS blocks with a good checkword, recent
    uint64_t rdsGroups = 0;     // RDS groups decoded since start

    // RDS contents (empty until received)
    std::string psName;         // programme service name, 8 characters
    std::string radioText;      // radio text, up to 64 characters
    std::string ptyText;        // programme type
    bool trafficAlert = false;  // TA: a traffic announcement is on air
    bool trafficProgram = false;// TP: the station carries traffic announcements
    bool music = false;         // M/S flag: music (true) or speech (false)
    int piCode = 0;             // programme identification, 0 = unknown

    std::vector<float> mpxDb;   // spectrum of the multiplex, 0 .. mpxMaxHz, dB relative to full deviation
    float mpxMaxHz = 0;
    std::vector<cf32> rdsConst; // recent RDS symbols after the matched filter (BPSK on the real axis)
};

} // namespace dect2
