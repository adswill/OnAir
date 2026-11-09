// Profile and link tables of the SatDump tier 3 modes (sd3_tel.h). Numbers from SatDump's resources/pipelines (GPL-3.0, Copyright Aang23 and contributors).
#include "dect2/sd3/sd3_tel.h"
#include "dect2/meteor/meteor_proto.h"

namespace dect2 {

static const Sd3ProfileInfo kProfiles[kSd3NumProfiles] = {
    {"goesgrb", "GOES GRB", 290, 20e6, 11e6},
    {"goesgvar", "GOES GVAR", 291, 6e6, 4.5e6},
    {"goescda", "GOES CDA", 292, 1e6, 0.25e6},
    {"goesmdl", "GOES MDL", 293, 3e6, 0.5e6},
    {"gk2auhrit", "GK-2A UHRIT", 294, 20e6, 18e6},
    {"gk2acdas", "GK-2A CDAS", 295, 8e6, 6.5e6},
    {"sband", "S-band CCSDS", 296, 10e6, 0.5e6},
    {"geoscan", "Geoscan", 297, 250000, 96000},
    {"lucky7", "Lucky-7", 298, 250000, 96000},
    {"spino", "Spino", 299, 250000, 48000},
    {"erminaz", "Erminaz", 510, 250000, 96000},
};
const Sd3ProfileInfo& sd3Profile(int p) { return kProfiles[p < 0 || p >= kSd3NumProfiles ? 0 : p]; }

// sync word constant used for the CADU links
static constexpr uint32_t A = 0x1ACFFC1Du;


// id, name, MHz, symbol rate, roll-off, modulation, convolutional, frame bits, NRZ-M, randomised, randomiser after RS, dual basis, interleave, RS, fill, sync, I/Q swap
static const Sd3Link kGrb[] = {{"grb", "GOES-R GRB", 1686.6, 8665938, 0.25, 0, false, 2048 * 8, false, false, false, true, 0, 0, 0, A, false}};
static const Sd3Link kGvar[] = {{"gvar", "GOES GVAR", 1685.7, 2.11e6, 0.5, kSd3Bpsk, false, 32786 * 8, false, false, false, true, 0, 0, 0, A, false}};
static const Sd3Link kCda[] = {
    {"cda40", "GOES-R CDA 40 kbit/s", 1693, 40000, 0.5, kSd3Bpsk, false, 2048, true, true, false, true, 1, 223, 0, A, false},
    {"cda4", "GOES-R CDA 4 kbit/s", 1693, 4000, 0.5, kSd3Bpsk, false, 2048, true, true, false, true, 1, 223, 0, A, false},
    {"cdan", "GOES-N CDA (PM)", 1694, 4000, 0.6, kSd3Pm, false, 4096, false, false, false, true, 0, 0, 0, 0xFAF3201Cu, false},
};
static const Sd3Link kMdl[] = {{"mdl", "GOES-N MDL", 1681.5, 200000, 0.5, kSd3Qpsk, false, 464 * 8, false, false, false, true, 0, 0, 0, A, false}};
static const Sd3Link kUhrit[] = {{"uhrit", "GK-2A UHRIT", 8070, 15622440, 0.25, 0, false, 2048 * 8, false, false, false, true, 0, 0, 0, A, false}};
static const Sd3Link kCdas[] = {{"cdas", "GK-2A CDAS", 0, 3.096e6, 0.5, kSd3Qpsk, false, 10112, false, true, false, true, 5, 223, 3, A, false}};
static const Sd3Link kSband[] = {
#include "sd3_links.inc"
};
// the Geoscan satellites differ only in frequency
static const Sd3Link kGeoscan[] = {
    {"stratosat", "StratoSat-TK1", 435.87, 9600, 0.5, kSd3Fsk, false, (66 + 4) * 8, false, false, false, true, 0, 0, 0, 0x930B51DEu, false},
    {"rtumirea", "RTU-MIREA1", 436.2, 9600, 0.5, kSd3Fsk, false, (66 + 4) * 8, false, false, false, true, 0, 0, 0, 0x930B51DEu, false},
    {"tusurgo", "TUSUR-GO", 437.73, 9600, 0.5, kSd3Fsk, false, (66 + 4) * 8, false, false, false, true, 0, 0, 0, 0x930B51DEu, false},
    {"vizardion", "VIZARD-ION", 436.87, 9600, 0.5, kSd3Fsk, false, (66 + 4) * 8, false, false, false, true, 0, 0, 0, 0x930B51DEu, false},
    {"colibris", "COLIBRI-S", 436.835, 9600, 0.5, kSd3Fsk, false, (66 + 4) * 8, false, false, false, true, 0, 0, 0, 0x930B51DEu, false},
    {"horizon", "HORIZON", 435.43, 9600, 0.5, kSd3Fsk, false, (66 + 4) * 8, false, false, false, true, 0, 0, 0, 0x930B51DEu, false},
};
static const Sd3Link kLucky7[] = {{"lucky7", "Lucky-7", 437.525, 4800, 0.5, kSd3Fsk, false, 312, false, false, false, true, 0, 0, 0, A, false}};
static const Sd3Link kSpino[] = {{"spino", "Spino 2400 bit/s", 435.2, 2400, 0.5, kSd3Fsk, false, 0, false, false, false, true, 0, 0, 0, A, false}};
static const Sd3Link kErminaz[] = {{"erminaz", "ERMINAZ UHF", 435.775, 9600, 0.5, kSd3Fsk, false, 2072, false, true, false, true, 1, 223, 0, A, false}};

#define TAB(X) {X, (int)(sizeof(X) / sizeof(X[0]))}
static const struct { const Sd3Link* t; int n; } kTabs[kSd3NumProfiles] = {
    TAB(kGrb), TAB(kGvar), TAB(kCda), TAB(kMdl), TAB(kUhrit), TAB(kCdas), TAB(kSband), TAB(kGeoscan), TAB(kLucky7), TAB(kSpino), TAB(kErminaz)};
#undef TAB

int sd3LinkCount(int p) { return kTabs[p < 0 || p >= kSd3NumProfiles ? 0 : p].n; }
const Sd3Link& sd3LinkAt(int p, int i) {
    const auto& t = kTabs[p < 0 || p >= kSd3NumProfiles ? 0 : p];
    return t.t[i < 0 || i >= t.n ? 0 : i];
}

} // namespace dect2
