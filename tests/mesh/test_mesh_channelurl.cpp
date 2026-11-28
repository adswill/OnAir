// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meshtastic channel URLs (round 4 #9): the factory default link, encode/decode round trips over presets, regions, key lengths and options, the
// slot of a private primary channel (djb2 of its name, as the firmware), channel_num and override frequency, damaged links, and an imported key
// decrypting a packet built for that channel.
#include "dect2/mesh/mesh_channel_url.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_pb.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    std::string err;
    // the default channel as the phone app shares it: psk index 1, LoRa use_preset, hop limit 3, transmit on, region unset
    MeshtasticChannelSet d;
    CHECK(meshtasticUrlDecode("https://meshtastic.org/e/#CgMSAQESBggBQANIAQ", d, err), "default link: %s", err.c_str());
    CHECK(d.channels.size() == 1 && d.channels[0].psk == std::vector<uint8_t>{1} && d.channels[0].name.empty() && d.hasLora && d.usePreset && d.modemPreset == 0 && d.hopLimit == 3 &&
          d.txEnabled && d.region == 0, "default link fields");
    CHECK(meshtasticUrlChannelName(d, 0) == "LongFast" && meshtasticUrlChannelKey(d.channels[0]) == "AQ==", "default name and key");
    MeshLoraSettings l;
    CHECK(!meshtasticUrlLora(d, l, err) && err.find("region") != std::string::npos, "no region: said so");
    CHECK(meshtasticUrlEncode(d) == "https://meshtastic.org/e/#CgMSAQESBggBQANIAQ", "re-encoded byte for byte: %s", meshtasticUrlEncode(d).c_str());
    // the same set through the other forms of the link
    MeshtasticChannelSet d2;
    CHECK(meshtasticUrlDecode("  https://meshtastic.org/e/?add=true#CgMSAQESBggBQANIAQ\n", d2, err) && d2.hopLimit == 3, "add=true link");
    CHECK(meshtasticUrlDecode("CgMSAQESBggBQANIAQ==", d2, err) && d2.hopLimit == 3, "bare base64 with padding");

    // round trips: every preset OnAir knows x several regions x key lengths x options
    int n = 0, bad = 0;
    const char* regions[] = {"US", "EU_868", "EU_433", "ANZ", "IN", "LORA_24"};
    for (int pe = 0; pe < 10; pe++) {
        if (meshtasticPresetFromEnum(pe).empty()) continue;
        for (const char* rg : regions) for (int kl : {0, 1, 16, 32}) for (int opt = 0; opt < 4; opt++) {
            MeshtasticChannelSet s;
            MeshtasticUrlChannel c;
            c.name = opt & 1 ? "Kaunas mesh" : "";
            for (int i = 0; i < kl; i++) c.psk.push_back((uint8_t)(i * 37 + 11 + pe));
            c.uplink = opt & 2; c.downlink = opt == 3; c.positionPrecision = opt * 7; c.id = opt ? 0xA1B2C3D4u : 0; c.muted = opt == 2;
            s.channels.push_back(c);
            if (opt & 2) { MeshtasticUrlChannel c2; c2.name = "admin"; c2.psk.assign(32, 0x5A); s.channels.push_back(c2); }
            s.hasLora = true; s.usePreset = true; s.modemPreset = pe; s.region = meshtasticRegionToEnum(rg); s.hopLimit = 3 + opt; s.txEnabled = opt & 1; s.txPower = opt == 3 ? -2 : 20;
            s.frequencyOffset = opt == 2 ? 0.01f : 0; s.channelNum = opt == 3 ? 2 : 0;
            MeshtasticChannelSet r;
            n++;
            if (!meshtasticUrlDecode(meshtasticUrlEncode(s), r, err)) { bad++; continue; }
            bool ok = r.channels.size() == s.channels.size() && r.modemPreset == s.modemPreset && r.region == s.region && r.hopLimit == s.hopLimit && r.txEnabled == s.txEnabled &&
                      r.txPower == s.txPower && r.frequencyOffset == s.frequencyOffset && r.channelNum == s.channelNum && r.usePreset;
            for (size_t i = 0; ok && i < s.channels.size(); i++)
                ok = r.channels[i].name == s.channels[i].name && r.channels[i].psk == s.channels[i].psk && r.channels[i].uplink == s.channels[i].uplink &&
                     r.channels[i].downlink == s.channels[i].downlink && r.channels[i].id == s.channels[i].id && r.channels[i].positionPrecision == s.channels[i].positionPrecision &&
                     r.channels[i].muted == s.channels[i].muted;
            if (!ok) bad++;
        }
    }
    CHECK(bad == 0 && n > 500, "%d round trips, %d differ", n, bad);

    // the slot: an empty primary name gives the preset's slot as OnAir computes it; a private name moves it by the firmware's hash
    MeshtasticChannelSet s;
    s.channels.push_back(MeshtasticUrlChannel());
    s.hasLora = true; s.region = meshtasticRegionToEnum("EU_868"); s.modemPreset = meshtasticPresetToEnum("LongFast");
    CHECK(meshtasticUrlLora(s, l, err) && std::fabs(l.freqHz - meshtasticSlotHz("EU_868", "LongFast")) < 1 && l.sf == 11 && std::fabs(l.bwHz - 250e3) < 1, "EU_868 LongFast %.0f Hz", l.freqHz);
    s.region = meshtasticRegionToEnum("US");
    for (const char* p : {"LongFast", "MediumFast", "ShortFast", "LongSlow"}) {
        s.modemPreset = meshtasticPresetToEnum(p);
        CHECK(meshtasticUrlLora(s, l, err) && std::fabs(l.freqHz - meshtasticSlotHz("US", p)) < 1, "US %s slot", p);
    }
    s.modemPreset = 0;
    s.channels[0].name = "Kaunas mesh";
    double start, end;
    meshtasticRegionRange("US", start, end);
    uint32_t h = 5381; for (unsigned char ch : std::string("Kaunas mesh")) h = h * 33 + ch;
    const double want = start + 125e3 + (double)(h % (uint32_t)std::floor((end - start) / 250e3)) * 250e3;
    CHECK(meshtasticUrlLora(s, l, err) && std::fabs(l.freqHz - want) < 1, "private name slot %.0f (want %.0f)", l.freqHz, want);
    s.channelNum = 20;
    CHECK(meshtasticUrlLora(s, l, err) && std::fabs(l.freqHz - (start + 125e3 + 19 * 250e3)) < 1, "channel_num 20");
    s.overrideFrequency = 906.875f; s.frequencyOffset = 0.002f;
    CHECK(meshtasticUrlLora(s, l, err) && std::fabs(l.freqHz - 906.877e6) < 50, "override frequency plus offset %.0f", l.freqHz);
    s.overrideFrequency = 0; s.frequencyOffset = 0; s.channelNum = 0;
    s.usePreset = false; s.bandwidth = 62; s.spreadFactor = 9; s.codingRate = 6;
    CHECK(meshtasticUrlLora(s, l, err) && std::fabs(l.bwHz - 62500) < 1 && l.sf == 9 && l.cr == 6, "custom modem setting");
    s.spreadFactor = 3;
    CHECK(!meshtasticUrlLora(s, l, err), "bad custom setting refused");
    s.usePreset = true; s.region = 99;
    CHECK(!meshtasticUrlLora(s, l, err), "unknown region refused");

    // damaged links
    MeshtasticChannelSet x;
    CHECK(!meshtasticUrlDecode("https://example.org/", x, err), "no # part");
    CHECK(!meshtasticUrlDecode("https://meshtastic.org/e/#!!!!", x, err), "not base64");
    CHECK(!meshtasticUrlDecode("https://meshtastic.org/e/#CgMSAQ", x, err), "cut short");
    CHECK(!meshtasticUrlDecode("https://meshtastic.org/e/#EgYIAUADSAE", x, err) && err.find("no channels") != std::string::npos, "LoRa only, no channel");
    { MeshtasticChannelSet b; MeshtasticUrlChannel c; c.psk.assign(5, 1); b.channels.push_back(c); CHECK(!meshtasticUrlDecode(meshtasticUrlEncode(b), x, err), "a 5 byte key refused"); }
    for (int cut = 1; cut < 30; cut++) { std::string u = meshtasticUrlEncode(s); u.resize(u.size() - (size_t)cut % (u.size() - 30)); meshtasticUrlDecode(u, x, err); }   // no crash on any cut

    // an imported private channel decrypts a packet built for it
    {
        MeshtasticChannelSet p;
        MeshtasticUrlChannel c;
        c.name = "Friends";
        std::vector<uint8_t> key;
        meshcrypto::hexDecode("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
        c.psk = key;
        p.channels.push_back(c);
        MeshtasticChannelSet q;
        CHECK(meshtasticUrlDecode(meshtasticUrlEncode(p), q, err), "private link");
        MeshProto mp;
        CHECK(mp.addMeshtasticChannel(meshtasticUrlChannelName(q, 0), meshtasticUrlChannelKey(q.channels[0])), "key accepted");
        const auto data = meshtasticData(1, std::vector<uint8_t>{'h', 'i'});
        const auto pk = meshtasticRaw(0x77, 0xFFFFFFFFu, 90, 3, 3, false, "Friends", key, data);
        const MeshRadioInfo radio{};
        const auto r = mp.decodeMeshtastic(pk.data(), pk.size(), radio);
        CHECK(r.packet.decrypted && r.packet.channel == "Friends" && r.messages.size() == 1 && r.messages[0].text == "hi", "decrypted with the imported key");
        MeshtasticUrlChannel open;
        CHECK(meshtasticUrlChannelKey(open) == "AA==" && mp.addMeshtasticChannel("Open", meshtasticUrlChannelKey(open)), "no-encryption channel");
    }
    // channel key files: round trip, bad lines skipped, no duplicates, existing names kept
    {
        std::vector<MeshChannelKey> ks = {{1, "Friends", "AQIDBAUGBwgJCgsMDQ4PEA=="}, {2, "Club room", "00112233445566778899aabbccddeeff"}, {2, "Mesh Cafe", "AAECAwQFBgcICQoLDA0ODw=="}};
        const std::string f = meshKeysFormat(ks);
        size_t sk = 99;
        auto back = meshKeysParse(f, sk);
        CHECK(sk == 0 && back.size() == 3, "round trip: %zu keys, %zu skipped", back.size(), sk);
        CHECK(back[0].proto == 1 && back[0].name == "Friends" && back[0].secret == ks[0].secret, "round trip meshtastic");
        CHECK(back[1].proto == 2 && back[1].name == "Club room" && back[1].secret == ks[1].secret, "round trip name with space, hex key");
        CHECK(meshKeysFormat(back) == f, "format of the parsed keys is the same text");
        const std::string messy = "# my keys\r\n\n"
            "Meshtastic\tFriends\tAQIDBAUGBwgJCgsMDQ4PEA==\r\n"
            "Meshtastic\tNoKey\n"
            "Telegram\tOther\tAQIDBAUGBwgJCgsMDQ4PEA==\n"
            "Meshtastic  Spaced  AQIDBAUGBwgJCgsMDQ4PEA==\n"
            "MeshCore\tBadHex\tzz!!\n"
            "MeshCore\tOk\t0a0b0c0d0e0f\n"
            "  \n"
            "Meshtastic\t\tAQ==\n"
            "Meshtastic\tWithPad\tAQ\n";
        size_t skipped = 0;
        auto rd = meshKeysParse(messy, skipped);
        CHECK(rd.size() == 4 && skipped == 4, "skipping: %zu keys, %zu bad", rd.size(), skipped);
        CHECK(rd[1].name == "Spaced" && rd[2].name == "Ok" && rd[2].proto == 2, "spaced fields and hex key kept");
        std::vector<MeshChannelKey> have = {{1, "Friends", "AAAA"}};
        auto fresh = meshKeysNew(have, rd);
        CHECK(fresh.size() == 3 && fresh[0].name == "Spaced" && have[0].secret == "AAAA", "existing name not added again or replaced");
        auto twice = meshKeysNew({}, {{1, "A", "AQ=="}, {1, "A", "AQ=="}, {2, "A", "AQ=="}});
        CHECK(twice.size() == 2, "repeat inside the file kept once, other protocol kept: %zu", twice.size());
    }
    return testSummary("mesh_channelurl");
}
