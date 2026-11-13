// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR subscriber transmit (dmr_tx.h): the bursts OnAir builds from the operator's own DMR ID and payload are looped back into the DMR receiver
// (dmr_eval.h harness) and must decode to exactly what was sent, with impairments (carrier offset, clock error, noise, 8-bit rounding). This is the
// "decode its own generator output" gate. Voice bits are a test pattern (no AMBE encoder), so sound is not checked; framing, IDs, alias, text,
// position and the control request are.
#include "dect2/dmr/dmr_tx.h"
#include "dect2/dmr/dmr_eval.h"
#include "dect2/dmr/dmr_lrrp.h"
#include "dect2/dmr/dmr_rx.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static DmrRunResult runTx(const DmrTxConfig& t, double secs, double snr, double cfo = 0, double sro = 0) {
    DmrScenario s;
    s.cfg = dmrTxGenConfig(t);
    s.cfg.snrDb = snr; s.cfg.cfoHz = cfo; s.cfg.sroPpm = sro;
    s.secs = secs; s.skip = 0; s.quantise = true;
    return dmrRun(s);
}

// find a logged voice call matching src/dst/kind
static const DmrCall* findVoice(const DmrRunResult& r, uint32_t src, uint32_t dst, int kind) {
    for (const DmrCall& c : r.tel.callLog)
        if (c.kind == kind && c.idsKnown && c.src == src && c.dst == dst) return &c;
    return nullptr;
}

int main() {
    const uint32_t myId = 2550123, tg = 91, unit = 2550777;

    // ---- group voice call with a talker alias, clean
    {
        DmrTxConfig t; t.myId = myId; t.dest = tg; t.callsign = "LY1ABC"; t.colourCode = 1; t.slot = 1;
        t.call = DmrTxCall::GroupVoice; t.voiceSeconds = 4.0;
        DmrRunResult r = runTx(t, 7, 45);
        const DmrCall* c = findVoice(r, myId, tg, 0);
        CHECK(c, "group voice: call %u -> %u not logged", myId, tg);
        if (c) {
            CHECK(c->voiceFrames > 20, "group voice: only %d voice frames", c->voiceFrames);
            CHECK(c->alias == "LY1ABC", "group voice: alias '%s' (wanted LY1ABC)", c->alias.c_str());
        }
        CHECK(r.tel.cc == 1, "group voice: colour code %d", r.tel.cc);
    }

    // ---- group voice with impairments: +1.2 kHz carrier offset, 30 ppm clock, 18 dB SNR
    {
        DmrTxConfig t; t.myId = myId; t.dest = tg; t.callsign = "LY1ABC"; t.colourCode = 7; t.slot = 2;
        t.call = DmrTxCall::GroupVoice; t.voiceSeconds = 4.5;
        DmrRunResult r = runTx(t, 8, 18, 1200, 30);
        const DmrCall* c = findVoice(r, myId, tg, 0);
        CHECK(c, "group voice (impaired): not logged");
        CHECK(r.tel.cc == 7, "group voice (impaired): colour code %d (wanted 7)", r.tel.cc);
    }

    // ---- private (unit-to-unit) voice call
    {
        DmrTxConfig t; t.myId = myId; t.dest = unit; t.callsign = "LY1ABC"; t.colourCode = 3; t.slot = 1;
        t.call = DmrTxCall::PrivateVoice; t.voiceSeconds = 3.6;
        DmrRunResult r = runTx(t, 7, 40);
        CHECK(findVoice(r, myId, unit, 1), "private voice: call %u -> %u not logged", myId, unit);
    }

    // ---- all call
    {
        DmrTxConfig t; t.myId = myId; t.colourCode = 1; t.slot = 1; t.call = DmrTxCall::AllCallVoice; t.voiceSeconds = 3.6;
        DmrRunResult r = runTx(t, 7, 40);
        CHECK(findVoice(r, myId, 0xFFFFFF, 2), "all call: not logged");
    }

    // ---- SMS text message
    {
        const std::string text = "OnAir DMR TX test de LY1ABC";
        DmrTxConfig t; t.myId = myId; t.dest = tg; t.colourCode = 1; t.slot = 1; t.call = DmrTxCall::Sms; t.text = text; t.voiceSeconds = 0;
        DmrRunResult r = runTx(t, 6, 45);
        bool found = false;
        for (const DmrMessage& m : r.tel.messages) if (m.text == text && m.src == myId && m.crcOk) found = true;
        CHECK(found, "SMS: message \"%s\" from %u not received with a good CRC", text.c_str(), myId);
    }

    // ---- LRRP position report
    {
        DmrTxConfig t; t.myId = myId; t.dest = tg; t.colourCode = 1; t.slot = 1; t.call = DmrTxCall::Lrrp; t.lat = 54.6872; t.lon = 25.2797;
        DmrRunResult r = runTx(t, 6, 45);
        bool found = false;
        for (const DmrLocation& l : r.tel.locations)
            if (l.hasFix && l.src == myId && std::fabs(l.lat - t.lat) < 1e-3 && std::fabs(l.lon - t.lon) < 1e-3) found = true;
        CHECK(found, "LRRP: position %.4f,%.4f from %u not received", t.lat, t.lon, myId);
    }

    // ---- Tier III voice service request (control-channel CSBK). Simulated only: no live system was involved.
    {
        DmrTxConfig t; t.myId = myId; t.dest = tg; t.colourCode = 1; t.slot = 1; t.call = DmrTxCall::Tier3VoiceRequest;
        DmrRunResult r = runTx(t, 5, 45);
        bool found = false;
        for (const DmrCall& c : r.tel.callLog) if (c.kind == 4 && c.src == myId && c.dst == tg) found = true;
        CHECK(found, "Tier III voice request: CSBK %u -> %u not logged as control", myId, tg);
    }

    // ---- slot selection: transmitting in slot 2 is heard in slot 2. Checked in direct mode, whose sync patterns carry the slot (DM TS1 / TS2);
    // a lone repeater uplink has no CACH, so a receiver cannot number its slot (TS 102 361-1 4.5: the slot comes from the outbound CACH).
    {
        DmrTxConfig t; t.myId = myId; t.dest = tg; t.callsign = "LY1ABC"; t.colourCode = 1; t.slot = 2; t.mobile = false;
        t.call = DmrTxCall::GroupVoice; t.voiceSeconds = 4.0;
        DmrRunResult r = runTx(t, 7, 40);
        const DmrCall* c = findVoice(r, myId, tg, 0);
        CHECK(c, "slot 2 voice: not logged");
        if (c) CHECK(c->slot == 2, "slot 2 voice: logged in slot %d", c->slot);
    }

    printf(fails ? "test_dmr_tx: %d FAILED\n" : "test_dmr_tx: all passed\n", fails);
    return fails ? 1 : 0;
}
