// Earth-space links: look angles and slant range (WGS 84 station, satellite given by latitude / longitude / altitude, e.g. a GEO slot),
// the range rate and Doppler of a circular-orbit satellite at a given elevation, and the link budget with the atmosphere of ITU-R
// P.618-14 (rain for p % of the year, scintillation, the section 2.5 sum), P.676-13 Annex 1 slant-path gases and the sky noise of
// P.618-14 section 3 (Tsky = Tmr (1 - 10^(-A/10)) + 2.7 x 10^(-A/10), Tmr = 275 K).
#pragma once

namespace dect2::prop::sat {

struct Look { double rangeKm = 0, elevDeg = 0, azDeg = 0; };
// station: geodetic lat / lon (degrees), height above sea level (km); satellite: geocentric lat / lon, altitude above the 6378.137 km radius
Look look(double stLat, double stLon, double stHkm, double satLat, double satLon, double satAltKm);
double orbitalSpeedKmS(double altKm);                       // circular orbit
double slantRangeKm(double altKm, double elevDeg);          // spherical earth, station at sea level
// line-of-sight speed of a circular-orbit satellite whose ground track passes through the station, at elevation elev (km/s, magnitude):
// v R sin(gamma) / rho, with gamma the earth-centre angle; the largest Doppler of a pass is at the horizon, zero overhead
double rangeRateKmS(double altKm, double elevDeg);
double dopplerHz(double fMHz, double rangeRateKmS);

struct Link {
    double fGHz = 11.7;
    double eirpDbw = 50;            // of the transmitting end (satellite for a downlink, station for an uplink)
    double rxGainDbi = 35;          // receive antenna
    double rxNoiseFigureDb = 1;     // receiver (LNB) noise figure
    double rxFeederDb = 0;
    double antennaM = 0.8;          // station antenna diameter (scintillation)
    double efficiency = 0.6;
    double groundPickupK = 20;      // what the antenna sees of the warm ground
    double stLat = 50, stHkm = 0;   // station latitude and height (rain height, P.618)
    double elevDeg = 30, rangeKm = 38000;
    double rainR001 = 30;           // rain rate exceeded 0.01 % of an average year (mm/h, P.837 map or local data)
    double availabilityPct = 99.9;  // the link must work this % of the year: rain and scintillation exceeded 100 - this %
    double tauDeg = 45;             // polarisation tilt (0 horizontal, 90 vertical, 45 circular)
    double rho0 = 7.5;              // surface water vapour density, g/m3
    double rainHeightKm = -1;       // < 0: from the latitude
    double nWet = 42;               // wet refractivity for scintillation
    double bandwidthHz = 36e6;
    double requiredSnrDb = 5;
};
struct Budget {
    double fsplDb = 0, gasDb = 0, rainDb = 0, scintDb = 0, atmDb = 0, totalLossDb = 0;
    double skyK = 0, tsysK = 0, gtDbK = 0;
    double rxDbw = 0, cn0DbHz = 0, cnDb = 0, marginDb = 0;
    double clearSkyCnDb = 0;        // without rain and scintillation
};
Budget budget(const Link& l);

} // namespace dect2::prop::sat
