// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN in the mesh mode (the "lorawan" protocol entry).
// 1. Packet layer: data frames and join requests built byte by byte as the LoRaWAN spec lays them out decode to the exact MHDR,
//    DevAddr, NetID / network, FCtrl, FCnt, FOpts MAC commands, FPort, encrypted length and MIC; DevEUI / JoinEUI / DevNonce and the
//    OUI vendor; malformed and truncated payloads are refused, random ones never crash.
// 2. Radio: uplinks and a join request at SF7 and SF12 (125 kHz, sync 0x34) through MeshReceiver with a +-5 kHz tuning error (tests/
//    impair.h, independent of the generator), a frame with a broken CRC (rejected, no device), noise alone (nothing).
// 3. The EU868 test scene (mesh_gen region 3): every device of a cycle in the device table with the right counters.
#include "dect2/mesh/mesh_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/mesh/mesh_proto.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/engine/mode_tuning.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static void packetLayer() {
    // unconfirmed uplink, The Things Network DevAddr, ADR, FPort 1, 10 bytes, MIC
    LorawanTx t;
    t.mtype = 2; t.devAddr = 0x260B1A2C; t.adr = true; t.fcnt = 1234; t.fport = 1; t.frm.assign(10, 0x5A); t.mic = 0xA1B2C3D4;
    auto b = lorawanBuild(t);
    // the bytes as the spec puts them: MHDR 0x40, DevAddr little endian, FCtrl, FCnt little endian, FPort, payload, MIC
    const uint8_t want[] = {0x40, 0x2C, 0x1A, 0x0B, 0x26, 0x80, 0xD2, 0x04, 0x01};
    CHECK(b.size() == 9 + 10 + 4 && std::equal(want, want + 9, b.begin()) && b[19] == 0xA1 && b[22] == 0xD4, "uplink bytes");
    auto r = meshDecodeLorawan(b.data(), b.size());
    const auto& w = r.lorawan;
    CHECK(r.ok && r.hasLorawan && w.mtype == 2 && w.uplink && !w.confirmed, "uplink type: %s", r.packet.note.c_str());
    CHECK(w.devAddr == 0x260B1A2C && r.packet.from == "260B1A2C" && w.fcnt == 1234 && r.packet.packetId == 1234, "DevAddr %08X FCnt %u", w.devAddr, w.fcnt);
    CHECK(w.netIdType == 0 && w.nwkId == 0x13 && w.netId == 0x13 && w.network == "The Things Network", "network %s type %d nwk %X", w.network.c_str(), w.netIdType, w.nwkId);
    CHECK(w.adr && !w.ack && !w.adrAckReq && w.foptsLen == 0 && w.fport == 1 && w.frmLen == 10 && w.mic == 0xA1B2C3D4, "FCtrl / FPort / MIC");
    CHECK(!r.packet.decrypted && r.packet.detail.find("MIC A1B2C3D4") != std::string::npos && r.packet.detail.find("10 bytes encrypted") != std::string::npos, "detail '%s'", r.packet.detail.c_str());
    // confirmed uplink with FOpts: LinkCheckReq, DevStatusAns (battery 200, margin -3 dB), no payload; Orange DevAddr
    t = LorawanTx(); t.mtype = 4; t.devAddr = 0x22A1B2C3; t.ack = true; t.adrAckReq = true; t.fcnt = 65535; t.fopts = {0x02, 0x06, 200, 0x3D}; t.mic = 0x01020304;
    b = lorawanBuild(t);
    r = meshDecodeLorawan(b.data(), b.size());
    CHECK(r.ok && r.lorawan.confirmed && r.lorawan.uplink && r.lorawan.ack && r.lorawan.adrAckReq && r.lorawan.fcnt == 65535 && r.lorawan.fport == -1, "confirmed up");
    CHECK(r.lorawan.macCommands.size() == 2 && r.lorawan.macCommands[0] == "LinkCheckReq" && r.lorawan.macCommands[1] == "DevStatusAns", "MAC commands");
    CHECK(r.lorawan.devStatusBattery == 200 && r.lorawan.devStatusMargin == -3 && r.lorawan.network == "Orange", "DevStatusAns %d %d, %s", r.lorawan.devStatusBattery, r.lorawan.devStatusMargin, r.lorawan.network.c_str());
    // downlink: LinkADRReq + FPending, the names of the other direction
    t = LorawanTx(); t.mtype = 3; t.devAddr = 0x48000A31; t.fpending = true; t.fcnt = 5; t.fopts = {0x03, 0x52, 0xFF, 0x00, 0x01, 0x06}; t.fport = 2; t.frm = {1, 2, 3};
    b = lorawanBuild(t);
    r = meshDecodeLorawan(b.data(), b.size());
    CHECK(r.ok && !r.lorawan.uplink && r.lorawan.fpending && r.lorawan.macCommands.size() == 2 && r.lorawan.macCommands[0] == "LinkADRReq" && r.lorawan.macCommands[1] == "DevStatusReq" &&
          r.lorawan.network == "Helium" && r.lorawan.macDetail.find("DR5") != std::string::npos, "downlink: %s / %s", r.packet.detail.c_str(), r.lorawan.network.c_str());
    // join request: EUIs little endian on air, shown most significant byte first; Dragino OUI
    b = lorawanJoinRequest(0xA840410000000101ull, 0xA84041000181C61Aull, 0xBEEF, 0x11223344);
    CHECK(b.size() == 23 && b[0] == 0 && b[1] == 0x01 && b[8] == 0xA8 && b[9] == 0x1A && b[17] == 0xEF, "join bytes");
    r = meshDecodeLorawan(b.data(), b.size());
    CHECK(r.ok && r.lorawan.mtype == 0 && r.packet.type == "Join request" && r.packet.from == "A84041000181C61A" && r.packet.to == "A840410000000101" &&
          r.lorawan.devNonce == 0xBEEF && r.lorawan.mic == 0x11223344 && r.lorawan.vendor == "Dragino", "join: %s", r.packet.detail.c_str());
    // network lookup, other types
    int ty = -2; uint32_t nwk = 0, net = 0;
    CHECK(lorawanNetwork(0x27FFFFFF, &ty) == "The Things Network" && ty == 0, "0x27 TTN");
    CHECK(lorawanNetwork(0xFC014C12, &ty, &nwk) == "Helium" && ty == 6 && nwk == 0x53, "Helium type 6: type %d nwk %X", ty, nwk);
    CHECK(lorawanNetwork(0x0A000000, &ty, &nwk, &net) == "NetID 0x000005" && net == 5, "unknown type 0");
    CHECK(lorawanNetwork(0xE0123456, &ty) .find("type 3") != std::string::npos && ty == 3, "type 3");
    CHECK(lorawanNetwork(0xFF000000, &ty) == "reserved DevAddr" && ty == -1, "reserved");
    CHECK(std::string(lorawanVendor(0x70B3D57ED0001234ull)) == "The Things Network block" && std::string(lorawanVendor(0x24E124136B502217ull)) == "Milesight" &&
          std::string(lorawanVendor(0x1122334455667788ull)).empty(), "OUI table");
    CHECK(lorawanMacName(0x0D, true) == "DeviceTimeReq" && lorawanMacName(0x0D, false) == "DeviceTimeAns" && lorawanMacName(0x85, true) == "proprietary 0x85", "MAC names");
    // join accept (encrypted), proprietary
    std::vector<uint8_t> ja(17, 0x33); ja[0] = 0x20;
    r = meshDecodeLorawan(ja.data(), ja.size());
    CHECK(r.ok && r.lorawan.mtype == 1 && r.packet.note == "encrypted", "join accept");
    // refused: RFU bits, major 1, short data, bad join length, FOptsLen past the end
    std::vector<uint8_t> v = lorawanBuild(LorawanTx{});
    v[0] |= 0x04; CHECK(!meshDecodeLorawan(v.data(), v.size()).ok, "RFU accepted");
    v[0] = 0x41; CHECK(!meshDecodeLorawan(v.data(), v.size()).ok, "major 1 accepted");
    v = {0x40, 1, 2, 3, 4, 0, 0, 0, 1, 2, 3}; CHECK(!meshDecodeLorawan(v.data(), v.size()).ok, "11 byte data accepted");
    b = lorawanJoinRequest(1, 2, 3, 4); b.pop_back(); CHECK(!meshDecodeLorawan(b.data(), b.size()).ok, "22 byte join accepted");
    v = {0x40, 1, 2, 3, 4, 0x0F, 0, 0, 2, 2, 2, 2, 2}; CHECK(!meshDecodeLorawan(v.data(), v.size()).ok, "FOptsLen past the end accepted");
    // random payloads and every truncation: no crash
    std::mt19937 g(5);
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> x((size_t)(g() % 64));
        for (auto& c : x) c = (uint8_t)g();
        meshDecodeLorawan(x.data(), x.size());
    }
    t = LorawanTx(); t.devAddr = 1; t.fopts = {0x03, 0x06, 0x02}; t.fport = 9; t.frm.assign(20, 1);
    b = lorawanBuild(t);
    for (size_t k = 0; k <= b.size(); k++) meshDecodeLorawan(b.data(), k);
    // the channel plans
    const auto eu = lorawanPresets(0), us = lorawanPresets(1);
    CHECK(eu.size() == 8 * 6 + 1 && eu[0].freqHz == 868.1e6 && eu[0].sf == 7 && eu[0].bwHz == 125e3 && eu[0].syncWord == 0x34 && eu[0].preamble == 8 && eu[5].sf == 12 && eu[5].ldro && !eu[3].ldro, "EU868 plan");
    CHECK(us.size() == 64 * 4 + 8 && us[0].freqHz == 902.3e6 && us.back().bwHz == 500e3, "US915 plan");
}

