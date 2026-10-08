// Meshtastic packet layer: header, AES-CTR, Data and the port payloads, builders and decoders.
// Published vectors: meshtastic/firmware test/test_crypto/test_main.cpp, test_PKC (a real packet header and the decrypted Data
// "08011204746573744800" = TEXT_MESSAGE_APP "test"). The ciphertext of that Data under the default key was computed with
// OpenSSL 3.6.4 (openssl enc -aes-128-ctr) from the nonce layout in CryptoEngine.cpp, independent of this code.
// Hand-encoded messages follow the field numbers of meshtastic/protobufs mesh.proto and telemetry.proto.
#include "dect2/mesh_proto.h"
#include "dect2/mesh_crypto.h"
#include "dect2/mesh_pb.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::vector<uint8_t> H(const char* s) { std::vector<uint8_t> v; meshcrypto::hexDecode(s, v); return v; }
static std::string hex(const std::vector<uint8_t>& v) { return meshcrypto::hexEncode(v.data(), v.size()); }
static const MeshRadioInfo kRadio{};
static const std::vector<uint8_t> kDef = H("d4f1bb3a20290759f0bcffabcf4e6901");

static MeshtasticNode node(uint32_t num, const char* l, const char* s, int hw, double lat, double lon, double alt) {
    MeshtasticNode n; n.num = num; n.longName = l; n.shortName = s; n.hwModel = hw; n.lat = lat; n.lon = lon; n.altM = alt; return n;
}

