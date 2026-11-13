// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GLONASS L1OF navigation message and orbit (GLONASS ICD L1, L2, edition 5.1).
// Strings of 2 s: 85 bits at 50 bps in relative code, each bit sent as two 10 ms halves under the 100 Hz meander (bi-binary), then the 30 chip
// time mark (0.3 s). Bit 85 (sent first) is the idle 0, bits 84..81 the string number, bits 8..1 the Hamming check bits (ICD 4.3.3, 4.7).
// Frames of 15 strings (30 s), superframes of 5 frames (2.5 min); the day starts with a superframe.
// Strings 1..4 carry the satellite's own data (Table 4.6): position, velocity and luni-solar acceleration in PZ-90 at tb, its clock (tau_n, gamma_n),
// day number NT; string 5 the GLONASS time corrections (tau_c to UTC(SU), tau_GPS to GPS time) and the four-year interval N4 (Table 4.11).
// The orbit is integrated from tb with the equations of motion of ICD A.3.1.2 by 4th order Runge-Kutta.
#pragma once
#include <cstdint>

namespace dect2 {

constexpr char kGloTimeMark[31] = "111110001101110101000010010110";   // ICD 3.3.2.2
constexpr double kGloMu = 398600.44e9;            // ICD A.3.1.2: m^3/s^2
constexpr double kGloAe = 6378136.0;              // m
constexpr double kGloJ2 = 1082625.7e-9;
constexpr double kGloOmega = 7.292115e-5;         // rad/s
constexpr int kGloDaysTo1996 = 5839;              // 1980-01-06 (GPS time 0) to 1996-01-01 (the first four-year interval of N4)

// A string: s[i] is bit i (1..85) of the string; s[0] unused
// The Hamming check bits beta1..beta8 of a string whose data bits 9..85 are set (for the generator)
void gloSetHamming(uint8_t* s);
// Check (and correct a single error in) a received string: 0 good, 1 corrected, -1 erased (ICD 4.7 rules a..c)
int gloCheckString(uint8_t* s);
uint32_t gloField(const uint8_t* s, int hi, int len);       // the field whose most significant bit is bit `hi`, `len` bits towards bit 1
double gloSignMag(uint32_t v, int len);                     // sign and magnitude (MSB 1 = negative), ICD 4.4
void gloPut(uint8_t* s, int hi, int len, uint32_t v);

struct GloEphemeris {
    int n = 0;                   // slot number (string 4)
    int k = 0;                   // frequency channel (from the band it came in)
    double tk = 0;               // frame start, seconds of the day (string 1)
    double tb = 0;               // reference time, seconds of the GLONASS day (string 2: index x 15 min)
    int tbIndex = 0;
    double pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, acc[3] = {0, 0, 0};   // m, m/s, m/s^2, PZ-90
    double tauN = 0, gammaN = 0, dTauN = 0;
    int Bn = 0, P1 = 0, P2 = 0, P3 = 0, P4 = 0, P = 0, ln = 0, FT = 0, NT = 0, M = 0, En = 0;
    // string 5
    bool has5 = false;
    int NA = 0, N4 = 0;
    double tauC = 0, tauGps = 0;
    bool healthy() const { return (Bn & 4) == 0 && ln == 0; }
};

// What a satellite has sent so far: strings 1..4 of one frame make an ephemeris
struct GloNav {
    GloEphemeris pend;
    int64_t frameOf[5] = {-1, -1, -1, -1, -1};
    int parts() const;           // strings 1..4 of the frame in progress (0..4)
};
// Fold a string (checked) into the collection. frameId: the frame the string belongs to (counted from any origin, the same for the 15 strings of a
// frame). Returns true when strings 1..4 of one frame are complete (in *eph, with the latest string 5 data).
bool gloHandleString(GloNav& nav, const uint8_t* s, int64_t frameId, GloEphemeris* eph);

// Satellite position (m, PZ-90 at time t) at GLONASS time t (seconds of the day, the same day as tb or near it) and the satellite clock: its own time
// minus GLONASS time (-tau_n + gamma_n (t - tb)). Integrated from tb by RK4 in steps of at most `step` seconds.
void gloEphemerisState(const GloEphemeris& e, double t, double pos[3], double vel[3], double* clockS, double step = 30.0);
// The difference of two times of day wrapped to +-12 h
double gloWrapDay(double dt);

} // namespace dect2