// ---- radio ----
struct Shot { std::vector<uint8_t> bytes; double startSec; int sf; double chanHz; bool breakCrc = false; };

static MeshTelemetry run(const std::vector<Shot>& shots, double snrDb, double shiftHz, double secs, uint32_t seed) {
    const double rate = 1.2e6, tuned = 868.1e6, sigOff = -300e3;   // the receiver tuned to 868.1 MHz, 300 kHz below the radio's centre
    std::vector<cf32> x((size_t)(secs * rate), cf32(0, 0));
    const float amp = 0.2f;
    for (const auto& s : shots) {
        lora::Params p;
        p.sf = s.sf; p.bwHz = 125e3; p.cr = 5; p.preamble = 8; p.syncWord = 0x34; p.ldro = meshLoraLdro(s.sf, 125e3);
        lora::TxFrame f;
        f.p = p; f.data = lora::encode(p, s.bytes.data(), s.bytes.size());
        if (s.breakCrc) f.data[f.data.size() / 2 + 4] ^= (uint16_t)((1 << s.sf) / 2 + 3);   // a payload symbol hit hard: the CRC must catch it
        f.startSec = s.startSec; f.freqHz = s.chanHz - tuned + sigOff; f.amp = amp; f.phase0 = 0.7;
        f.render(x.data(), x.size(), 0, rate);
    }
    impair::shift(x, shiftHz, rate);
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, (float)std::sqrt(amp * amp * (rate / 125e3) / std::pow(10.0, snrDb / 10) / 2));
    for (auto& v : x) v += cf32{nd(g), nd(g)};
    MeshReceiver rx;
    rx.setProtocols(16);
    rx.setTunedHz(tuned);
    rx.setSignalOffset(sigOff);
    rx.configure(rate);
    for (size_t i = 0; i < x.size(); i += 50000) rx.feed(x.data() + i, std::min<size_t>(50000, x.size() - i));
    MeshTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static const MeshLorawanDevice* device(const MeshTelemetry& t, const std::string& id) {
    for (const auto& d : t.lorawanDevices) if (d.id == id) return &d;
    return nullptr;
}