int main() {
    MeshProto mp;

    { // the packet from the firmware's PKC test: header fields and "encrypted (direct)"
        const auto pkt = H("8c646d7a2909000062d6b2136b00000040df24abfcc30a17a3d9046726099e796a1c036a792b");
        auto r = mp.decodeMeshtastic(pkt.data(), pkt.size(), kRadio);
        CHECK(r.ok && !r.packet.decrypted, "pkc parsed, not decrypted");
        CHECK(r.packet.to == "!7a6d648c" && r.packet.from == "!00000929" && r.packet.packetId == 0x13b2d662, "pkc ids %s %s %08x", r.packet.to.c_str(), r.packet.from.c_str(), r.packet.packetId);
        CHECK(r.packet.hopLimit == 3 && r.packet.hopStart == 3 && r.packet.wantAck && !r.packet.viaMqtt && r.packet.channelHash == 0, "pkc flags 0x6b");
        CHECK(r.packet.note == "encrypted (direct)", "pkc note '%s'", r.packet.note.c_str());
        CHECK(r.packet.size == pkt.size() && r.messages.empty() && r.nodes.empty(), "pkc outputs");
    }
    { // the same header with the published plaintext Data, encrypted with the default key (OpenSSL vector)
        const auto want = H("8c646d7a2909000062d6b2136b08002946f2ecd0b02ac4cf2be5");
        auto got = meshtasticRaw(0x929, 0x7a6d648c, 0x13b2d662, 3, 3, true, "LongFast", kDef, H("08011204746573744800"));
        CHECK(got == want, "default-key packet %s", hex(got).c_str());
        auto r = mp.decodeMeshtastic(want.data(), want.size(), kRadio);
        CHECK(r.ok && r.packet.decrypted && r.packet.type == "TEXT_MESSAGE_APP" && r.packet.channel == "LongFast", "decode type %s", r.packet.type.c_str());
        CHECK(r.messages.size() == 1 && r.messages[0].text == "test" && r.messages[0].from == "!00000929" && r.messages[0].to == "!7a6d648c" &&
              r.messages[0].hops == 0 && r.messages[0].channel == "LongFast", "decoded text");
        CHECK(r.packet.relayNode == 0x29 && r.packet.nextHop == 0 && r.packet.channelHash == 0x08, "relay and channel bytes");
    }

    const MeshtasticNode a = node(0x1a2b3c4d, "Dubai Marina Base", "DMB1", 4, 25.0805, 55.1403, 12);
    const MeshtasticNode b = node(0x99aabbcc, "Palm Heltec", "PLM2", 43, -33.8688, -151.2093 + 300, -5);   // negative lat and altitude
    { // text, broadcast and direct, hops, reply
        auto p = meshtasticText(a, 0xFFFFFFFFu, 0x1234, 2, 3, "Hello mesh, tést ✓");
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.decrypted && r.packet.to == "^all" && r.packet.hopLimit == 2 && r.packet.hopStart == 3, "text header");
        CHECK(r.messages.size() == 1 && r.messages[0].text == "Hello mesh, tést ✓" && r.messages[0].hops == 1 && r.messages[0].to == "^all", "text content");
        p = meshtasticTextEx(b, a.num, 0x1235, 3, 3, "ok", 0x1234, true);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.messages.size() == 1 && r.messages[0].replyId == 0x1234 && r.messages[0].to == "!1a2b3c4d" && r.packet.wantAck, "reply");
        CHECK(r.packet.detail.find("(reply)") != std::string::npos, "reply detail");
    }
    { // node info
        auto p = meshtasticNodeInfo(a, 0x2001, 3, 3);
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.type == "NODEINFO_APP" && r.nodes.size() == 1, "nodeinfo");
        if (r.nodes.size() == 1) {
            const auto& n = r.nodes[0];
            CHECK(n.nodeId == "!1a2b3c4d" && n.longName == "Dubai Marina Base" && n.shortName == "DMB1" && n.hwModel == "TBEAM" && n.role == "CLIENT", "nodeinfo fields %s %s", n.hwModel.c_str(), n.role.c_str());
        }
        MeshtasticNode rt = b; rt.role = 2;
        p = meshtasticNodeInfo(rt, 0x2002, 3, 3);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].hwModel == "HELTEC_V3" && r.nodes[0].role == "ROUTER", "router role");
    }
    { // position, full precision and 13 bit precision
        auto p = meshtasticPosition(a, 0x3001, 3, 3, 1760000000);
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.type == "POSITION_APP" && r.nodes.size() == 1 && r.nodes[0].hasPosition, "position");
        if (r.nodes.size() == 1) {
            const auto& n = r.nodes[0];
            CHECK(std::abs(n.lat - 25.0805) < 1e-6 && std::abs(n.lon - 55.1403) < 1e-6 && n.altM == 12 && n.positionTime == 1760000000u, "position values %.7f %.7f", n.lat, n.lon);
        }
        p = meshtasticPosition(b, 0x3002, 3, 3, 1760000100);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && std::abs(r.nodes[0].lat + 33.8688) < 1e-6 && std::abs(r.nodes[0].lon - (-151.2093 + 300)) < 1e-6 && r.nodes[0].altM == -5, "negative coordinates and altitude");
        p = meshtasticPositionEx(a, 0x3003, 3, 3, 1760000000, 13);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && std::abs(r.nodes[0].lat - 25.0805) < 0.03 && std::abs(r.nodes[0].lon - 55.1403) < 0.03 && std::abs(r.nodes[0].lat - 25.0805) > 1e-5, "13 bit precision cell centre");
        CHECK(r.packet.detail.find("13 bit") != std::string::npos, "precision in detail: %s", r.packet.detail.c_str());
        MeshtasticNode nofix = a; nofix.lat = 0; nofix.lon = 0;
        p = meshtasticPosition(nofix, 0x3004, 3, 3, 0);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.nodes.empty(), "0,0 is no fix");
    }
    { // hand-encoded Position and User (field numbers from mesh.proto), carried in Data with the default key
        // Position { latitude_i 252048000 (1), longitude_i 552708000 (2), altitude 20 (3), time 1760000000 (4) }
        const auto pos = H("0d80f2050f15a0a7f1201814250078e768");
        auto p = meshtasticRaw(0x55, 0xFFFFFFFFu, 77, 3, 3, false, "LongFast", kDef, meshtasticData(3, pos));
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && std::abs(r.nodes[0].lat - 25.2048) < 1e-9 && std::abs(r.nodes[0].lon - 55.2708) < 1e-9 && r.nodes[0].altM == 20 && r.nodes[0].positionTime == 1760000000u, "hand-encoded position");
        // User { id "!deadbeef" (1), long_name "Alice" (2), short_name "AL" (3), hw_model 43 (5) }
        const auto user = H("0a092164656164626565661205416c6963651a02414c282b");
        p = meshtasticRaw(0xdeadbeef, 0xFFFFFFFFu, 78, 3, 3, false, "LongFast", kDef, meshtasticData(4, user));
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].longName == "Alice" && r.nodes[0].shortName == "AL" && r.nodes[0].hwModel == "HELTEC_V3" && r.nodes[0].nodeId == "!deadbeef", "hand-encoded user");
        // a User with an unknown field and a public key still decodes: id, names, hw 43, unknown field 99 varint, role 4 (REPEATER)
        const auto user2 = H("0a092164656164626565661205416c6963651a02414c282b" "9806" "01" "3804");
        p = meshtasticRaw(0xdeadbeef, 0xFFFFFFFFu, 79, 3, 3, false, "LongFast", kDef, meshtasticData(4, user2));
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].role == "REPEATER", "unknown field skipped, role %s", r.nodes.empty() ? "" : r.nodes[0].role.c_str());
    }
    { // telemetry
        auto p = meshtasticTelemetry(a, 0x4001, 3, 3, 87, 4.07, 12.5, 1.25, 86400);
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.type == "TELEMETRY_APP" && r.nodes.size() == 1, "telemetry");
        if (r.nodes.size() == 1) {
            const auto& n = r.nodes[0];
            CHECK(n.batteryPct == 87 && std::abs(n.voltage - 4.07) < 1e-5 && std::abs(n.channelUtilPct - 12.5) < 1e-5 && std::abs(n.airUtilTxPct - 1.25) < 1e-5 && n.uptimeS == 86400, "device metrics");
        }
        p = meshtasticTelemetry(a, 0x4002, 3, 3, 101, 5.0, 0, 0, 5);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].batteryPct == 100 && r.packet.detail.find("powered") != std::string::npos, "powered: %s", r.packet.detail.c_str());
        p = meshtasticEnvTelemetry(a, 0x4003, 3, 3, 31.5, 48, 1005.5);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].hasEnv && std::abs(r.nodes[0].tempC - 31.5) < 1e-5 && std::abs(r.nodes[0].humidity - 48) < 1e-5 && std::abs(r.nodes[0].pressureHpa - 1005.5) < 1e-3, "environment");
        // Telemetry { time (1) 1760000000, device_metrics (2) { battery_level 55 (1), voltage 3.5 (2) } } by hand
        const auto tel = H("0d0078e768" "1207" "0837" "1500006040");
        p = meshtasticRaw(0x66, 0xFFFFFFFFu, 80, 3, 3, false, "LongFast", kDef, meshtasticData(67, tel));
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.nodes.size() == 1 && r.nodes[0].batteryPct == 55 && std::abs(r.nodes[0].voltage - 3.5) < 1e-6, "hand-encoded telemetry");
    }
    { // routing ack, traceroute, neighbour info
        auto p = meshtasticRoutingAck(b, a.num, 0x5001, 3, 3, 0x1234, 0);
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && r.packet.decrypted && r.packet.type == "ROUTING_APP" && r.packet.detail == "ack of 00001234", "ack '%s'", r.packet.detail.c_str());
        p = meshtasticRoutingAck(b, a.num, 0x5002, 3, 3, 0x1234, 1);
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.detail.find("NO_ROUTE") != std::string::npos, "error name: %s", r.packet.detail.c_str());
        p = meshtasticTraceroute(a, b.num, 0x5003, 3, 3, false, 0, {0x11223344}, {-8}, {}, {});
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.type == "TRACEROUTE_APP" && r.packet.detail.find("!11223344") != std::string::npos && r.packet.detail.find("request") != std::string::npos, "traceroute request: %s", r.packet.detail.c_str());
        p = meshtasticTraceroute(b, a.num, 0x5004, 3, 3, true, 0x5003, {0x11223344, 0x55667788}, {-8, 10}, {0x55667788}, {6});
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.detail.find("reply") != std::string::npos && r.packet.detail.find("-2.0") != std::string::npos && r.packet.detail.find("2.5") != std::string::npos, "traceroute reply: %s", r.packet.detail.c_str());
        p = meshtasticNeighborInfo(a, 0x5005, 3, 3, 900, {{0x11223344, 5.5f}, {0x55667788, -3.25f}});
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.type == "NEIGHBORINFO_APP" && r.packet.detail.find("2 neighbors") != std::string::npos && r.packet.detail.find("!11223344 5.5 dB") != std::string::npos, "neighbors: %s", r.packet.detail.c_str());
    }
    { // user channels: key given as base64, short index, and no encryption
        const auto key = H("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        const std::string b64 = meshcrypto::base64Encode(key.data(), key.size());
        auto data = meshtasticData(1, std::vector<uint8_t>{'s', 'e', 'c', 'r', 'e', 't'});
        auto p = meshtasticRaw(0x77, 0xFFFFFFFFu, 90, 3, 3, false, "Friends", key, data);
        // AES-256-CTR vector computed with OpenSSL 3.6.4 (openssl enc -aes-256-ctr) for this packet, independent of this code
        CHECK(hex(p) == "ffffffff770000005a000000634100779a4823bfc6429c7c74ac25a2", "aes256 packet %s", hex(p).c_str());
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && !r.packet.decrypted && r.packet.note.find("unknown channel") != std::string::npos && r.packet.type == "ENCRYPTED", "unknown channel: %s", r.packet.note.c_str());
        CHECK(mp.addMeshtasticChannel("Friends", b64), "add channel");
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "Friends" && r.messages.size() == 1 && r.messages[0].text == "secret" && r.messages[0].channel == "Friends", "user channel decoded");
        CHECK(!mp.addMeshtasticChannel("Bad", "not base64 !!"), "bad key text");
        CHECK(!mp.addMeshtasticChannel("Bad", meshcrypto::base64Encode(std::vector<uint8_t>(40, 1).data(), 40)), "40 byte key");
        // AES-128 key
        const auto k16 = H("00112233445566778899aabbccddeeff");
        p = meshtasticRaw(0x78, 0xFFFFFFFFu, 91, 3, 3, false, "Ham", k16, data);
        CHECK(mp.addMeshtasticChannel("Ham", meshcrypto::base64Encode(k16.data(), 16)), "add 16 byte");
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "Ham", "aes128 channel");
        // PSK index 2 ("Ag==") = default key with last byte + 1
        std::vector<uint8_t> k2 = kDef; k2[15] = 0x02;
        p = meshtasticRaw(0x79, 0xFFFFFFFFu, 92, 3, 3, false, "Idx2", k2, data);
        CHECK(mp.addMeshtasticChannel("Idx2", "Ag=="), "add index key");
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "Idx2", "psk index 2");
        // no encryption (psk byte 0)
        p = meshtasticRaw(0x7a, 0xFFFFFFFFu, 93, 3, 3, false, "Open", {}, data);
        CHECK(mp.addMeshtasticChannel("Open", "AA=="), "add plaintext channel");
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.decrypted && r.packet.channel == "Open", "plaintext channel");
        auto names = mp.channelNames(MeshProtocol::Meshtastic);
        CHECK(names.size() >= 14 && names[0] == "LongFast", "channel names");
        mp.clearUserChannels();
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(!r.packet.decrypted, "user channels cleared");
    }
    { // custom channel name with the default key: the hash differs from every default name, found by trying the default key
        auto data = meshtasticData(1, std::vector<uint8_t>{'h', 'i'});
        auto p = meshtasticRaw(0x88, 0xFFFFFFFFu, 100, 3, 3, false, "MyMesh", kDef, data);
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.decrypted && r.messages.size() == 1 && r.messages[0].text == "hi" && r.packet.note.find("header hash differs") != std::string::npos, "default key under another name: %s", r.packet.note.c_str());
        // hash matches a known channel but the key is wrong
        MeshProto fresh;
        const auto other = H("ffeeddccbbaa99887766554433221100");
        p = meshtasticRaw(0x88, 0xFFFFFFFFu, 101, 3, 3, false, "LongFast", other, data);
        p[13] = 0x08;                                   // pretend the sender used the LongFast hash
        r = fresh.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.ok && !r.packet.decrypted && r.packet.note.find("wrong key") != std::string::npos, "wrong key: %s", r.packet.note.c_str());
    }
    { // flags and via MQTT, short packets
        auto p = meshtasticText(a, 5, 1, 7, 7, "x");
        p[12] |= 0x10;
        auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.packet.viaMqtt && r.packet.hopLimit == 7 && r.packet.hopStart == 7, "mqtt flag");
        p = meshtasticText(a, 5, 1, 2, 0, "x");
        r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
        CHECK(r.messages.size() == 1 && r.messages[0].hops == -1, "hop start 0 = unknown hops");
        for (size_t n = 0; n < 16; n++) { r = mp.decodeMeshtastic(p.data(), n, kRadio); CHECK(!r.ok, "short packet %zu", n); }
        r = mp.decodeMeshtastic(p.data(), 16, kRadio);
        CHECK(r.ok && r.packet.type == "EMPTY", "header only");
        r = mp.decodeMeshtastic(nullptr, 0, kRadio);
        CHECK(!r.ok, "null");
    }
    { // random bytes: never a crash, and a wrong key almost never passes the Data check
        std::mt19937 rng(12345);
        int decoded = 0;
        const int N = 200000;
        for (int i = 0; i < N; i++) {
            std::vector<uint8_t> p(16 + 1 + rng() % 120);
            for (auto& x : p) x = (uint8_t)rng();
            if (i & 1) p[13] = 0x08;
            auto r = mp.decodeMeshtastic(p.data(), p.size(), kRadio);
            if (r.packet.decrypted) decoded++;
        }
        printf("random packets taken for messages: %d of %d\n", decoded, N);
        CHECK(decoded < N / 2000, "false decodes %d of %d", decoded, N);
    }
    { // the lists can grow while another thread decodes
        auto p = meshtasticText(a, 5, 1, 2, 3, "x");
        std::atomic<bool> stop{false};
        std::atomic<int> good{0};
        std::thread t([&] { while (!stop) { if (mp.decodeMeshtastic(p.data(), p.size(), kRadio).packet.decrypted) good++; } });
        for (int i = 0; i < 200; i++) { mp.addMeshtasticChannel("Ch" + std::to_string(i), "AQ=="); mp.addMeshCoreChannel("Mc" + std::to_string(i), "8b3387e9c5cdea6ac9e5edbaa115cd72"); }
        // the adds can be over before the thread has even started (Windows): it has to decode once with the grown lists, up to 5 s
        for (int w = 0; w < 500 && good == 0; w++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        stop = true;
        t.join();
        CHECK(good > 0, "decoding during add");
    }

    if (fails) return 1;
    printf("mesh_proto_meshtastic ok\n");
    return 0;
}
