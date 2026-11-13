// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising monitor: the test signal through the receiver (ble_gen.h, ble_rx.h): each advertising channel, beacons,
// CRC failures, carrier offsets with noise and two devices, another sample rate, and noise alone.
#include "dect2/ble/ble_gen.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/ble/ble_tel.h"
#include "dect2/dsp/gen_util.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::ble;
#include "../common/test_util.h"

static BleTelemetry run(BleGenConfig c, double secs, double centerHz = 0) {
    BleGenerator g(c);
    BleReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);   // as the engine does: minus the tuning offset
    r.setCenterHz(centerHz);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(secs * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    BleTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const BleDevice* find(const BleTelemetry& t, const char* addr) {
    for (const auto& d : t.devices) if (d.addr == addr) return &d;
    return nullptr;
}

static const uint8_t kA[6] = {0x56, 0x34, 0x12, 0xEE, 0xFF, 0xC0};   // C0:FF:EE:12:34:56
static const uint8_t kB[6] = {0x01, 0x00, 0x5E, 0xA1, 0x0B, 0x00};   // 00:0B:A1:5E:00:01

static BleGenPacket advA(int ch) {
    BleGenPacket p;
    p.pdu = makePdu(AdvInd, true, advPayload(kA, AdBuilder().flags(6).name("Kitchen scale").mfr(0x0059, {0xDE, 0xAD, 0xBE, 0xEF}).b));
    p.channel = ch; p.gapSec = 0.002;
    return p;
}

int main() {
    // an ADV_IND with a name and manufacturer data on each advertising channel; the channel comes from the whitening
    for (int ch = 37; ch <= 39; ch++) {
        BleGenConfig c;
        c.snrDb = 30;
        c.pkts = {advA(ch)};
        const BleTelemetry t = run(c, 0.25, channelMhz(ch) * 1e6);
        const BleDevice* d = find(t, "C0:FF:EE:12:34:56");
        printf("  channel %d: SNR %.1f, %llu good, %llu bad, %zu devices\n", ch, t.snrDb, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.devices.size());
        CHECK(d && d->name == "Kitchen scale" && d->vendor == "Nordic Semiconductor" && d->mfrHex == "DEADBEEF", "channel %d: device", ch);
        CHECK(d && d->kind == 1 && d->connectable && d->chanMask == (1 << (ch - 37)), "channel %d: kind / flags", ch);
        CHECK(t.blocksOk >= 90 && t.lastChannel == ch && t.perChannel[ch - 37] == t.blocksOk, "channel %d: %llu packets", ch, (unsigned long long)t.blocksOk);
        CHECK(t.devices.size() == 1 && t.blocksBad == 0, "channel %d: %zu devices, %llu bad", ch, t.devices.size(), (unsigned long long)t.blocksBad);
    }
    {   // the whitening is found without knowing the channel (a file), and the full schedule decodes: beacons and the other PDU types
        BleGenConfig c;
        c.snrDb = 30;
        c.pkts = bleTestSchedule(0);
        const BleTelemetry t = run(c, 0.5);
        const BleDevice* ib = find(t, "00:0B:A1:5E:00:01");
        const BleDevice* ed = find(t, "3A:55:44:33:22:02");
        const BleDevice* ph = find(t, "C0:FF:EE:12:34:56");
        CHECK(ib && ib->beacon.find("iBeacon E2C56DB5-DFFB-48D2-B060-D0F5A71096E0 major 100 minor 7") != std::string::npos && ib->kind == 0, "iBeacon: %s", ib ? ib->beacon.c_str() : "-");
        CHECK(ed && ed->kind == 3, "Eddystone device");
        bool url = false, tlm = false, uid = false;
        for (const auto& p : t.log) {
            if (p.info.find("Eddystone URL https://example.com") != std::string::npos) url = true;
            if (p.info.find("Eddystone TLM 3000 mV, 21.5 C") != std::string::npos) tlm = true;
            if (p.info.find("Eddystone UID EDD1EBEAC04E5DEFA017") != std::string::npos) uid = true;
        }
        CHECK(url && tlm && uid, "Eddystone URL %d TLM %d UID %d", url, tlm, uid);
        CHECK(ph && ph->name == "OnAir test phone" && ph->scanned && ph->txPower == 4, "phone: name '%s' (scan response)", ph ? ph->name.c_str() : "-");
        CHECK(t.perType[ScanReq] > 0 && t.perType[ConnectInd] > 0 && t.perType[AdvExtInd] > 0 && t.perType[ScanRsp] > 0, "PDU types");
        CHECK(t.perChannel[0] > 0 && t.perChannel[1] > 0 && t.perChannel[2] > 0, "channels %llu %llu %llu", (unsigned long long)t.perChannel[0], (unsigned long long)t.perChannel[1], (unsigned long long)t.perChannel[2]);
        bool en = false;
        for (const auto& d : t.devices) if (d.beacon.find("Exposure Notification") != std::string::npos && d.kind == 2) en = true;
        CHECK(en, "exposure notification");
        CHECK(t.blocksBad == 0, "%llu bad", (unsigned long long)t.blocksBad);
    }
    {   // a wrong CRC is rejected, the good packets around it are not
        BleGenConfig c;
        c.snrDb = 99;
        BleGenPacket bad = advA(37);
        bad.badCrc = true;
        bad.pdu = makePdu(AdvInd, true, advPayload(kB, AdBuilder().name("broken").b));
        c.pkts = {advA(37), bad};
        const BleTelemetry t = run(c, 0.25);
        CHECK(find(t, "C0:FF:EE:12:34:56") && !find(t, "00:0B:A1:5E:00:01"), "bad CRC decoded");
        CHECK(t.blocksBad >= 40 && t.blocksOk >= 40, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    // +-50 kHz, 12 dB, two devices interleaved
    for (double cfo : {50e3, -50e3}) {
        BleGenConfig c;
        c.snrDb = 12;
        c.cfoHz = cfo;
        c.seed = cfo > 0 ? 3 : 4;
        BleGenPacket a = advA(38), b;
        b.pdu = makePdu(AdvNonconnInd, false, advPayload(kB, AdBuilder().flags(4).name("Door sensor").mfr(0x0006, {1, 2}).b));
        b.channel = 38; b.gapSec = 0.0015; b.cfoHz = -cfo / 5;
        c.pkts = {a, b};
        const BleTelemetry t = run(c, 0.5);
        const BleDevice* da = find(t, "C0:FF:EE:12:34:56");
        const BleDevice* db = find(t, "00:0B:A1:5E:00:01");
        std::vector<uint8_t> ba, bb;
        packetBits(a.pdu, 38, ba); packetBits(b.pdu, 38, bb);
        const int sent = (int)(0.5 / ((ba.size() + bb.size() + 12) * 1e-6 + a.gapSec + b.gapSec));   // packets of each device
        printf("  %+.0f kHz, 12 dB: %llu packets (%llu and %llu of about %d each), %llu bad, SNR %.1f dB, offset %+.1f kHz\n", cfo / 1e3, (unsigned long long)t.blocksOk,
               da ? (unsigned long long)da->packets : 0ull, db ? (unsigned long long)db->packets : 0ull, sent, (unsigned long long)t.blocksBad, t.snrDb, t.cfoHz / 1e3);
        CHECK(da && db && da->name == "Kitchen scale" && db->name == "Door sensor" && db->vendor == "Microsoft", "%+.0f kHz: devices", cfo / 1e3);
        CHECK(da && db && da->packets > (uint64_t)sent * 0.7 && db->packets > (uint64_t)sent * 0.7, "%+.0f kHz: too few packets", cfo / 1e3);
        CHECK(t.devices.size() == 2, "%+.0f kHz: %zu devices", cfo / 1e3, t.devices.size());
    }
    {   // 10 Msps with the channel 1 MHz off centre (as the radio is tuned), clock 40 ppm off
        BleGenConfig c;
        c.rate = 10e6;
        c.snrDb = 20;
        c.dialOffsetHz = -bleTuning().tuneOffsetHz;
        c.clockPpm = 40;
        c.pkts = {advA(39)};
        const BleTelemetry t = run(c, 0.25);
        CHECK(find(t, "C0:FF:EE:12:34:56") && t.blocksOk >= 80, "10 Msps: %llu packets", (unsigned long long)t.blocksOk);
    }
    {   // noise alone: nothing
        BleReceiver r;
        r.configure(4e6);
        std::vector<cf32> buf(16384);
        genutil::NoiseSource ns(9);
        for (int i = 0; i < 2 * 4000000 / 16384; i++) { for (auto& v : buf) v = cf32(0, 0); ns.add(buf.data(), buf.size(), 0.1f); r.feed(buf.data(), buf.size()); }
        BleTelemetry t;
        r.telemetry(t, 0);
        printf("  noise: %llu access address hits, %llu good, %llu bad\n", (unsigned long long)t.aaHits, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.devices.empty() && t.blocksOk == 0 && t.log.empty(), "noise gave %zu devices", t.devices.size());
    }
    printf(fails ? "ble rx: %d FAILED\n" : "ble rx: all passed\n", fails);
    return fails ? 1 : 0;
}
