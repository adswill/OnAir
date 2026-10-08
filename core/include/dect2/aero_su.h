// Inmarsat Aero signal-unit layer (no radio): the decoded bits of one P-channel block -> signal units (SUs) ->
// logons and ACARS messages, and blocks for the test signal.
// Boundary with the demodulator: the information bits of one interleaver block after deinterleaving, Viterbi decoding
// and descrambling, as JAERO's AeroL hands them to its SU parsing, packed into bytes with the first decoded bit in bit 0 (LSB first; aeroBlockBytes(rate) bytes).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AeroSu {
    uint8_t type = 0;                   // SU type octet
    std::string typeName;
    uint8_t bytes[12] = {};             // the whole SU as sent
    bool crcOk = false;
    uint32_t aesId = 0;                 // ICAO 24-bit address when the SU carries one, else 0
    int gesId = -1;
};

size_t aeroBlockBytes(int bitRate);                                       // information bytes per block at 600 / 1200 / 10500 bit/s; 0 = unknown rate
std::vector<AeroSu> aeroSplitSus(const uint8_t* block, size_t nBytes, int bitRate);

struct AeroAcars {
    uint32_t aesId = 0;
    int gesId = -1;
    bool uplink = true;
    std::string mode, registration, label, labelText, blockId, msgNo, flight, text;
    bool crcOk = false;
};
struct AeroLogon {
    uint32_t aesId = 0;
    int gesId = -1;
    bool logon = true;                  // false: logoff
};

// Reassembles ACARS messages from the user-data SUs (they span several SUs and blocks).
class AeroSuDecoder {
public:
    AeroSuDecoder();
    ~AeroSuDecoder();
    void feed(const AeroSu& su, double timeSec, std::vector<AeroAcars>& acars, std::vector<AeroLogon>& logons);
    void reset();
private:
    struct State;
    std::unique_ptr<State> s_;
};

// Builders for the test signal
std::vector<AeroSu> aeroAcarsToSus(const AeroAcars& m);                  // an uplink ACARS message as the SUs that carry it
AeroSu aeroLogonSu(uint32_t aesId, int gesId, bool logon);
AeroSu aeroSystemTableSu(int gesId, int part);
std::vector<uint8_t> aeroBuildBlock(const std::vector<AeroSu>& sus, int bitRate);   // one block of aeroBlockBytes(rate) bytes, padded with fill-in SUs

// ---- Additions of the SU layer (the declarations above are unchanged) ----
// SU: 12 bytes = 10 bytes of message + CRC (CRC-16/IBM-SDLC "X-25" as JAERO's AeroLcrc16: init 0xFFFF, reflected 0x8408,
// output inverted) stored low byte first at bytes 10 and 11. Block sizes: channel bits per interleaver block 384 / 576 /
// 4992 at 600 / 1200 / 10500 bit/s, rate 1/2 coding, so 24 / 36 / 312 information bytes = 2 / 3 / 26 SUs.
uint16_t aeroSuCrc(const uint8_t* p, size_t n);          // CRC of n bytes; check value of "123456789" is 0x906E
size_t aeroSusPerBlock(int bitRate);                     // SUs per block (0 = unknown rate)
std::string aeroDescribeSu(const AeroSu& su);            // one line: type name plus the decoded fields of the SU types known

// Decoded system information (JAERO's reading of the P channel system table SUs).
struct AeroSysInfo {
    int kind = 0;                       // 0 none, 1 GES channels (0x05), 2 satellite id (0x0C), 3 P/R channel control (0x40)
    int gesId = -1;
    int seq = 0;
    int lsu = 0;                        // kind 1: which group of three channels this SU carries
    int satId = -1;                     // kind 2
    double lonDeg = 0;                  // kind 2, east positive, -180..180
    int bitRate = 0;                    // kind 3: bit/s, 0 when the code is not known
    double freqMHz[3] = {0, 0, 0};      // kind 1: three channels in the order given in `names`; kind 2: Psmc1, Psmc2; kind 3: [0] = P channel
    bool spotBeam = false;
    std::string names;                  // kind 1: what freqMHz[0..2] are, e.g. "Psmc Rsmc0 Rsmc1"
};
AeroSysInfo aeroParseSystemSu(const AeroSu& su);

// Builders for the same SUs (frequency grid 2.5 kHz from 1510 MHz, transmit channels +101.5 MHz, as JAERO reads them)
AeroSu aeroSatelliteIdSu(int satId, double lonDegEast, double psmc1MHz, double psmc2MHz, int seq = 0);
AeroSu aeroChannelControlSu(int gesId, int bitRate, double pChannelMHz, bool spotBeam = false);
AeroSu aeroFillSu();                                     // fill-in SU (type 0x01)
// ACARS uplink with explicit ISU numbers (the one-argument builder derives them from the message)
std::vector<AeroSu> aeroAcarsToSus(const AeroAcars& m, int qno, int refno);

} // namespace dect2
