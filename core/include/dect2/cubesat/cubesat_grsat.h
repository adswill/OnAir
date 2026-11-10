// The satellite table of gr-satellites (python/satyaml, GPL-3.0-or-later, Daniel Estevez EA4GPZ and contributors): per NORAD number the
// transmitters with frequency, modulation, baud, framing and the telemetry decoder gr-satellites uses. Generated into
// core/src/cubesat/cubesat_grsat_data.inc by tools/cubesat/satyaml2inc.py (regenerate, do not edit). The CubeSat receiver uses it to set
// itself up for the satellite of a pass: the framing, its parameters (CCSDS frame size, code, basis, Mobitex variant) and the baud rate.
#pragma once
#include "cubesat_more.h"
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

enum class GrFraming { G3ruh, Ax25, Ax100M5, Ax100M6, Usp, U482c, CcsdsConcat, CcsdsRs, Ao40, Mobitex, MobitexNx, Beesat1, Beesat9, Ngham, NghamNoRs, Other };
constexpr int kGrDiff = 1, kGrRsConv = 2, kGrNasaDsn = 4, kGrUninverted = 8, kGrNoScrambler = 16;

struct GrTx {
    int norad;
    const char* sat;
    const char* tx;                 // "9k6 FSK downlink"
    long long hz;
    const char* modulation;         // "FSK", "AFSK", "BPSK", "DBPSK", ...
    int baud;
    GrFraming framing;
    const char* framingName;        // gr-satellites' name
    const char* telemetry;          // gr-satellites telemetry decoder(s), "+" separated ("" none)
    int deviation, afCarrier;       // Hz (AFSK: the audio carrier)
    int frameSize, rsInterleave;    // CCSDS
    int flags;                      // kGr*
    const char* callsign;
};

const std::vector<GrTx>& grsatTable();
std::vector<const GrTx*> grsatFor(int norad);
// cubesat::Framing of the entry (kFramingAo40 of cubesat_telem.h for AO-40 FEC), -1 when OnAir has no decoder for that framing
int grsatFraming(const GrTx& t);
// whether the CubeSat telemetry receiver (one FM channel) can decode it: an FSK / GMSK or AFSK transmitter with a framing it decodes
bool grsatReceiverDecodes(const GrTx& t);
ConcatParams grsatConcat(const GrTx& t);
int grsatMobitexVariant(const GrTx& t);
// OnAir's field decoder for gr-satellites' telemetry name ("FUNcube", "GOMX-1", "AX.25 header", "CSP header"; "" none)
std::string grsatFieldDecoder(const GrTx& t);

// The transmitter of a satellite the receiver sets itself up for: the decodable one nearest the dial (dialHz 0: the first decodable one).
// nullptr when the satellite is not in the table or none of its transmitters can be decoded by the receiver.
const GrTx* grsatChoose(int norad, double dialHz);
std::string grsatDescribe(const GrTx& t);       // "USP, FSK 9600 baud, 436.6 MHz (field decoder: CSP header)"

} // namespace cubesat
} // namespace dect2
