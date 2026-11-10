// LoRaWAN with keys the user imported: data frame MIC and FRMPayload (LoRaWAN L2 1.0.x 4.3.3, 4.4) and Class B beacon frames (15.2, 15.3).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace lorawan {

// MIC of a whole data frame (MHDR..FRMPayload). up: uplink
uint32_t dataMic(const uint8_t* phy, size_t n, const uint8_t nwkSKey[16], bool up, uint32_t devAddr, uint32_t fcnt);
// The FRMPayload after decrypting with the session key the port uses (FPort 0 NwkSKey, other AppSKey)
std::vector<uint8_t> decryptFrm(const uint8_t* frm, size_t n, const uint8_t key[16], bool up, uint32_t devAddr, uint32_t fcnt);

struct Beacon {
    bool ok = false, crc1Ok = false;
    uint32_t netId = 0;                    // 1.0.2 format; 0 and the RFU layout for the 1.0.3+ one
    uint32_t timeSec = 0;                  // seconds since 1970 UTC (GPS seconds in 1.0.3+)
    bool newFormat = false;
    bool hasGps = false;
    int infoDesc = -1;
    double lat = 0, lon = 0;
};
// One 17-byte EU868 style beacon: both the 1.0.2 layout (NetID 3, Time 4, CRC-8) and the 1.0.3+ layout (RFU 2, Time 4, CRC-16)
Beacon parseBeacon(const uint8_t* p, size_t n);
uint16_t beaconCrc(const uint8_t* p, size_t n);

} // namespace lorawan
} // namespace dect2
