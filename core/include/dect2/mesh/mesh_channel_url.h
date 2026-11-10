// Meshtastic channel URLs (https://meshtastic.org/e/#<base64url ChannelSet>), the links and QR codes the phone app and the web client share.
// ChannelSet (apponly.proto): 1 repeated ChannelSettings, 2 Config.LoRaConfig. ChannelSettings (channel.proto): 1 channel_num, 2 psk, 3 name, 4 id (fixed32),
// 5 uplink_enabled, 6 downlink_enabled, 7 module_settings {1 position_precision, 2 is_muted}. LoRaConfig (config.proto): 1 use_preset, 2 modem_preset,
// 3 bandwidth, 4 spread_factor, 5 coding_rate, 6 frequency_offset (float), 7 region, 8 hop_limit, 9 tx_enabled, 10 tx_power, 11 channel_num,
// 12 override_duty_cycle, 13 sx126x_rx_boosted_gain, 14 override_frequency (float). Checked against meshtastic/protobufs master, 2026-11-10.
#pragma once
#include "dect2/mesh/mesh_proto.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct MeshtasticUrlChannel {
    std::string name;                 // empty on the primary channel: the modem preset's name is used
    std::vector<uint8_t> psk;         // 0 bytes no encryption, 1 byte a built-in key index, 16 or 32 bytes AES
    uint32_t id = 0;
    bool uplink = false, downlink = false;
    uint32_t positionPrecision = 0;
    bool muted = false;
};

struct MeshtasticChannelSet {
    std::vector<MeshtasticUrlChannel> channels;   // the first is the primary
    bool hasLora = false;
    bool usePreset = true;
    int modemPreset = 0;              // ModemPreset enum (0 LONG_FAST ...)
    int region = 0;                   // RegionCode enum (0 UNSET, 3 EU_868 ...)
    uint32_t bandwidth = 0, spreadFactor = 0, codingRate = 0;   // when not using a preset (bandwidth in kHz, 31 = 31.25 ...)
    float frequencyOffset = 0, overrideFrequency = 0;           // MHz
    uint32_t hopLimit = 0, channelNum = 0;                      // channel_num: 1-based slot, 0 = from the primary channel name
    bool txEnabled = false;
    int32_t txPower = 0;
};

// url: the full link (also with ?add=true), the part after '#', or the bare base64 (URL or standard alphabet). false with err when it is not a channel set.
bool meshtasticUrlDecode(const std::string& url, MeshtasticChannelSet& out, std::string& err);
// the link the app shares: https://meshtastic.org/e/#<base64url, no padding>
std::string meshtasticUrlEncode(const MeshtasticChannelSet& s);

// enum to OnAir's names: "LongFast" ..., "EU_868" ...; "" when OnAir has no such preset / region
std::string meshtasticPresetFromEnum(int e);
int meshtasticPresetToEnum(const std::string& name);    // -1 unknown
std::string meshtasticRegionFromEnum(int e);
int meshtasticRegionToEnum(const std::string& name);    // -1 unknown

// The channel name as the firmware uses it (the preset's name when empty) and the key as base64 for the channel list
std::string meshtasticUrlChannelName(const MeshtasticChannelSet& s, size_t i);
std::string meshtasticUrlChannelKey(const MeshtasticUrlChannel& c);
// The radio setting the set asks for: frequency (override, else the slot of the primary channel name or channel_num, plus the offset), preset or
// custom bandwidth, spreading factor and coding rate. False with err when the region or preset is not known or has no slot.
bool meshtasticUrlLora(const MeshtasticChannelSet& s, MeshLoraSettings& out, std::string& err);

}  // namespace dect2
