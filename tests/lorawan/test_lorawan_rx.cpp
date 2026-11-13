// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN monitor: the test signal (lorawan_gen.h) through the receiver (lorawan_rx.h) on the EU868 plan: several channels at once at different spreading
// factors and bandwidths, the MAC layer (type, DevAddr, counter, port, MIC, MAC commands, join request / accept), the gateways' inverted-IQ frames,
// carrier offsets, noise, a plan change, the channel limit, the results file, and noise alone.
#include "dect2/lorawan/lorawan_gen.h"
#include "dect2/lorawan/lorawan_rx.h"
#include "dect2/lorawan/lorawan_plan.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static LorawanTelemetry run(LorawanGenConfig c, const LorawanSettings& s, double secs, double centreHz = 0) {
    LorawanGenerator g(c);
    LorawanReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(c.dialOffsetHz);
    if (centreHz > 0) r.setCenterHz(centreHz);
    r.setSettings(s);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.3) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    LorawanTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const LwFrame* byAddr(const LorawanTelemetry& t, uint32_t addr, bool inverted) {
    for (const auto& f : t.log) if (f.lorawan && f.hasDevAddr && f.devAddr == addr && f.inverted == inverted && (!f.hasCrc || f.crcOk)) return &f;
    return nullptr;
}

static LorawanGenConfig sceneConfig(int plan, double snr) {
    LorawanGenConfig c;
    c.rate = 4e6;
    c.dialMhz = lorawanPlanCentreMhz(plan);
    c.dialOffsetHz = -lorawanTuning().tuneOffsetHz;
    c.snrDb = snr; c.loop = false;
    c.frames = lorawanTestSchedule(plan);
    return c;
}

