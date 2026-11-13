// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SBAS L1 message content: what the 250 bit messages of WAAS, EGNOS, MSAS, GAGAN, SDCM and the like carry (RTCA DO-229, public description).
// The bit layer (Viterbi, preamble, CRC-24Q) is in gnss_msg.h; this decodes the data field of a message with a good CRC into the PRN mask,
// the fast corrections with their UDRE, the integrity flags, the degradation factors, the GEO navigation message, the time offset, the GEO
// almanac, the ionospheric grid and the long-term corrections, and keeps them per satellite so a viewer can show the picture of the service.
//
// Bit numbers in this file are offsets into the 250 message bits, counted from 0: the preamble is 0..7, the type 8..13, the data 14..225, the CRC 226..249.
// Receive only. Nothing here applies a correction to a position: the corrections are shown, not used.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace sbas {

constexpr int kMsgBits = 250;
constexpr int kMaxSlots = 51;            // satellites a PRN mask can hold for the corrections
constexpr int kIonoBands = 11;           // ionospheric grid bands 0..10
constexpr int kIgpPerBand = 201;         // mask bits of one band

// Fields of the message bits (offset from 0, MSB first)
uint32_t getBits(const uint8_t* bits, int first, int len);
int32_t getSigned(const uint8_t* bits, int first, int len);
void putBits(uint8_t* bits, int first, int len, uint32_t v);

// CRC-24Q check of a 250 bit message (the first 226 bits against the last 24)
bool crcOk(const uint8_t* bits250);
// The preamble index (0, 1, 2) of the 8 bit preamble, -1 when it is not one of the three
int preambleIndex(const uint8_t* bits250);
int messageType(const uint8_t* bits250);

const char* typeName(int type);          // "PRN mask", "Fast corrections", ...
// What a PRN mask bit (1..210) stands for: "GPS 12", "GLONASS 3", "SBAS 131", "QZSS 195", "reserved"; PRN number in *prn (0 when none)
std::string maskBitName(int bit, int* prn = nullptr);
double udreVariance(int udrei);          // m^2, 0 for the two non-numeric indices (14 not monitored, 15 do not use)
const char* udreText(int udrei);         // "0.23 m" style one-liner ("not monitored", "do not use")
double giveVariance(int givei);          // m^2, 0 for index 15 (not monitored)
double degradationA(int ai);             // fast correction degradation factor, m/s^2 (ai 0..15)

// ------------------------------------------------------------------ what the messages say
struct FastCorr {
    bool valid = false;
    double prcM = 0;                     // pseudorange correction, metres (12 bit field, 0.125 m)
    int udrei = 15;
    int iodf = -1;
    int iodp = -1;
    double timeS = 0;                    // receiver time of the message
};

struct GeoNav {                          // type 9
    bool valid = false;
    int iodn = 0;
    double t0S = 0;                      // time of day of the data, s (16 s steps)
    int ura = 0;
    double pos[3] = {}, vel[3] = {}, acc[3] = {};     // ECEF, m, m/s, m/s^2
    double af0 = 0, af1 = 0;             // s, s/s
};

struct TimeOffset {                      // type 12
    bool valid = false;
    double a1 = 0, a0 = 0;               // s/s, s
    int tUtc = 0, wnT = 0, dtLS = 0, wnLSF = 0, dn = 0, dtLSF = 0;
    int utcId = 0;                       // 0 UTC (standard unknown), 1 UTC(USNO), 2 UTC(SU), 3 UTC(NICT), 4 UTC(BIPM) ... shown as the number
    int tow = 0, wn = 0;
    bool glonass = false;
};

struct AlmEntry {                        // type 17, three a message
    int dataId = 0, prn = 0, health = 0;
    double pos[3] = {}, vel[3] = {};     // m, m/s
};

struct LongTerm {                        // type 25 / 24, one satellite
    int slot = 0;                        // index in the PRN mask (6 bit field, 1 based; 0 = unused)
    int iode = 0;
    double dpos[3] = {};                 // m
    double daf0 = 0;                     // s
    bool velocity = false;
    double dvel[3] = {};                 // m/s
    double daf1 = 0;                     // s/s
    double t0S = 0;
    int iodp = -1;
    double timeS = 0;
};

struct IonoBand {
    bool maskValid = false;
    int iodi = -1;
    std::array<uint8_t, kIgpPerBand + 1> mask{};     // [1..201]
    int nIgp = 0;                        // mask bits set
    std::vector<float> delayM;           // per IGP in mask order; NaN-free: valid[] says which have a value
    std::vector<uint8_t> valid;
    std::vector<uint8_t> givei;
    int blocksSeen = 0;                  // bit b: block b (0..13) of the message type 26 received
};

