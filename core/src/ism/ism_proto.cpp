// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors: pulse slicers and device decoders. See ism_proto.h.
//
// Written from rtl_433 (GPL-2.0, https://github.com/merbanan/rtl_433): the slicers follow src/pulse_slicer.c, the bit helpers
// src/bitbuffer.c and src/bit_util.c, and every decoder names the src/devices/ file it was written from. The timings in the protocol
// table are the r_device values of those files.
#include "dect2/ism/ism_proto.h"
#include "ism_internal.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dect2 {

// ---------------------------------------------------------------- bit_util.c

uint8_t ismCrc8(const uint8_t* m, unsigned n, uint8_t poly, uint8_t init) {
    uint8_t r = init;
    for (unsigned i = 0; i < n; i++) {
        r ^= m[i];
        for (int b = 0; b < 8; b++) r = (r & 0x80) ? (uint8_t)((r << 1) ^ poly) : (uint8_t)(r << 1);
    }
    return r;
}

uint16_t ismCrc16(const uint8_t* m, unsigned n, uint16_t poly, uint16_t init) {
    uint16_t r = init;
    for (unsigned i = 0; i < n; i++) {
        r ^= (uint16_t)(m[i] << 8);
        for (int b = 0; b < 8; b++) r = (r & 0x8000) ? (uint16_t)((r << 1) ^ poly) : (uint16_t)(r << 1);
    }
    return r;
}

uint8_t ismLfsrDigest8(const uint8_t* m, unsigned n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    for (unsigned k = 0; k < n; k++)
        for (int i = 7; i >= 0; i--) {
            if ((m[k] >> i) & 1) sum ^= key;
            key = (key & 1) ? (uint8_t)((key >> 1) ^ gen) : (uint8_t)(key >> 1);
        }
    return sum;
}

uint8_t ismLfsrDigest8Reflect(const uint8_t* m, int n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    for (int k = n - 1; k >= 0; k--)
        for (int i = 0; i < 8; i++) {
            if ((m[k] >> i) & 1) sum ^= key;
            key = (key & 0x80) ? (uint8_t)((key << 1) ^ gen) : (uint8_t)(key << 1);
        }
    return sum;
}

uint16_t ismLfsrDigest16(const uint8_t* m, unsigned n, uint16_t gen, uint16_t key) {
    uint16_t sum = 0;
    for (unsigned k = 0; k < n; k++)
        for (int i = 7; i >= 0; i--) {
            if ((m[k] >> i) & 1) sum ^= key;
            key = (key & 1) ? (uint16_t)((key >> 1) ^ gen) : (uint16_t)(key >> 1);
        }
    return sum;
}