int main() {
    {   // plans and the MAC parser on known bytes (no radio)
        using namespace lorawan;
        CHECK(selectChannels(EU868, 0, false).size() == 9, "EU868 without downlink-only channels: %zu", selectChannels(EU868, 0, false).size());
        CHECK(selectChannels(US915, 0, false).size() == 72 && selectChannels(US915, 0, true).size() == 80, "US915 channel counts %zu %zu", selectChannels(US915, 0, false).size(), selectChannels(US915, 0, true).size());
        CHECK(selectChannels(US915, 2, false).size() == 9, "US915 sub-band 2: %zu", selectChannels(US915, 2, false).size());
        const auto as1 = planChannels(AS923_1), as3 = planChannels(AS923_3);
        CHECK(std::fabs(as1.front().mhz - 923.2) < 1e-6 && std::fabs(as3.front().mhz - 916.6) < 1e-6, "AS923 offsets %.3f %.3f", as1.front().mhz, as3.front().mhz);
        CHECK(std::fabs(planChannels(EU433).front().mhz - 433.175) < 1e-6, "EU433 first channel");
        // a data frame with MAC commands in FOpts
        Build b; b.mtype = 4; b.devAddr = 0x260B1234; b.fcnt = 7; b.fport = 10; b.frm = {1, 2, 3}; b.fopts = {0x06, 0x4F, 0x0A, 0x02}; b.mic = 0x55AA55AA; b.adr = true; b.adrAckReq = true;
        const auto bytes = build(b);
        const Frame f = parse(bytes.data(), bytes.size());
        CHECK(f.ok && f.mtype == 4 && f.uplink && f.confirmed && f.devAddr == 0x260B1234 && f.fcnt == 7 && f.fport == 10 && f.frmLen == 3 && f.mic == 0x55AA55AA, "data frame fields");
        CHECK(f.mac.size() == 2 && f.mac[0].name == "DevStatusAns" && f.mac[1].name == "LinkCheckReq", "MAC commands: %zu", f.mac.size());
        CHECK(f.mac[0].detail.find("battery 31 %") != std::string::npos && f.mac[0].detail.find("margin 10 dB") != std::string::npos, "DevStatusAns detail: %s", f.mac[0].detail.c_str());
        CHECK(f.flags == "ADR, ADRAckReq", "flags %s", f.flags.c_str());
        CHECK(f.addrType == 0 && f.nwkId == 0x13, "DevAddr 0x260B1234 is type 0 with NwkID 0x13: %d %X", f.addrType, f.nwkId);
        const auto jr = buildJoinRequest(0x70B3D57ED0000001ull, 0x70B3D57ED0123456ull, 0x4A2B, 0x12345678);
        const Frame j = parse(jr.data(), jr.size());
        CHECK(j.ok && j.mtype == 0 && j.devNonce == 0x4A2B && eui(j.devEui) == "70B3D57ED0123456" && eui(j.joinEui) == "70B3D57ED0000001" && j.mic == 0x12345678, "join request");
        const auto ja = buildJoinAccept(33, 1);
        const Frame a = parse(ja.data(), ja.size());
        CHECK(a.ok && a.mtype == 1 && a.joinAcceptCfList && !a.uplink, "join accept with a channel list");
        const uint8_t rfu[] = {0x04, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        CHECK(!parse(rfu, sizeof rfu).ok, "RFU bits of MHDR set must be refused");
        const uint8_t shortF[] = {0x40, 1, 2, 3, 4, 0x0F, 0, 0, 1, 2, 3, 4};   // FOptsLen 15 does not fit
        CHECK(!parse(shortF, sizeof shortF).ok, "FOpts longer than the frame must be refused");
        CHECK(macCommandName(0x03, false) == "LinkADRReq" && macCommandName(0x03, true) == "LinkADRAns" && macCommandName(0x90, true) == "proprietary 0x90", "MAC command names");
    }
    {   // the EU868 scene, clean, 4 Msps
        LorawanGenConfig c = sceneConfig(lorawan::EU868, 30);
        LorawanSettings s;
        const double secs = lorawanSceneSeconds(c.frames);
        const LorawanTelemetry t = run(c, s, secs);
        printf("  EU868 scene: %s, %zu in the log, %u demodulators, note: %s\n", lorawanSummary(t).c_str(), t.log.size(), t.demodulators, t.note.c_str());
        CHECK(t.channelsInBand >= 8, "EU868 channels decoded: %u", t.channelsInBand);
        CHECK(t.blocksBad == 0, "%llu bad frames", (unsigned long long)t.blocksBad);
        size_t want = 0;
        for (const auto& g : c.frames) want += g.mhz > 0;
        CHECK(t.blocksOk >= want, "%llu frames of %zu", (unsigned long long)t.blocksOk, want);
        const LwFrame* u = byAddr(t, 0x26011A2B, false);
        CHECK(u && u->mtype == 2 && u->fcnt == 100 && u->fport == 1 && u->frmLen == 8 && u->sf == 7 && u->mic == 0xA1B2C3D4 && std::fabs(u->freqHz - c.frames[0].mhz * 1e6) < 1, "uplink 1");
        if (u) CHECK(u->flags == "ADR" && u->snrDb > 20 && std::fabs(u->cfoHz - 1300) < 150 && !u->inverted, "uplink 1 details: %s snr %.1f cfo %.0f", u->flags.c_str(), u->snrDb, u->cfoHz);
        const LwFrame* u2 = byAddr(t, 0x26014C01, false);
        CHECK(u2 && u2->sf == 11 && u2->fcnt == 3041 && u2->fport == 2 && u2->frmLen == 20 && std::fabs(u2->cfoHz + 2100) < 150, "uplink 2 at SF11");
        const LwFrame* cf = byAddr(t, 0x260B1234, false);
        CHECK(cf && cf->mtype == 4 && cf->sf == 9 && cf->mac.find("DevStatusAns") != std::string::npos && cf->mac.find("LinkCheckReq") != std::string::npos, "confirmed uplink with MAC commands: %s", cf ? cf->mac.c_str() : "missing");
        const LwFrame* dn = byAddr(t, 0x260B1234, true);
        CHECK(dn && dn->mtype == 3 && !dn->uplink && dn->sf == 9 && dn->flags.find("ACK") != std::string::npos && dn->mac.find("LinkADRReq") != std::string::npos && dn->fport == -1, "downlink on inverted IQ with ACK and LinkADRReq: %s", dn ? dn->mac.c_str() : "missing");
        bool join = false, accept = false, prop = false, alt = false;
        for (const auto& f : t.log) {
            if (f.lorawan && f.mtype == 0 && f.devEui == 0x70B3D57ED0123456ull && f.devNonce == 0x4A2B && f.sf == 12) join = true;
            if (f.lorawan && f.mtype == 1 && f.inverted && f.len == 17) accept = true;
            if (f.lorawan && f.mtype == 7) prop = true;
            if (f.lorawan && f.bwHz == 250e3 && f.hasDevAddr && f.devAddr == 0x2701AAAA) alt = true;
        }
        CHECK(join, "join request at SF12 not shown");
        CHECK(accept, "join accept (inverted IQ, 17 bytes) not shown");
        CHECK(prop, "proprietary frame not shown");
        CHECK(alt, "250 kHz uplink (DR6) not shown");
        bool dev = false;
        for (const auto& d : t.devices) if (d.hasDevAddr && d.devAddr == 0x260B1234 && d.uplinks == 1 && d.downlinks == 1 && d.confirmed >= 1) dev = true;
        CHECK(dev, "device table: confirmed uplink and downlink of 260B1234");
    }
    {   // noise and a carrier offset on top
        LorawanGenConfig c = sceneConfig(lorawan::EU868, 6);
        c.cfoHz = 2500;
        LorawanSettings s;
        const LorawanTelemetry t = run(c, s, lorawanSceneSeconds(c.frames));
        CHECK(t.blocksOk >= 7, "6 dB SNR, +2.5 kHz offset: %llu frames", (unsigned long long)t.blocksOk);
        CHECK(byAddr(t, 0x26011A2B, false) != nullptr, "the SF7 uplink at 6 dB");
    }
    {   // downlinks off: no gateway frames
        LorawanGenConfig c = sceneConfig(lorawan::EU868, 30);
        LorawanSettings s; s.downlinks = false;
        const LorawanTelemetry t = run(c, s, lorawanSceneSeconds(c.frames));
        size_t inv = 0;
        for (const auto& f : t.log) inv += f.inverted;
        CHECK(inv == 0 && t.blocksOk >= 5, "downlinks off: %zu inverted, %llu frames", inv, (unsigned long long)t.blocksOk);
    }
    {   // the channel limit keeps the nearest channels
        LorawanGenConfig c = sceneConfig(lorawan::EU868, 30);
        LorawanSettings s; s.maxChannels = 3;
        const LorawanTelemetry t = run(c, s, 1.0);
        CHECK(t.channelsInBand == 3, "limit of 3 channels: %u", t.channelsInBand);
    }
    {   // another plan: AS923-1 and EU433 each decode their own scene
        for (int plan : {(int)lorawan::AS923_1, (int)lorawan::EU433, (int)lorawan::US915}) {
            LorawanGenConfig c = sceneConfig(plan, 30);
            LorawanSettings s; s.plan = plan; s.maxChannels = 12;
            const LorawanTelemetry t = run(c, s, lorawanSceneSeconds(c.frames));
            const size_t want = c.frames.size();
            printf("  %s: %llu of %zu frames\n", lorawan::planName(plan), (unsigned long long)t.blocksOk, want);
            CHECK(t.blocksOk >= want - 1, "%s: %llu of %zu frames", lorawan::planName(plan), (unsigned long long)t.blocksOk, want);
            CHECK(byAddr(t, 0x26011A2B, false) != nullptr, "%s: the first uplink", lorawan::planName(plan));
        }
    }
    {   // noise alone: no frames
        LorawanGenConfig c;
        c.rate = 4e6; c.frames.clear(); c.snrDb = 10;
        LorawanSettings s;
        const LorawanTelemetry t = run(c, s, 2.0);
        CHECK(t.blocksOk == 0 && t.log.empty(), "noise alone: %llu frames", (unsigned long long)t.blocksOk);
    }
    {   // results file
        LorawanGenConfig c = sceneConfig(lorawan::EU868, 30);
        LorawanSettings s;
        LorawanGenerator g(c);
        LorawanReceiver r;
        r.configure(c.rate); r.setSignalOffset(c.dialOffsetHz); r.setSettings(s);
        std::vector<cf32> buf(16384);
        const size_t total = (size_t)((lorawanSceneSeconds(c.frames) + 0.3) * c.rate);
        for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        const std::string blob = r.saveResults();
        LorawanReceiver r2;
        r2.configure(c.rate);
        CHECK(r2.loadResults(blob), "results load");
        CHECK(!r2.loadResults("junk"), "damaged results are refused");
    }
    return testSummary("lorawan rx");
}