struct Degradation {                     // type 10, the raw fields (the units are in the document, they are not guessed here)
    bool valid = false;
    int brrc = 0, cltcLsb = 0, cltcV1 = 0, iltcV1 = 0, cltcV0 = 0, iltcV0 = 0, cgeoLsb = 0, cgeoV = 0, igeo = 0, cer = 0;
    int cionoStep = 0, iiono = 0, cionoRamp = 0, rssUdre = 0, rssIono = 0;
};

struct LogEntry {
    double timeS = 0;
    int prn = 0, type = 0;
    std::string text;
};

// Everything one SBAS satellite (one PRN) has sent
struct GeoState {
    int prn = 0;
    uint32_t messages = 0;               // with a good CRC
    uint32_t badCrc = 0;
    uint64_t typesSeen = 0;              // bit t: type t has arrived
    double firstS = -1, lastS = -1;
    // type 1
    bool maskValid = false;
    int iodp = -1;
    std::array<uint8_t, 211> mask{};     // [1..210]
    int nMasked = 0;
    std::vector<int> slotBit;            // slot -> mask bit
    // types 2..5, 24, 6, 7
    std::array<FastCorr, kMaxSlots> fast{};
    int iodf[4] = {-1, -1, -1, -1};      // type 6: IODF of the four groups of 13
    int tLat = -1;                       // type 7 system latency, s
    int iodpDeg = -1;
    std::array<uint8_t, kMaxSlots> aiDeg{};
    bool degValid = false;
    uint32_t fastMessages = 0;
    // others
    GeoNav nav;
    TimeOffset utc;
    std::vector<AlmEntry> almanac;       // the last type 17 message's entries
    double almT0 = 0;
    Degradation deg;
    std::array<IonoBand, kIonoBands> iono{};
    int ionoIodi = -1;
    std::vector<LongTerm> longTerm;      // the latest of each slot
    uint32_t longTermMessages = 0;
    int lastType = -1;
};

struct IonoSummary {
    int band = 0, nIgp = 0, nValue = 0, nNotMonitored = 0;
    float minM = 0, maxM = 0, meanM = 0;
    int worstGivei = 0;
    int blocksSeen = 0;                  // bitmask
    int blocksNeeded = 0;                // blocks the mask needs: ceil(nIgp / 15)
};

struct Summary {
    int prn = 0;
    uint32_t messages = 0;
    int nFast = 0;                       // slots with a fast correction
    int nUsable = 0;                     // of those: UDRE index 0..13
    int nDoNotUse = 0, nNotMonitored = 0;
};

// The decoder: one per receiver. Not thread safe: the owner locks.
class Network {
public:
    // A message with a good CRC (or one the caller has checked); bits are the 250 message bits. timeS is the receiver time in seconds.
    // Returns a one line description of it for the message viewer.
    std::string add(int prn, const uint8_t* bits250, double timeS);
    void countBad(int prn);
    void clear();

    const std::map<int, GeoState>& geos() const { return geos_; }
    const std::vector<LogEntry>& log() const { return log_; }
    uint64_t total() const { return total_; }
    uint64_t totalBad() const { return bad_; }
    uint64_t typeCount(int type) const { return type >= 0 && type < 64 ? perType_[type] : 0; }

    // The PRN a slot of a satellite's PRN mask stands for (0 when the mask is not known or the slot is outside it)
    static int slotPrn(const GeoState& g, int slot);
    static std::string slotName(const GeoState& g, int slot);
    static Summary summarise(const GeoState& g);
    static IonoSummary ionoSummary(const GeoState& g, int band);

private:
    std::map<int, GeoState> geos_;
    std::vector<LogEntry> log_;           // the last 200 messages
    uint64_t total_ = 0, bad_ = 0;
    uint64_t perType_[64] = {};
};

// ------------------------------------------------------------------ the test signal's content
// The 212 data bits of a message of this type with plausible, self consistent content for a made up service (a PRN mask of 14 GPS satellites and the SBAS
// satellite itself, fast corrections that change with time, an ionospheric grid in bands 0 and 1, a GEO navigation message at geostationary radius).
// Types the generator has no content for return false (the caller then fills random bits). prn selects the satellite, m the message number.
bool synthData(int prn, int64_t m, int type, uint8_t* data212);

}  // namespace sbas
}  // namespace dect2
