// DVB service information beyond the services of the multiplex being received (EN 300 468): the network information table with its transponder lists
// (frequency, polarisation, symbol rate, FEC, orbital position, for satellite, cable and terrestrial delivery), other networks, bouquets (BAT), the services
// of other transport streams (SDT other), the event counts of other transport streams (EIT other), local time offsets (TOT) and the conditional access table (CAT).
// Signalling only: nothing is descrambled. Fed with CRC-checked sections by TsDemux.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dect2 {

struct SiTransponder {
    int onid = 0, tsid = 0;
    int kind = 0;                 // 1 satellite (descriptor 0x43), 2 cable (0x44), 3 terrestrial (0x5A), 4 DVB-T2 (0x7F/0x04), 5 satellite S2X (0x7F/0x17)
    double freqMhz = 0;
    int pol = -1;                 // satellite: 0 linear horizontal, 1 linear vertical, 2 circular left, 3 circular right
    double symbolRateK = 0;       // ksym/s
    int fecInner = -1;            // EN 300 468 table 38 code
    int modulation = -1;          // satellite: 0 auto, 1 QPSK, 2 8PSK, 3 16QAM; cable: 1 16QAM ... 5 256QAM
    bool s2 = false;
    int rollOff = -1;             // satellite S2: 0 0.35, 1 0.25, 2 0.20, 3 reserved
    double orbitDeg = 0;          // satellite: degrees east (negative: west)
    double bandwidthMhz = 0;      // terrestrial
    std::vector<int> services;    // from the service list descriptor of the transport stream loop
    std::vector<uint8_t> cellList;   // payloads of the cell_list_descriptors (0x6C, EN 300 468 6.2.7), concatenated as sent: the transmitter cells (smart5/cell_tx.h)
};

// Which satellite the NIT says this is: the orbital position most of its satellite transponders give. Compared with the carriers found (frequency MHz of each transponder as it
// is at L-band plus the LNB, so give the Ku frequencies): the share of the found carriers the NIT lists says how sure it is, and a poor match with a good number of carriers says the
// dish looks at another satellite than the NIT's, or the LNB is off.
struct SiOrbitGuess { bool valid = false; double orbitDeg = 0; int transponders = 0; int matched = 0, carriers = 0; };
SiOrbitGuess siGuessOrbit(const struct SiSnapshot& s, const std::vector<double>& foundMhz, double tolMhz = 5.0);

struct SiNetwork {
    int id = 0;
    std::string name;
    bool actual = false;          // the network of the multiplex being received (table 0x40), else another (0x41)
    std::vector<SiTransponder> transponders;
};

struct SiBouquet {
    int id = 0;
    std::string name;
    struct Svc { int onid = 0, tsid = 0, sid = 0, type = 0; };
    std::vector<Svc> services;
};

struct SiOtherService { int onid = 0, tsid = 0, sid = 0, type = 0; std::string name, provider; bool freeCa = false; int running = 0; };

struct SiEitOther { int onid = 0, tsid = 0, sid = 0; uint32_t events = 0; int64_t first = 0, last = 0; std::string nowTitle; };

struct SiTimeOffset { std::string country; int region = 0; int offsetMin = 0; int64_t changeAt = 0; int nextOffsetMin = 0; };

struct SiCaSystem { int caSystemId = 0; int emmPid = 0; std::string name; };   // CAT descriptor 0x09

struct SiSnapshot {
    std::vector<SiNetwork> networks;
    std::vector<SiBouquet> bouquets;
    std::vector<SiOtherService> otherServices;
    std::vector<SiEitOther> eitOther;
    std::vector<SiTimeOffset> timeOffsets;
    std::vector<SiCaSystem> ca;
    int64_t totUtc = 0;           // UTC of the last TOT
    uint64_t sections = 0;
    bool empty() const { return networks.empty() && bouquets.empty() && otherServices.empty() && eitOther.empty() && timeOffsets.empty() && ca.empty(); }
};

const char* siPolName(int pol);
const char* siFecName(int fec);
const char* siModName(int kind, int mod);
const char* siCaSystemName(int id);    // "Conax", "Viaccess", ... the family of a CA system id (EN 300 468 / ETSI TR 101 162); "" when not listed

class SiCollector {
public:
    // a CRC-checked section of a PSI/SI PID (table ids 0x01 CAT, 0x40/0x41 NIT, 0x42/0x46 SDT, 0x4A BAT, 0x4F and 0x60..0x6F EIT other, 0x73 TOT)
    void section(int pid, const uint8_t* s, int len);
    SiSnapshot snapshot() const;
    void reset() { *this = SiCollector(); }
private:
    void nit(const uint8_t* s, int len);
    void bat(const uint8_t* s, int len);
    void sdt(const uint8_t* s, int len);
    void eit(const uint8_t* s, int len);
    void tot(const uint8_t* s, int len);
    void cat(const uint8_t* s, int len);
    std::map<int, SiNetwork> networks_;
    std::map<int, SiBouquet> bouquets_;
    std::map<std::tuple<int, int, int>, SiOtherService> others_;
    std::map<std::tuple<int, int, int>, SiEitOther> eit_;
    std::vector<SiTimeOffset> tot_;
    int64_t totUtc_ = 0;
    std::map<int, SiCaSystem> ca_;
    uint64_t sections_ = 0;
};

} // namespace dect2