static void radio() {
    LorawanTx a; a.mtype = 2; a.devAddr = 0x260B1A2C; a.adr = true; a.fcnt = 41; a.fport = 1; a.frm.assign(12, 0x77); a.mic = 0xCAFEF00D;
    LorawanTx b = a; b.devAddr = 0x22A1B2C3; b.mtype = 4; b.fcnt = 7001; b.fopts = {0x06, 0xC8, 0x07}; b.fport = 3; b.frm.assign(6, 0x11); b.mic = 0x0BADBEEF;
    LorawanTx a2 = a; a2.fcnt = 43;
    const auto join = lorawanJoinRequest(0xA840410000000101ull, 0xA84041000181C61Aull, 0x1234, 0x55667788);
    for (double shift : {0.0, 5e3, -5e3}) {
        // SF7 on 868.1, SF12 on 868.5 at the same time, a join request at SF12 on 868.3, then SF7 again with a frame counter gap
        const std::vector<Shot> shots = {{lorawanBuild(a), 0.2, 7, 868.1e6}, {lorawanBuild(b), 0.3, 12, 868.5e6}, {join, 0.4, 12, 868.3e6}, {lorawanBuild(a2), 2.0, 7, 868.1e6}};
        const MeshTelemetry t = run(shots, -5, shift, 3.2, 3);
        const MeshLorawanDevice* da = device(t, "260B1A2C");
        const MeshLorawanDevice* db = device(t, "22A1B2C3");
        const MeshLorawanDevice* dj = device(t, "DevEUI A84041000181C61A");
        int mics = 0;
        for (const auto& p : t.packets) mics += p.protocol == 5 && p.parsed && (p.mic == 0xCAFEF00D || p.mic == 0x0BADBEEF || p.mic == 0x55667788);
        printf("  -5 dB, %+5.0f Hz: %llu good / %llu bad frames, devices %zu, SF7 %s, SF12 %s, join %s, MICs %d\n", shift, (unsigned long long)t.blocksOk,
               (unsigned long long)t.blocksBad, t.lorawanDevices.size(), da ? "yes" : "NO", db ? "yes" : "NO", dj ? "yes" : "NO", mics);
        CHECK(da && da->fcnt == 43 && da->packets == 2 && da->missed == 1 && da->sf == 7 && da->network == "The Things Network" && da->adr && da->lastFport == 1, "%+.0f Hz: SF7 device", shift);
        CHECK(db && db->fcnt == 7001 && db->sf == 12 && db->confirmed == 1 && db->battery == 200 && db->marginDb == 7 && db->network == "Orange", "%+.0f Hz: SF12 device", shift);
        CHECK(dj && dj->joins == 1 && dj->sf == 12 && dj->joinEui == "A840410000000101" && dj->vendor == "Dragino", "%+.0f Hz: join request", shift);
        CHECK(mics == 4, "%+.0f Hz: %d MICs", shift, mics);
        CHECK(t.blocksBad == 0 && t.lorawanDevices.size() == 3, "%+.0f Hz: %llu CRC errors, %zu devices", shift, (unsigned long long)t.blocksBad, t.lorawanDevices.size());
        for (const auto& p : t.packets) if (p.parsed && p.from == "260B1A2C") CHECK(std::fabs(p.cfoHz - shift) < 300, "CFO %.0f for %+.0f", p.cfoHz, shift);
    }
    // a broken CRC: counted, never a device
    {
        const MeshTelemetry t = run({{lorawanBuild(a), 0.2, 7, 868.1e6, true}, {lorawanBuild(b), 0.3, 12, 868.5e6, true}}, 5, 0, 2.6, 4);
        CHECK(t.lorawanDevices.empty() && t.blocksOk == 0 && t.blocksBad >= 1, "bad CRC: %zu devices, %llu good, %llu bad", t.lorawanDevices.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        printf("  broken CRC: %llu CRC errors, %zu devices (none expected)\n", (unsigned long long)t.blocksBad, t.lorawanDevices.size());
    }
    // noise alone
    {
        const MeshTelemetry t = run({}, 0, 0, 6.0, 9);
        CHECK(t.lorawanDevices.empty() && t.packets.empty() && t.blocksOk == 0, "noise: %zu devices, %zu packets", t.lorawanDevices.size(), t.packets.size());
        printf("  noise: %zu packets, %llu preambles\n", t.packets.size(), (unsigned long long)t.preambles);
    }
}

// ---- the EU868 scene ----
static void scene() {
    const double rate = 1.2e6, secs = 60;
    int n = 0;
    const SubMode* sm = subModes(n);
    const SubMode* lw = subModeById("lorawan");
    CHECK(lw && lw->stdMode == 22 && lw->decoders == 16 && lw->defMhz == 868.1, "the lorawan entry");
    (void)sm;
    SynthConfig cfg;
    cfg.mode = 22;
    cfg.snrDb = 0;
    cfg.modeVal[0] = -8;
    cfg.cfoHz = 5e3;
    if (lw) applySubModeSynth(cfg, *lw);
    CHECK(cfg.modeOpt[0] == 16 && cfg.modeOpt[1] == 3, "entry's test signal");
    auto syn = makeMeshSynth(cfg, rate);
    MeshReceiver rx;
    rx.setProtocols(16);
    rx.setTunedHz(868.1e6);
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    rx.configure(rate);
    std::vector<cf32> buf(60000);
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    MeshTelemetry t;
    rx.telemetry(t, 0);
    printf("%s\n", meshSummary(t).c_str());
    for (const auto& d : t.lorawanDevices)
        printf("  %-24s %-20s %-18s SF%-2d FCnt %5ld, %u packets, %u missed, '%s'\n", d.id.c_str(), d.network.c_str(), d.vendor.c_str(), d.sf, d.fcnt, d.packets, d.missed, d.lastMac.c_str());
    CHECK(t.lorawanDevices.size() == 6, "%zu devices of 6", t.lorawanDevices.size());
    const MeshLorawanDevice* d0 = device(t, "260B1A2C");
    CHECK(d0 && d0->packets == 6 && d0->fcnt == 1205 && d0->missed == 0, "TTN sensor: %u packets, FCnt %ld", d0 ? d0->packets : 0, d0 ? d0->fcnt : -1);
    const MeshLorawanDevice* d2 = device(t, "22A1B2C3");
    CHECK(d2 && d2->sf == 12 && d2->packets == 2 && d2->battery == 200, "Orange meter");
    const MeshLorawanDevice* d4 = device(t, "01ABCDEF");
    CHECK(d4 && d4->lastMac == "LinkCheckReq" && d4->network == "Experimental / private", "private device");
    CHECK(device(t, "DevEUI 24E124136B502217") && device(t, "DevEUI 24E124136B502217")->vendor == "Milesight", "Milesight join");
    CHECK(t.blocksBad == 0, "%llu CRC errors", (unsigned long long)t.blocksBad);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    packetLayer();
    radio();
    scene();
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