namespace {

// Merges every run shorter than minUs into the run before it (the run after it joins that one too): a noise spike of the FSK
// discriminator inside a chip. A train is a list of mark / space runs.
IsmTrain despike(const IsmTrain& t, float minUs) {
    std::vector<float> d;
    for (const auto& q : t.p) { d.push_back(q.pulseUs); d.push_back(q.gapUs); }
    for (size_t i = 1; i + 1 < d.size();) {
        if (d[i] < minUs) { d[i - 1] += d[i] + d[i + 1]; d.erase(d.begin() + (ptrdiff_t)i, d.begin() + (ptrdiff_t)i + 2); }
        else i++;
    }
    IsmTrain o; o.fsk = t.fsk;
    for (size_t i = 0; i + 1 < d.size(); i += 2) o.p.push_back({d[i], d[i + 1]});
    return o;
}

// ---------------------------------------------------------------- the table

thread_local std::vector<uint32_t> tlRemoteSeen;   // the remote codes seen in the current train (ismDecode clears it)

struct Proto {
    IsmProtoInfo info;
    Mod mod;
    Timing t;
    std::function<void(Bits&, Out&)> fn;
    float despikeUs = 0;   // FSK noise: tone runs shorter than this are merged into their neighbours before slicing
};

const std::vector<Proto>& protos() {
    static const std::vector<Proto> p = {
        {{"Nexus / FreeTec / TFA 30.3209", "nexus.c", "Weather", false}, OokPpm, {1000, 2000, 5000, 3000, 0, 0}, decNexus},
        {{"Rubicson / TFA 30.3197", "rubicson.c", "Weather", false}, OokPpm, {1000, 2000, 4800, 3000, 0, 0}, decRubicson},
        {{"Generic temperature sensor 1", "generic_temperature_sensor.c", "Weather", false}, OokPpm, {2000, 4000, 10000, 4800, 0, 0}, decGenericTemp},
        {{"LaCrosse TX141 / TX141TH-Bv2 / TFA 30.3221", "lacrosse_tx141x.c", "Weather", false}, OokPwm, {208, 417, 1700, 625, 833, 0}, decTx141},
        {{"Acurite 592TXR tower, 5-in-1", "acurite.c", "Weather", false}, OokPwm, {220, 408, 4000, 500, 620, 0}, decAcurite},
        {{"Fine Offset WH1080 / WH3080", "fineoffset_wh1080.c", "Weather", false}, OokPwm, {544, 1524, 2800, 0, 0, 0}, decWh1080},
        {{"Ambient Weather F007TH / TFA 30.3208", "ambient_weather.c", "Weather", false}, OokMcZero, {500, 0, 2400, 0, 0, 0}, decF007th},
        {{"Oregon Scientific v2.1 / v3", "oregon_scientific.c", "Weather", false}, OokMcZero, {440, 0, 2400, 0, 0, 0}, decOregon},
        {{"Acurite 606TX / Technoline TX960", "acurite.c", "Weather", false}, OokPpm, {2000, 4000, 10000, 7000, 0, 0}, decAcurite606},
        {{"Acurite 609TXC", "acurite.c", "Weather", false}, OokPpm, {1000, 2000, 10000, 3000, 0, 0}, decAcurite609},
        {{"Prologue / FreeTec NC-7104", "prologue.c", "Weather", false}, OokPpm, {2000, 4000, 10000, 7000, 0, 0}, decPrologue},
        {{"Kedsum / Pearl NC-7415", "kedsum.c", "Weather", false}, OokPpm, {2000, 4000, 9400, 4400, 0, 0}, decKedsum},
        {{"Globaltronics GT-WT-02", "gt_wt_02.c", "Weather", false}, OokPpm, {2500, 5000, 12000, 8000, 0, 0}, decGtWt02},
        {{"Bresser 3CH thermo-hygro", "bresser_3ch.c", "Weather", false}, OokPwm, {250, 500, 1250, 625, 750, 0}, decBresser3ch},
        {{"Fine Offset WH2 / WH2A / WH5, Telldus", "fineoffset.c", "Weather", false}, OokPwm, {500, 1500, 1200, 0, 0, 160}, decWh2},
        {{"ThermoPro TP11", "thermopro_tp11.c", "Weather", false}, OokPpm, {500, 1500, 4000, 2000, 0, 0}, decTp11},
        {{"ThermoPro TP08 / TP12 / TP20", "thermopro_tp12.c", "Weather", false}, OokPpm, {500, 1500, 4000, 2000, 0, 0}, decTp12},
        {{"FT-004-B temperature sensor", "ft004b.c", "Weather", false}, OokPpm, {1956, 3900, 4000, 4000, 0, 0}, decFt004b},
        {{"Itron ERT SCM (utility meter)", "ert_scm.c", "Meter", false}, OokMcZero, {30, 0, 64, 0, 0, 0}, decErtScm},
        {{"Itron ERT IDM (utility meter)", "ert_idm.c", "Meter", false}, OokMcZero, {30, 0, 20000, 0, 0, 0}, decErtIdm},
        {{"Solight TE44 / TE66, EMOS E0107T", "solight_te44.c", "Weather", false}, OokPpm, {972, 1932, 6000, 3000, 0, 0}, decSolightTe44},
        {{"Honeywell / Ademco 345 MHz door and window sensors", "honeywell.c", "Sensor", false}, OokPcm, {136, 136, 408, 0, 0, 0}, decHoneywell},
        {{"Kerui PIR / contact sensor", "kerui.c", "Sensor", false}, OokPwm, {420, 960, 9900, 1100, 0, 160}, decKerui},
        {{"Springfield temperature and soil moisture", "springfield.c", "Weather", false}, OokPpm, {2000, 4000, 9200, 5000, 0, 0}, decSpringfield},
        {{"Schrader TPMS", "schraeder.c", "TPMS", false}, OokMcZero, {120, 0, 480, 0, 0, 0}, decSchrader},
        {{"Schrader EG53MA4 TPMS", "schraeder.c", "TPMS", false}, OokMcZero, {123, 0, 300, 0, 0, 0}, decSchraderEg53},
        {{"EV1527 / PT2262 remotes", "generic_remote.c", "Remote", false}, OokPwm, {464, 1404, 1800, 0, 0, 200},
         [](Bits& b, Out& o) { decGenericRemote(b, o, tlRemoteSeen); }},
        {{"Bresser 5-in-1", "bresser_5in1.c", "Weather", true}, FskPcm, {124, 124, 25000, 0, 0, 0}, decBresser5in1},
        {{"Bresser 6-in-1", "bresser_6in1.c", "Weather", true}, FskPcm, {124, 124, 25000, 0, 0, 0}, decBresser6in1},
        {{"Bresser 7-in-1 / air quality", "bresser_7in1.c", "Weather", true}, FskPcm, {124, 124, 25000, 0, 0, 0}, decBresser7in1},
        {{"LaCrosse TX29-IT / TFA 30.3159", "lacrosse_tx35.c", "Weather", true}, FskPcm, {55, 55, 4000, 0, 0, 0}, [](Bits& b, Out& o) { decTx29(b, o, false); }},
        {{"LaCrosse TX35DTH-IT / TFA 30.3155", "lacrosse_tx35.c", "Weather", true}, FskPcm, {105, 105, 4000, 0, 0, 0}, [](Bits& b, Out& o) { decTx29(b, o, true); }},
        {{"Fine Offset / Ecowitt WH24 / WH65", "fineoffset.c", "Weather", true}, FskPcm, {58, 58, 20000, 0, 0, 0}, decWh24},
        {{"Toyota TPMS (PMV-C210)", "tpms_toyota.c", "TPMS", true}, FskPcm, {52, 52, 150, 0, 0, 0}, decToyota},
        {{"PMV-107J TPMS (Toyota)", "tpms_pmv107j.c", "TPMS", true}, FskPcm, {100, 100, 250, 0, 0, 0}, decPmv107j},
        {{"Citroen / Peugeot TPMS", "tpms_citroen.c", "TPMS", true}, FskPcm, {52, 52, 150, 0, 0, 0}, decCitroen},
        {{"Renault TPMS", "tpms_renault.c", "TPMS", true}, FskPcm, {52, 52, 150, 0, 0, 0}, decRenault},
        {{"Ford TPMS", "tpms_ford.c", "TPMS", true}, FskPcm, {52, 52, 150, 0, 0, 0}, decFord},
        {{"Hyundai / Kia TPMS (VDO)", "tpms_hyundai_vdo.c", "TPMS", true}, FskPcm, {52, 52, 150, 0, 0, 0}, decHyundai},
        {{"Abarth 124 TPMS (VDO TG1C)", "tpms_abarth124.c", "TPMS", true}, FskPcm, {52, 52, 150, 0, 0, 0}, decAbarth},
        {{"Efergy e2 classic", "efergy_e2_classic.c", "Energy", true}, FskPwm, {64, 136, 400, 200, 500, 0}, decEfergy},
        {{"CurrentCost TX / EnviR", "current_cost.c", "Energy", true}, FskPcm, {250, 250, 8000, 0, 0, 0}, decCurrentCost},
        {{"Z-Wave R1 (9.6 kbit/s)", "G.9959", "Z-Wave", true}, FskPcm, {52, 52, 3000, 0, 0, 0}, [](Bits& b, Out& o) { decZwave(b, o, 1); }, 25},
        {{"Z-Wave R2 (40 kbit/s)", "G.9959", "Z-Wave", true}, FskPcm, {25, 25, 1000, 0, 0, 0}, [](Bits& b, Out& o) { decZwave(b, o, 2); }, 10},
        {{"Z-Wave R3 (100 kbit/s)", "G.9959", "Z-Wave", true}, FskPcm, {10, 10, 500, 0, 0, 0}, [](Bits& b, Out& o) { decZwave(b, o, 3); }},
        {{"Wireless M-Bus T / C (header)", "m_bus.c", "Meter", true}, FskPcm, {10, 10, 500, 0, 0, 0}, decMbus},
        {{"KNX RF 1.1 (S-mode PHY, 32.768 kchip/s)", "m_bus.c", "Sensor", true}, FskPcm, {30.5f, 30.5f, 275, 0, 0, 0}, decKnxRf, 15},
        {{"DASH7 (55.555 kbit/s)", "Sub-IoT phy.c / dll.c", "Sensor", true}, FskPcm, {18, 18, 1500, 0, 0, 0}, [](Bits& b, Out& o) { decDash7(b, o, false); }, 8},
        {{"DASH7 (9.6 kbit/s)", "Sub-IoT phy.c / dll.c", "Sensor", true}, FskPcm, {104, 104, 8000, 0, 0, 0}, [](Bits& b, Out& o) { decDash7(b, o, true); }, 40},
        {{"EnOcean ERP1 (868.3 MHz ASK, 125 kbit/s)", "enocean_erp1.c", "Sensor", false}, OokPcm, {8, 8, 800, 0, 0, 0}, decEnOcean},
        {{"HomeMatic BidCoS (868.3 MHz, 10 kbit/s)", "culfw rf_asksin.c / FHEM HMConfig.pm", "Sensor", true}, FskPcm, {100, 100, 4000, 0, 0, 0}, decHomeMatic, 30},
        // 3.9 (ism_dec_39.cpp)
        {{"X10 RF remotes and sensors", "x10_rf.c", "Remote", false}, OokPpm, {562, 1687, 6000, 2200, 0, 0}, decX10Rf},
        {{"inFactory / nor-tec / DAY / Tchibo thermo-hygro", "infactory.c", "Weather", false}, OokPpm, {2000, 4000, 5000, 0, 500, 750}, decInfactory},
        {{"GM aftermarket TPMS", "tpms_gm.c", "TPMS", false}, OokMcZero, {120, 0, 15600, 0, 0, 0}, decGmTpms},
        {{"Itron ERT SCM+ (utility meter)", "scmplus.c", "Meter", false}, OokMcZero, {30, 0, 64, 0, 0, 0}, decScmPlus},
        {{"Insteon RF (915 MHz)", "insteon.c", "Sensor", true}, FskPcm, {110, 110, 1000, 500, 0, 15}, decInsteon},
        {{"Ambient Weather / Ecowitt WH31E, WH31B", "ambientweather_wh31e.c", "Weather", true}, FskPcm, {56, 56, 1500, 1800, 0, 0}, decWh31e},
        {{"Nissan TPMS", "tpms_nissan.c", "TPMS", true}, FskPcm, {120, 120, 250, 0, 0, 0}, decNissan},
        {{"Efergy Optical", "efergy_optical.c", "Energy", true}, FskPwm, {64, 136, 400, 0, 500, 0}, decEfergyOptical},
        {{"ESIC EMT7110 power meter", "esic_emt7110.c", "Energy", true}, FskPcm, {104, 104, 10000, 0, 0, 0}, decEmt7110},
    };
    return p;
}

} // namespace

