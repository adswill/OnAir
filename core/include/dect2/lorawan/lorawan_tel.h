// LoRaWAN monitor telemetry, shared by the receiver and the UI. The first members are copied into RxTelemetry (the top bar and the history plot):
// keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kLwLogMax = 600;
constexpr size_t kLwDevMax = 300;

struct LorawanSettings {
    int plan = 0;                 // lorawan::Plan: EU868, US915, AU915, AS923-1..4, EU433
    int subBand = 0;              // US915 / AU915: 0 all 64 + 8 channels, 1..8 one sub-band
    bool downlinks = true;        // also the gateways' frames (inverted IQ, and the downlink-only channels of the plan)
    bool defaultsOnly = false;    // only the plan's default (join) channels
    bool privateSync = false;     // also sync word 0x12 (private networks that use it); the public LoRaWAN word is 0x34
    int maxChannels = 24;         // demodulator sets run at most (the ones nearest the radio's centre first): the CPU limit
    bool include250_500 = true;   // the 250 kHz (DR6) and 500 kHz channels of the plan
    bool fsk = true;              // also the plan's FSK channel (DR7, 50 kbit/s GFSK: EU868 868.8 MHz, AS923-1 924.8 MHz)
};

struct LwFrame {
    int64_t wallTime = 0;
    double sec = 0;               // signal time of the frame's start
    double freqHz = 0;            // channel centre (absolute when the radio's centre is known, else relative to the dial)
    double bwHz = 0;
    int chan = 0;                 // channel number in the plan's list (ascending frequency), -1 unknown
    int sf = 0;                   // 0 for an FSK frame
    bool fsk = false;             // a LoRaWAN FSK frame (DR7, 50 kbit/s GFSK, lorawan_fsk.h); bwHz is then the bit rate
    bool inverted = false;        // inverted IQ: sent by a gateway
    bool hasCrc = false, crcOk = false;
    float snrDb = 0, levelDb = 0; // SNR in the LoRa bandwidth; level in dB re full scale (the "RSSI" column: not calibrated to dBm)
    double cfoHz = 0;
    int len = 0;
    int corrected = 0;
    // LoRaWAN MAC layer (never decrypted)
    bool lorawan = false;         // parsed as a LoRaWAN PHY payload
    int mtype = -1;
    bool uplink = true;
    uint32_t devAddr = 0;
    bool hasDevAddr = false;
    int addrType = -1;
    uint32_t nwkId = 0;
    uint16_t fcnt = 0;
    int fport = -1;
    int frmLen = 0;
    uint32_t mic = 0;
    uint64_t devEui = 0, joinEui = 0;
    uint16_t devNonce = 0;
    std::string type;             // "Unconfirmed up", "Join request", ...
    std::string flags;            // "ADR, ACK"
    std::string mac;              // MAC commands in FOpts, comma separated, with values
    std::string summary;
    std::string hex;              // the whole PHY payload
};

struct LwDevice {
    std::string id;               // DevAddr (8 hex digits) or "DevEUI 70B3..." until a join gives it an address
    uint32_t devAddr = 0;
    bool hasDevAddr = false;
    std::string devEui, joinEui;
    double lastHeard = -1;        // signal seconds
    double freqHz = 0;
    int sf = 0;
    bool fsk = false;             // last heard on the FSK channel (DR7)
    float lastSnrDb = 0, lastLevelDb = 0;
    long fcnt = -1;               // last 16-bit counter
    uint32_t packets = 0, uplinks = 0, downlinks = 0, joins = 0, confirmed = 0, missed = 0;   // missed: gaps in the 16-bit counter
    int lastFport = -1;
    std::string lastType, lastMac;
};

struct LwChannelInfo {
    double freqHz = 0;
    double bwHz = 0;
    int sfMin = 0, sfMax = 0;
    bool downlinkOnly = false;
    bool fsk = false;             // the plan's FSK (DR7) channel; sfMin / sfMax 0
    bool inBand = false;
    uint64_t preambles = 0, frames = 0, crcBad = 0;
};

struct LorawanTelemetry {
    uint64_t seq = 0;
    int state = 0;                // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;             // carrier offset of the last good frame
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;        // frames with a good CRC (or no CRC: downlinks)
    uint64_t blocksBad = 0;
    // details
    double timeSec = 0, inputRate = 0, centerHz = 0;
    LorawanSettings cfg;
    bool centreKnown = false;     // the radio's centre frequency is known (a real radio); else the plan's own centre is assumed
    uint64_t preambles = 0, headerBad = 0;
    uint64_t fskSyncs = 0, fskFrames = 0, fskCrcBad = 0;   // the FSK channel(s): strong sync words, good frames, CRC errors after a strong sync
    uint64_t perSf[13] = {};
    uint64_t perType[8] = {};     // by MType
    uint32_t channelsInBand = 0, channelsListed = 0, demodulators = 0;
    std::string note;             // why channels are missing ("12 of 72 channels fit the captured band; sample rate 2 Msps")
    std::vector<LwChannelInfo> channels;
    std::vector<LwFrame> log;     // newest first, at most kLwLogMax
    std::vector<LwDevice> devices;
};

inline std::string lorawanSummary(const LorawanTelemetry& t) {
    char b[160];
    if (t.blocksOk + t.blocksBad == 0) {
        snprintf(b, sizeof b, "listening on %u channels (%u demodulators)", t.channelsInBand, t.demodulators);
        return b;
    }
    snprintf(b, sizeof b, "%llu frames, %zu devices, %u channels", (unsigned long long)t.blocksOk, t.devices.size(), t.channelsInBand);
    return b;
}

} // namespace dect2
