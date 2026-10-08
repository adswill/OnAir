// Presets, slot rule and channel hashes of Meshtastic, and the MeshCore settings.
// Sources: meshtastic/firmware src/mesh/MeshRadio.h (modemPresetToParams), RadioInterface.cpp (regions, hash, slot formula),
// meshtastic.org/docs/overview/radio-settings (EU_868 LongFast 869.525 MHz, US LongFast 906.875 MHz), api.meshcore.nz/api/v1/config.
#include "dect2/mesh_proto.h"
#include "dect2/mesh_crypto.h"
#include <cstdio>
#include <cmath>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    MeshLoraSettings s;
    CHECK(meshtasticPreset("LongFast", s) && s.sf == 11 && s.bwHz == 250000 && s.cr == 5 && s.preamble == 16 && s.syncWord == 0x2B && !s.ldro, "LongFast");
    CHECK(meshtasticPreset("longfast", s) && s.name == "LongFast", "case insensitive");
    CHECK(meshtasticPreset("LONG_FAST", s) && s.name == "LongFast", "enum spelling");
    CHECK(meshtasticPreset("MediumFast", s) && s.sf == 9 && s.bwHz == 250000 && s.cr == 5, "MediumFast");
    CHECK(meshtasticPreset("MediumSlow", s) && s.sf == 10 && s.bwHz == 250000 && s.cr == 5, "MediumSlow");
    CHECK(meshtasticPreset("ShortFast", s) && s.sf == 7 && s.bwHz == 250000 && s.cr == 5, "ShortFast");
    CHECK(meshtasticPreset("ShortSlow", s) && s.sf == 8 && s.bwHz == 250000 && s.cr == 5, "ShortSlow");
    CHECK(meshtasticPreset("ShortTurbo", s) && s.sf == 7 && s.bwHz == 500000 && s.cr == 5, "ShortTurbo");
    CHECK(meshtasticPreset("LongSlow", s) && s.sf == 12 && s.bwHz == 125000 && s.cr == 8 && s.ldro, "LongSlow: SF12 125 kHz needs LDRO");
    CHECK(meshtasticPreset("LongModerate", s) && s.sf == 11 && s.bwHz == 125000 && s.cr == 8 && s.ldro, "LongModerate: SF11 125 kHz needs LDRO");
    CHECK(meshtasticPreset("LongMod", s) && s.name == "LongModerate", "display name LongMod");
    CHECK(meshtasticPreset("LongTurbo", s) && s.sf == 11 && s.bwHz == 500000 && s.cr == 8 && !s.ldro, "LongTurbo");
    CHECK(!meshtasticPreset("Nope", s), "unknown preset");

    // low data rate optimisation: symbol time >= 16 ms (RadioLib); SF12/250 and SF11/125 qualify, SF11/250 does not
    CHECK(meshLoraLdro(12, 125000) && meshLoraLdro(11, 125000) && meshLoraLdro(12, 250000) && !meshLoraLdro(11, 250000) && !meshLoraLdro(12, 500000) &&
          !meshLoraLdro(8, 62500) && meshLoraLdro(10, 62500), "ldro rule");

    // frequency slot: published defaults
    CHECK(meshtasticSlotHz("EU_868", "LongFast") == 869525000.0, "EU_868 LongFast %.0f", meshtasticSlotHz("EU_868", "LongFast"));
    CHECK(meshtasticSlotHz("EU868", "LongFast") == 869525000.0, "EU868 spelling");
    CHECK(meshtasticSlotHz("US", "LongFast") == 906875000.0, "US LongFast %.0f", meshtasticSlotHz("US", "LongFast"));
    CHECK(meshtasticSlotHz("Mars", "LongFast") == 0 && meshtasticSlotHz("US", "Nope") == 0, "unknown region and preset");
    // every region and preset: the channel lies inside the band, on a 1/2 bandwidth offset grid
    const char* regions[] = {"US", "EU_433", "EU_868", "CN", "JP", "ANZ", "RU", "KR", "TW", "IN", "NZ_865", "TH", "UA_868", "PH_868", "KZ_863", "BR_902"};
    const char* presets[] = {"LongFast", "LongSlow", "LongModerate", "LongTurbo", "MediumFast", "MediumSlow", "ShortFast", "ShortSlow", "ShortTurbo"};
    int given = 0;
    for (const char* r : regions) for (const char* p : presets) {
        const double f = meshtasticSlotHz(r, p);
        if (f == 0) continue;                                  // the band is narrower than the preset
        given++;
        MeshLoraSettings m; meshtasticPreset(p, m);
        CHECK(f - m.bwHz / 2 >= 400e6 && f < 930e6, "%s %s: %.0f", r, p, f);
    }
    CHECK(given > 100, "slots computed: %d", given);
    { // EU_868 has room for one 250 kHz channel only, so every 250 kHz preset lands on 869.525; a 500 kHz preset does not fit
      CHECK(meshtasticSlotHz("EU_868", "MediumFast") == 869525000.0 && meshtasticSlotHz("EU_868", "ShortTurbo") == 0, "EU_868 single slot"); }

    // channel hash: xor of name and key bytes; LongFast with the default key is 0x08 (the value seen in captures)
    const std::vector<uint8_t> def = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    CHECK(meshtasticChannelHash("LongFast", def) == 0x08, "LongFast hash %02x", meshtasticChannelHash("LongFast", def));
    // key expansion (Channels.cpp getKey)
    std::vector<uint8_t> k;
    CHECK(meshtasticExpandPsk({1}, k) && k == def, "psk index 1 is the default key");
    CHECK(meshtasticExpandPsk({3}, k) && k.size() == 16 && k[15] == 0x03 && k[0] == 0xd4, "psk index 3 raises the last byte by 2");
    CHECK(meshtasticExpandPsk({0}, k) && k.empty(), "psk 0 = no encryption");
    CHECK(meshtasticExpandPsk({1, 2, 3}, k) && k.size() == 16 && k[0] == 1 && k[3] == 0, "short key padded to 16");
    CHECK(meshtasticExpandPsk(std::vector<uint8_t>(20, 7), k) && k.size() == 32 && k[19] == 7 && k[20] == 0, "20 byte key padded to 32");
    CHECK(!meshtasticExpandPsk(std::vector<uint8_t>(33, 1), k), "33 byte key rejected");
    // the default key as base64 in the published form
    std::vector<uint8_t> raw;
    CHECK(meshcrypto::base64Decode("1PG7OiApB1nwvP+rz05pAQ==", raw) && raw == def, "default PSK base64");

    // MeshCore community presets (api.meshcore.nz/api/v1/config) and the radio code (sync 0x12, preamble 32 up to SF8)
    MeshLoraSettings e = meshcoreDefaults("EU");
    CHECK(e.freqHz == 869618000.0 && e.sf == 8 && e.bwHz == 62500 && e.cr == 8 && e.syncWord == 0x12 && e.preamble == 32 && !e.ldro, "MeshCore EU");
    MeshLoraSettings u = meshcoreDefaults("US");
    CHECK(u.freqHz == 910525000.0 && u.sf == 7 && u.bwHz == 62500 && u.cr == 5 && u.preamble == 32, "MeshCore US");
    MeshLoraSettings d = meshcoreDefaults("EU_DEPRECATED");
    CHECK(d.freqHz == 869525000.0 && d.sf == 11 && d.bwHz == 250000 && d.cr == 5 && d.preamble == 16, "MeshCore old EU default");
    CHECK(meshcoreDefaults("nowhere").freqHz == 869618000.0, "unknown region falls back to EU");

    // hardware model names from mesh.proto
    CHECK(meshtasticHwModelName(4) == "TBEAM" && meshtasticHwModelName(9) == "RAK4631" && meshtasticHwModelName(43) == "HELTEC_V3" &&
          meshtasticHwModelName(0) == "UNSET" && meshtasticHwModelName(255) == "PRIVATE_HW" && meshtasticHwModelName(1000) == "HW_1000", "hw names");
    CHECK(meshtasticHwModelValue("TBEAM") == 4 && meshtasticHwModelValue("NOPE") == -1, "hw values");
    CHECK(meshtasticNodeIdString(0x1a2b3c4d) == "!1a2b3c4d" && meshtasticNodeIdString(0x929) == "!00000929", "node id string");

    if (fails) return 1;
    printf("mesh_proto_settings ok\n");
    return 0;
}