const std::vector<IsmProtoInfo>& ismProtocols() {
    static const std::vector<IsmProtoInfo> v = [] { std::vector<IsmProtoInfo> r; for (const auto& p : protos()) r.push_back(p.info); return r; }();
    return v;
}

std::vector<IsmReading> ismDecode(const IsmTrain& t) {
    Out out;
    if (t.p.empty()) return out;
    for (const auto& q : t.p) if (!(q.pulseUs >= 0 && q.gapUs >= 0 && std::isfinite(q.pulseUs) && std::isfinite(q.gapUs))) return out;
    const auto& P = protos();
    tlRemoteSeen.clear();
    for (const auto& pr : P) {
        if (pr.info.fsk != t.fsk) continue;
        Emit emit = [&](Bits& b) { Bits c = b; pr.fn(c, out); };
        const IsmTrain clean = pr.despikeUs > 0 ? despike(t, pr.despikeUs) : IsmTrain();
        const IsmTrain& tt = pr.despikeUs > 0 ? clean : t;
        switch (pr.mod) {
        case OokPwm: case FskPwm: slicePwm(tt, pr.t, emit); break;
        case OokPpm: slicePpm(tt, pr.t, emit); break;
        case OokPcm: case FskPcm: slicePcm(tt, pr.t, emit); break;
        case OokMcZero: sliceMcZero(tt, pr.t, emit); break;
        }
    }
    // a device repeats its packet within a burst: keep one of each
    Out u;
    for (auto& r : out) {
        bool dup = false;
        for (const auto& k : u) {
            if (k.model != r.model || k.id != r.id || k.channel != r.channel || k.fields.size() != r.fields.size()) continue;
            bool same = true;
            for (size_t i = 0; i < k.fields.size() && same; i++) same = k.fields[i].value == r.fields[i].value && k.fields[i].text == r.fields[i].text;
            if (same) { dup = true; break; }
        }
        if (!dup) u.push_back(std::move(r));
    }
    return u;
}

