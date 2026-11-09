// BGAN forward-bearer signalling (ETSI TS 102 744-3-1): the broadcast Bearer Control PDUs of a FEC block, to plain language.
// Only broadcast system information is read. PDUs addressed to one terminal (common-signalling addresses carrying IMSI / TMSI / IMEI, connection PDUs,
// ConnPDU, SpecificAVPList, StatusAckList) are counted and never parsed or stored; no user data and no subscriber identity leaves this file.
// The generator side (buildBlock*) makes synthetic blocks for the test signal only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::bgan {

uint16_t crcX25(const uint8_t* d, size_t n);                  // CRC-16 of the Bearer Control PDU, ones' complement, low byte first when appended
double forwardMhz(int offsetBit, int channelIndex);           // forward frequency = index x 0.0025 + offset x 0.00125 + 1510 MHz

struct SatLoc { int id = -1; double lonDeg = 0; double primaryMhz = 0, alternateMhz = 0; };
struct PrimaryBearer { int spot = 0; double mhz = 0; int fbType = 0; int fBearer = 0; };
struct Plmn { int mcc = 0, mnc = 0; int mncDigits = 2; };

// what one carrier has said so far
struct CarrierSig {
    bool haveBb = false;
    int rnc = -1, netVer = -1, frameNo = -1, fBearer = -1, bctId = -1, spotBeam = -1;
    int beamType = -1;                                        // BeamInfo: 0 global, 1 regional, 2 narrow; -1 not heard
    int beamSpot = -1;                                        // spot id from BeamInfo
    std::vector<SatLoc> sats;                                 // the first entry is the satellite this bearer comes from
    std::map<int, PrimaryBearer> primaries;                   // by spot-beam id
    std::vector<Plmn> plmns;
    bool haveUtc = false;
    int utcY = 0, utcMo = 0, utcD = 0, utcH = 0, utcMi = 0, utcS = 0, utcMs = 0;   // GPS time of frame number 0
    int stateX = 0, stateY = 0, stateZ = 0;  bool haveState = false;               // satellite state vector in units of 488 m
    double returnMhz = 0;                                     // return channel of the last return schedule
    int returnBearer = -1;                                    // ReturnBearerTypeShort, -1 not heard
    uint32_t schedReservedSum = 0, schedSlotsSum = 0;         // reserved and total short slots over all return schedules
    uint32_t schedules = 0, connectionsListed = 0;            // return schedules heard; connection slots they listed
    int spotMapTotal = 0, spotMapVersion = -1, bearerTableEntries = 0, gpsEphPrn = -1;
    std::string sysInfoIndex;                                 // the catalogue of the system information broadcast here
    uint64_t broadcastPdus = 0, connectionPdus = 0, addressedPdus = 0, badPdus = 0;
    int nextCodeRate[16] = {};                                // per FEC block, subtype of the next frame (X4/16); -1 = same as the UW says
    int nextCodeMod16[16] = {};                               // 1 = this block is 16-QAM in the next frame
    bool haveNextCode = false;
    CarrierSig() { for (int i = 0; i < 16; i++) { nextCodeRate[i] = -1; nextCodeMod16[i] = 0; } }
};

// parse one FEC block's octets, updating `s`; lines (optional) receive one plain-language line per broadcast PDU.
// Returns the kind of block: 1 at least one valid CRC, 2 all zero (empty), 0 no good PDU (decoder failure, or it is not this subtype)
int parseBlock(const uint8_t* d, size_t n, CarrierSig& s, std::vector<std::string>* lines);
// quick check used to stop the turbo decoder: true when the octets begin with a PDU whose CRC is good, or are all zero
bool blockLooksValid(const uint8_t* d, size_t n);

std::string describeBeamType(int t);
std::string fbearerTypeName(int t);
std::string returnBearerTypeName(int t);
std::string describeSatellite(double lonDeg);                 // "Inmarsat-4 F3 (98 W)" style name, or the longitude when not known

// ---- test signal ----
struct TxSig {
    int rnc = 7, netVer = 2, bctId = 3, spotBeam = 42, fBearer = 1, beamType = 2;
    int satId = 3; double lonDeg = 24.9;
    int mcc = 901, mnc = 11;
    double primaryMhz = 1545.0;
    bool emptyLaterBlocks = false;
};
// FEC block `block` of frame `frameNo` (12 bits); `bytes` octets of content, zero padded. Block 0 carries the bulletin board and AVPs.
void buildBlock(const TxSig& tx, int frameNo, int block, std::vector<uint8_t>& out, size_t bytes);

} // namespace dect2::bgan
