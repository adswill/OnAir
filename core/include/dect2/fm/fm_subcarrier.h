// The rest of the FM multiplex: the three RDS2 data subcarriers (66.5, 71.25 and 76 kHz, ITU-R BS.643-4) and the analogue SCA audio subcarriers (67 and 92 kHz).
// RDS2: the same 1187.5 bit/s biphase signal as RDS on 57 kHz: blocks, checkwords and raw groups. The tunnelling and RFT file layers (IEC 62106-2 Ed2) are not
// public and are not decoded. SCA: narrow FM audio.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Rds2Stream {
    double carrierHz = 0;
    bool synced = false;
    float okPct = 0;
    uint64_t groups = 0;
    std::vector<std::string> lastGroups;   // raw groups, newest last, as redsea hex
};
struct ScaChannel {
    double carrierHz = 0;
    bool present = false;
    float snrDb = 0;                       // the band against the noise floor above the multiplex
};
struct FmSubcarrierStatus {
    Rds2Stream rds2[3];
    ScaChannel sca[2];
};

class FmSubcarriers {
public:
    FmSubcarriers();
    ~FmSubcarriers();
    void configure(double mpxRate);        // at least 200 kHz
    void reset();
    void feed(const float* mpx, size_t n); // the multiplex, full deviation = 1 (75 kHz)
    FmSubcarrierStatus status() const;
    // 48 kHz mono audio of one SCA channel (0 or 1) since the last call; pass -1 for none.
    void setScaListen(int channel);
    int scaListen() const;
    void takeScaAudio(std::vector<float>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