// ---------------------------------------------------------------- unknown signals

namespace {
std::vector<float> clusters(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    std::vector<float> c;
    std::vector<int> cnt;
    for (float x : v) {
        if (!c.empty() && x <= c.back() * 1.3f + 2) { c.back() = (c.back() * (float)cnt.back() + x) / (float)(cnt.back() + 1); cnt.back()++; }
        else { c.push_back(x); cnt.push_back(1); }
    }
    std::vector<float> r;   // drop clusters under 10 % of the values (glitches, sync gaps, the end gap)
    for (size_t i = 0; i < c.size(); i++) if (cnt[i] * 10 >= (int)v.size()) r.push_back(c[i]);
    return r;
}
} // namespace

IsmGuess ismClassify(const IsmTrain& t) {
    IsmGuess g;
    if (t.p.size() < 4) { g.code = "?"; return g; }
    std::vector<float> pw, gw;
    for (size_t i = 0; i + 1 < t.p.size(); i++) { pw.push_back(t.p[i].pulseUs); gw.push_back(t.p[i].gapUs); }
    pw.push_back(t.p.back().pulseUs);
    std::vector<float> sg = gw;
    std::sort(sg.begin(), sg.end());
    const float med = sg.empty() ? 0 : sg[sg.size() / 2];
    std::vector<float> gIn;
    for (float x : gw) if (x < med * 6 + 1) gIn.push_back(x); else g.gapUs = std::max(g.gapUs, x);
    const auto pc = clusters(pw), gc = clusters(gIn);
    auto nearBy = [](float a, float b) { return std::fabs(a - b) <= 0.25f * std::max(a, b) + 2; };
    if (pc.size() == 1 && gc.size() == 2) { g.code = "PPM"; g.shortUs = gc[0]; g.longUs = gc[1]; g.bits = (int)gIn.size(); }
    else if (pc.size() == 2 && gc.size() <= 2 && !nearBy(pc[1], 2 * pc[0]) ) { g.code = "PWM"; g.shortUs = pc[0]; g.longUs = pc[1]; g.bits = (int)pw.size(); }
    else if (pc.size() <= 2 && gc.size() <= 2 && nearBy(pc[0], gc.empty() ? pc[0] : gc[0]) &&
             (pc.size() < 2 || nearBy(pc[1], 2 * pc[0])) && (gc.size() < 2 || nearBy(gc[1], 2 * gc[0]))) {
        g.code = "Manchester"; g.shortUs = pc[0]; g.longUs = 2 * pc[0];
        float total = 0;
        for (float x : pw) total += x;
        for (float x : gIn) total += x;
        g.bits = (int)(total / (2 * pc[0]) + 0.5f);
    } else if (pc.size() == 2 && gc.size() <= 2) { g.code = "PWM"; g.shortUs = pc[0]; g.longUs = pc[1]; g.bits = (int)pw.size(); }
    else {
        const float base = std::min(pc.empty() ? 1e9f : pc[0], gc.empty() ? 1e9f : gc[0]);
        bool mult = base > 0 && base < 1e8f;
        for (float c : pc) mult = mult && std::fabs(c / base - std::round(c / base)) < 0.2f;
        for (float c : gc) mult = mult && std::fabs(c / base - std::round(c / base)) < 0.2f;
        if (mult) {
            g.code = "NRZ"; g.shortUs = base; g.longUs = base;
            float total = 0;
            for (float x : pw) total += x;
            for (float x : gIn) total += x;
            g.bits = (int)(total / base + 0.5f);
        } else g.code = "?";
    }
    return g;
}

} // namespace dect2
