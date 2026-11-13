// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// sens_sweep registry entries added for the 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md), used by tests/sens/test_sens39d.cpp.
// Same conventions as sens_modes.h: the mode's own test signal (noise free), the receiver set up as the engine sets it up, "valid
// output" = the receiver's own count of units whose check passed. SNR = active signal power against the noise in bwHz.
#pragma once
#include "sens_modes.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/ale/ale_tel.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_tel.h"
#include "dect2/edacs/edacs_gen.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/fusion/fusion_gen.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/mpt1327/mpt1327_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/smartnet/smartnet_gen.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/smartnet/smartnet_tel.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/wxsat/wxsat_tel.h"

namespace sens {

struct AptRx : WxsatReceiver { AptRx() : WxsatReceiver(0) {} };   // NOAA APT profile

// a finite test signal repeated until n samples
inline Sig repeatTo(const Sig& one, size_t n) {
    Sig v; v.reserve(n);
    while (v.size() < n && !one.empty()) v.insert(v.end(), one.begin(), one.begin() + (long)std::min(one.size(), n - v.size()));
    v.resize(n);
    return v;
}

inline std::vector<Mode> extraModesD() {
    std::vector<Mode> v;
    auto base = [](const char* id, const char* name, const char* fam, int std_, double rate, double secs, double bw, double mhz) {
        Mode m; m.id = id; m.name = name; m.family = fam; m.stdMode = std_; m.rate = rate; m.secs = secs; m.bwHz = bw; m.mhz = mhz; m.maxJobs = 4;
        return m;
    };
    {   // pagers one at a time (the "pager" entry sends POCSAG and FLEX in turn): code words that passed
        for (int kind : {1, 2}) {
            const ModeTuning* t = modeTuning(25);
            Mode m = base(kind == 1 ? "pocsag" : "flex", kind == 1 ? "POCSAG (512 / 1200 / 2400)" : "FLEX (1600 / 3200 / 6400)", "narrowband-data", 25,
                          t ? t->sampleRate : 1.2e6, 8.0, 25e3, 466);
            m.snrHi = 25;
            m.gen = [kind, r = m.rate](size_t n) { SynthConfig c; c.modeOpt[0] = kind; return clean(25, c, r, n); };
            m.make = factory<PagerReceiver, PagerTelemetry>([](PagerReceiver& rx, double r) { prepGeneric(rx, r, 25); }, blocks<PagerTelemetry>);
            v.push_back(m);
        }
    }
    {   // Yaesu System Fusion (the sens_modes.h entry has no test signal: std 130 has no mode synth): V/D mode 1 transmissions, FICH frames that checked
        Mode m = base("fusion39", "Yaesu System Fusion (C4FM)", "digital-voice", 130, 250e3, 6.0, 12.5e3, 144);
        m.snrHi = 25;
        m.gen = [](size_t n) {
            Sig one;
            for (int k = 0; k < 4; k++) {
                fusion::GenParams p; p.seed = (uint32_t)(k + 1); p.commFrames = 12; p.dt = k & 1 ? 2 : 0;
                std::vector<uint8_t> bits;
                fusion::genTransmissionBits(p, bits);
                const auto x = fusion::modulateC4fm(bits, 250e3, 0, 0.5f, 400);
                one.insert(one.end(), x.begin(), x.end());
            }
            return repeatTo(one, n);
        };
        m.make = factory<FusionReceiver, FusionTelemetry>([](FusionReceiver& rx, double r) { rx.configure(r); rx.setSignalOffset(0); }, blocks<FusionTelemetry>);
        v.push_back(m);
    }
    {   // DVB-S (EN 300 421) QPSK 2/3 at 1 Msym/s; SNR = Es/N0 (noise bandwidth = symbol rate). Transport packets that passed Reed-Solomon
        Mode m = base("dvbs1", "DVB-S QPSK 2/3 (1 Msym/s)", "satellite-tv", 8, 4e6, 1.5, 1e6, 11700);
        m.echoUs = 0.4; m.snrHi = 16;
        m.gen = [](size_t n) { SynthConfig c; c.modeOpt[0] = 1; c.modeVal[0] = 1e6; return clean(8, c, 4e6, n); };
        m.make = factory<DvbsReceiver, DvbsTelemetry>([](DvbsReceiver& rx, double r) { rx.setBlocking(true); rx.configure(r); }, blocks<DvbsTelemetry>, false,
                                                      [](DvbsReceiver&) {});
        v.push_back(m);
    }
    {   // HF ALE (2G, 8-FSK 125 Bd): words decoded; SNR in 3 kHz
        const ModeTuning* t = modeTuning(51);
        Mode m = base("ale", "HF ALE 2G", "hf-data", 51, t ? t->sampleRate : 250e3, 20.0, 3e3, 14.109);
        m.snrHi = 20;
        m.gen = [r = m.rate](size_t n) { return clean(51, SynthConfig(), r, n); };
        m.make = factory<AleReceiver, AleTelemetry>([](AleReceiver& rx, double r) { prepGeneric(rx, r, 51); }, blocks<AleTelemetry>);
        v.push_back(m);
    }
    {   // NOAA APT (FM, 2400 Hz subcarrier): picture lines with sync; SNR in the 34 kHz FM channel
        const ModeTuning* t = modeTuning(262);
        Mode m = base("apt", "NOAA APT", "satellite-weather", 262, t ? t->sampleRate : 250e3, 8.0, 34e3, 137.62);
        m.snrHi = 25;
        m.gen = [r = m.rate](size_t n) { return clean(262, SynthConfig(), r, n); };
        m.make = factory<AptRx, WxsatTelemetry>([](AptRx& rx, double r) { prepGeneric(rx, r, 262); }, blocks<WxsatTelemetry>, true,
                                                [](AptRx& rx) { rx.flush(); });
        v.push_back(m);
    }
    {   // EDACS control channel (9600 bit/s GFSK): frames that passed BCH; SNR in 12.5 kHz
        Mode m = base("edacs", "EDACS control channel", "trunking", 132, 480e3, 6.0, 12.5e3, 856);
        m.snrHi = 25;
        m.gen = [](size_t n) {
            EdacsGenConfig c; c.noise = 0; c.durSec = (double)n / c.rate + 0.2;
            return repeatTo(edacsSystemSignal(c), n);
        };
        m.make = factory<EdacsReceiver, EdacsTelemetry>([](EdacsReceiver& rx, double r) {
            EdacsGenConfig c; rx.configure(r); rx.setCenterHz(c.centreHz); rx.setSignalOffset(c.controlHz - c.centreHz); }, blocks<EdacsTelemetry>);
        v.push_back(m);
    }
    {   // Motorola Type II (SmartNet) control channel (3600 bit/s): OSWs that passed; SNR in 12.5 kHz
        Mode m = base("smartnet", "Motorola Type II control channel", "trunking", 322, 480e3, 6.0, 12.5e3, 851);
        m.snrHi = 25;
        m.gen = [](size_t n) {
            SmartnetGenConfig c; c.noise = 0; c.durSec = (double)n / c.rate + 0.2;
            return repeatTo(smartnetSystemSignal(c), n);
        };
        m.make = factory<SmartnetReceiver, SmartnetTelemetry>([](SmartnetReceiver& rx, double r) {
            SmartnetGenConfig c; rx.configure(r); rx.setCenterHz(c.centreHz); rx.setSignalOffset(c.controlHz - c.centreHz); }, blocks<SmartnetTelemetry>);
        v.push_back(m);
    }
    {   // MPT 1327 control channel (1200 bit/s FFSK on FM, 1.5 kHz deviation): codewords that passed; SNR in 12.5 kHz
        Mode m = base("mpt1327", "MPT 1327 control channel", "trunking", 129, 240e3, 6.0, 12.5e3, 170);
        m.snrHi = 30;
        m.gen = [](size_t n) {
            using namespace mpt1327;
            std::vector<uint64_t> cw;
            for (int k = 0; k < 24; k++) cw.push_back(k % 3 == 0 ? makeGtc(5, 1234 + k, 0, 17, 2003) : k % 3 == 1 ? makeAloha(5, 0, 0, 0) : makeAhy(5, 100 + k));
            const auto bits = controlChannelBits(20001, cw, 16);
            const auto aud = ffskAudio(bits, 240e3, false, 0.5f);
            const auto iq = fmModulate(aud, 240e3, 3000.0, 0.0);
            return repeatTo(Sig(iq.begin(), iq.end()), n);
        };
        m.make = factory<MptReceiver, MptTelemetry>([](MptReceiver& rx, double r) { rx.configure(r); rx.setSignalOffset(0); rx.setCenterHz(170e6); }, blocks<MptTelemetry>);
        v.push_back(m);
    }
    return v;
}

} // namespace sens
