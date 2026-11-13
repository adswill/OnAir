// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The gr-satellites satellite table: see cubesat_grsat.h. Data: gr-satellites python/satyaml (GPL-3.0-or-later, Daniel Estevez EA4GPZ and
// contributors), converted by tools/cubesat/satyaml2inc.py.
#include "dect2/cubesat/cubesat_grsat.h"
#include "dect2/cubesat/cubesat_proto.h"
#include "dect2/cubesat/cubesat_telem.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace cubesat {

const std::vector<GrTx>& grsatTable() {
    static const std::vector<GrTx> t = {
#include "cubesat_grsat_data.inc"
    };
    return t;
}

std::vector<const GrTx*> grsatFor(int norad) {
    std::vector<const GrTx*> v;
    if (norad <= 0) return v;
    for (const GrTx& t : grsatTable()) if (t.norad == norad) v.push_back(&t);
    return v;
}

int grsatFraming(const GrTx& t) {
    switch (t.framing) {
    case GrFraming::G3ruh: return Ax25G3ruh;
    case GrFraming::Ax25: return std::strcmp(t.modulation, "AFSK") == 0 ? Ax25Afsk1200 : Ax25G3ruh;
    case GrFraming::Ax100M5: return Ax100Mode5;
    case GrFraming::Ax100M6: return Ax100Mode6;
    case GrFraming::CcsdsRs: return CcsdsRs;
    case GrFraming::Usp: return Usp;
    case GrFraming::U482c: return U482c;
    case GrFraming::CcsdsConcat: return CcsdsConcat;
    case GrFraming::Ngham: case GrFraming::NghamNoRs: return Ngham;
    case GrFraming::Mobitex: case GrFraming::MobitexNx: case GrFraming::Beesat1: case GrFraming::Beesat9: return Mobitex;
    case GrFraming::Ao40: return kFramingAo40;
    default: return -1;
    }
}

bool grsatReceiverDecodes(const GrTx& t) {
    const int f = grsatFraming(t);
    if (f < 0 || f == kFramingAo40) return false;               // AO-40 FEC is BPSK: the Smart CubeSat decoder
    const bool fsk = std::strcmp(t.modulation, "FSK") == 0, afsk = std::strcmp(t.modulation, "AFSK") == 0;
    if (afsk) return (f == Ax25Afsk1200 && t.baud >= 1100 && t.baud <= 1300) || (t.afCarrier > 0 && t.baud > 0);
    if (!fsk) return false;
    if (t.framing == GrFraming::Ax25) return t.baud >= 300 && t.baud <= 19200;       // plain NRZI AX.25 on FSK
    return t.baud >= 300 && t.baud <= 19200;
}

ConcatParams grsatConcat(const GrTx& t) {
    ConcatParams p;
    if (t.frameSize > 0) p.frameSize = t.frameSize;
    p.interleave = std::max(1, t.rsInterleave);
    p.dualBasis = !(t.flags & kGrRsConv);
    p.differential = (t.flags & kGrDiff) != 0;
    p.scrambler = !(t.flags & kGrNoScrambler);
    const bool nasa = t.flags & kGrNasaDsn, uninv = t.flags & kGrUninverted;
    p.conv = nasa ? (uninv ? ConvNasaDsnUninv : ConvNasaDsn) : (uninv ? ConvCcsdsUninv : ConvCcsds);
    return p;
}

int grsatMobitexVariant(const GrTx& t) {
    if (t.framing == GrFraming::Beesat1) return MobitexBeesat1;
    if (t.framing == GrFraming::Beesat9) return MobitexBeesat9;
    return MobitexDefault;
}

std::string grsatFieldDecoder(const GrTx& t) {
    const std::string tel = t.telemetry;
    if (tel.find("funcube") != std::string::npos) return "FUNcube";
    if (tel.find("gomx_1") != std::string::npos) return "GOMX-1";
    if (tel.find("csp") != std::string::npos) return "CSP header";
    if (tel.find("ax25") != std::string::npos) return "AX.25 header";
    return "";
}

const GrTx* grsatChoose(int norad, double dialHz) {
    const GrTx* best = nullptr;
    double bd = 1e18;
    for (const GrTx* t : grsatFor(norad)) {
        if (!grsatReceiverDecodes(*t)) continue;
        const double d = dialHz > 0 ? std::fabs((double)t->hz - dialHz) : 0;
        if (!best || d < bd) { best = t; bd = d; }
    }
    return best;
}

std::string grsatDescribe(const GrTx& t) {
    char b[200];
    const std::string fd = grsatFieldDecoder(t);
    snprintf(b, sizeof b, "%s, %s %d baud, %.3f MHz%s%s%s", t.framingName, t.modulation, t.baud, (double)t.hz / 1e6, fd.empty() ? "" : " (fields: ",
             fd.c_str(), fd.empty() ? "" : ")");
    return b;
}

} // namespace cubesat
} // namespace dect2
