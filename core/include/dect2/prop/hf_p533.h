// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF sky-wave prediction after ITU-R P.533-14 (08/2019), written from the recommendation text:
//  - ionosphere: ITU-R P.1239-4 (08/2023): the CCIR (Oslo 1966) numerical maps of foF2 and M(3000)F2 for each month at R12 = 0 and 100,
//    evaluated with the modified magnetic dip of an epoch-1960 sixth-order field model at 300 km (DGRF 1960 coefficients); foE from
//    section 4; the within-the-month decile factors of foF2 (Tables 2 and 3)
//  - basic MUF of E and F2 modes (P.533 section 3, P.1240-2 eqs. (3)-(6)), operational MUF (P.1240-2 Table 1), E-layer screening (section 4)
//  - mirror heights and elevation angles (section 5.1), up to 3 E and 6 F2 modes up to 9000 km (5.2) with ray-path loss: free space over
//    the virtual slant range, D-layer absorption, above-the-MUF loss, ground reflections, auroral loss (Table 2) and Lz = 8.72 dB
//  - the composite long-path method beyond 7000 km (5.3: fM with the K factor, fL with the night LUF and the sunset decay, Etl, Gap,
//    Ly) and the 7000-9000 km interpolation (5.4)
//  - available power and SNR (sections 6, 7), noise from ITU-R P.372-18 (man-made by environment, galactic above foF2, atmospheric),
//    the decile deviations of signal (WARC HFBC-87 within the hour, ITU-R P.842-5 Table 2 day to day) and noise, and the basic circuit
//    reliability of P.842 / P.533 section 10.1
// Deviation from the text, on purpose: the absorption term uses the closed-form CCIR Report 252 expression (the figures of P.533 for
// AT_noon, phi_n and the exponent p are graphs only) at the two 90 km penetration points of every hop, with the gyrofrequency about the
// longitudinal field there. The CCIR coefficients come from the ccirNN.asc files of the IRI distribution (THIRD_PARTY.md).
#pragma once
#include <vector>

namespace dect2::prop::hf {

// ---- P.1239-4 ionosphere ----
void magField(double latDeg, double lonDeg, double hKm, double& dipDeg, double& totalGauss);   // epoch 1960 field model, degree 6
double modipDeg(double latDeg, double lonDeg);                                          // X = atan(I / sqrt(cos lat)), I at 300 km
double gyroMHz(double latDeg, double lonDeg, double hKm);                               // fH = 2.8 F (gauss)
double foF2(int month, double latDeg, double lonDeg, double utcHour, double r12);       // monthly median, MHz (R12 limited to 160)
double m3000F2(int month, double latDeg, double lonDeg, double utcHour, double r12);
// both for all 24 whole UTC hours at once (the geographic part is computed once)
void foF2Day(int month, double latDeg, double lonDeg, double r12, double foF2Out[24], double m3000Out[24]);
double foE(int month, double latDeg, double lonDeg, double utcHour, double sfi12);      // section 4 (sfi12: 12-month smoothed 10.7 cm flux)
double foF2DecileFactor(bool upper, int month, double latDeg, double localHour, double r12);   // Tables 2 / 3
double solarZenithDeg(double latDeg, double lonDeg, int month, double utcHour);         // P.533 eqs. (34), (35), Table 4
double subsolarLatDeg(int month);                                                        // P.533 Table 4
double geomagneticLatDeg(double latDeg, double lonDeg);                                  // dipole pole 78.5 N, 68.2 W (P.533 Lh)
double r12FromSfi(double sfi);                                                           // inverse of P.371: SFI = 63.7 + 0.728 R + 0.00089 R^2
double sfiFromR12(double r12);

// ---- P.372-18 noise ----
// Fa (dB above kTb) and its upper / lower decile deviations (dB), for an environment 0 quiet rural, 1 rural, 2 residential, 3 city
struct Noise { double faDb = 0, manDb = 0, galDb = -99, atmDb = -99, du = 0, dl = 0; };
Noise noise(double fMHz, int env, double rxLat, double rxLon, int year, int month, int day, double utcHour, double foF2AtRx, bool atmospheric);

// ---- P.533 path ----
struct Path {
    double txLat = 0, txLon = 0, rxLat = 0, rxLon = 0;
    int year = 2026, month = 1, day = 15;
    double utcHour = 12;
    double r12 = 80;            // 12-month smoothed sunspot number
    double sfi12 = -1;          // < 0: from r12
    double fMHz = 14.1;
    double txPowerDbw = 20;     // into the antenna
    double txGainDbi = 0, rxGainDbi = 0;   // toward the horizon / take-off angles (one value; P.533 uses the gain at each mode's angle)
    double bandwidthHz = 3000, requiredSnrDb = 10;
    int noiseEnv = 2;           // 0 quiet rural .. 3 city
    bool atmosphericNoise = true;
    double extraNoiseDb = 0;    // receiver noise above the external noise, added as power (e.g. a poor feeder / receiver)
};

struct Mode {
    char layer = 'F';           // 'E' or 'F'
    int hops = 1;
    double hopKm = 0, elevDeg = 0, mirrorKm = 0, basicMufMHz = 0;
    double absorptionDb = 0, aboveMufDb = 0, groundDb = 0, auroralDb = 0, lossDb = 0;   // ray-path basic loss and its terms
    double fieldDbuv = 0, delayMs = 0;
    bool screened = false;      // F2 mode screened by the E layer (not counted)
};

struct Prediction {
    bool valid = false;
    double distKm = 0;
    int method = 0;             // 0 ray paths (< 7000 km), 1 composite long path (> 9000 km), 2 interpolated
    double basicMufMHz = 0, opMufMHz = 0, owfMHz = 0, hpfMHz = 0;   // path basic MUF, operational MUF (median), MUF exceeded 90 % / 10 % of days
    double mufProb = 0;         // probability that the ionosphere supports the frequency (P.533 3.6)
    double fieldDbuv = -999;    // median sky-wave field strength
    double prDbw = -999;        // median available power from the receive antenna
    Noise n;
    double snrDb = -999, snrLowerDb = -999, snrUpperDb = -999;   // median, exceeded 90 % and 10 % of the time
    double reliability = 0;     // basic circuit reliability 0..1 (SNR >= required)
    double lufLongMHz = 0;      // fL of the long-path method (0 when not used)
    double foF2Mid = 0, foEMid = 0, m3000Mid = 0;
    std::vector<Mode> modes;
    int bestMode = -1;          // strongest unscreened mode
};
Prediction predict(const Path& p);

// hour (UTC 0..23) x frequency map of reliability and SNR, plus the MUF and a median-SNR LUF per hour
struct Chart {
    std::vector<double> freqMHz;
    std::vector<float> rel, snr;            // [hour * nf + i]
    double muf[24] = {0}, owf[24] = {0}, luf[24] = {0};   // luf 0: none that hour
};
Chart chart(Path p, const std::vector<double>& freqMHz);
std::vector<double> hamBandsMHz();          // 160 .. 10 m (1.84, 3.6, 5.35, 7.1, 10.12, 14.1, 18.1, 21.1, 24.9, 28.2)

} // namespace dect2::prop::hf
