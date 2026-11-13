// The atmosphere along a radio path, from the ITU-R texts (written from the recommendations, no ITU code used):
//  - ITU-R P.835-7 (08/2024)  mean annual global reference atmosphere (temperature, pressure, water vapour against height)
//  - ITU-R P.676-13 (08/2022) Annex 1: line-by-line specific attenuation of oxygen and water vapour (44 + 35 lines, Tables 1 and 2),
//                             terrestrial paths (section 2.1) and slant paths through a layered atmosphere with ray bending (2.2.1)
//  - ITU-R P.838-3 (03/2005)  rain specific attenuation k R^alpha, any polarisation tilt and path elevation
//  - ITU-R P.453-14 (08/2019) radio refractivity N and the median effective earth radius factor k50 from the lapse rate dN
//  - ITU-R P.530-19 (09/2025) section 2.4.1: rain attenuation of a terrestrial link exceeded for p % of the time
//  - ITU-R P.618-14 (08/2023) sections 2.2.1.1 (rain on an earth-space path), 2.4.1 (tropospheric scintillation), 2.5 (total)
//  - ITU-R P.839-4 (09/2013)  rain height: the recommendation gives a digital map of the 0 degree isotherm; here a latitude fit of it
//    (documented in docs/PROPAGATION-4.0.md), or a typed value
#pragma once

namespace dect2::prop::itu {

// P.835-7 section 1.1: the mean annual global reference atmosphere. h in km above sea level (geometric), rho0 the surface water vapour
// density (7.5 g/m3 in the standard), scale height 2 km, the mixing ratio floored at 2e-6 as the recommendation says.
struct Atmos { double pHpa = 1013.25, tK = 288.15, rhoGm3 = 7.5; };   // total pressure, temperature, water vapour density
Atmos refAtmosphere(double hKm, double rho0 = 7.5);
inline double vapourPressureHpa(double rhoGm3, double tK) { return rhoGm3 * tK / 216.7; }   // P.676 eq. (4)

// P.676-13 Annex 1, section 1: dB/km. pDry = dry air pressure (hPa), e = water vapour partial pressure (hPa), T in kelvin.
double gammaOxygenDbKm(double fGHz, double pDryHpa, double eHpa, double tK);   // oxygen lines + the dry continuum (eq. 8)
double gammaWaterDbKm(double fGHz, double pDryHpa, double eHpa, double tK);
// total, from the total pressure (hPa), temperature (K) and water vapour density (g/m3)
double gammaGasDbKm(double fGHz, double pTotHpa = 1013.25, double tK = 288.15, double rhoGm3 = 7.5);
// section 2.1: a horizontal (terrestrial) path at one height, A = gamma * d
double terrestrialGasDb(double fGHz, double dKm, double pTotHpa = 1013.25, double tK = 288.15, double rhoGm3 = 7.5);
// section 2.2.1: earth station at hStation km, apparent elevation >= 0 degrees, to the top of the atmosphere (100 km), through the P.835
// reference atmosphere with the surface water vapour density rho0; ray bending from the refractivity of each layer
double slantGasDb(double fGHz, double hStationKm, double elevDeg, double rho0 = 7.5);

// P.838-3: k and alpha for a polarisation tilt tau (degrees from horizontal: 0 horizontal, 90 vertical, 45 circular) and a path
// elevation (0 for terrestrial). gamma = k R^alpha dB/km.
void rainCoefficients(double fGHz, double elevDeg, double tauDeg, double& k, double& alpha);
double rainGammaDbKm(double fGHz, double rainMmH, double elevDeg = 0, double tauDeg = 45);

// P.453-14: N = 77.6 / T (P + 4810 e / T), P total pressure hPa, e water vapour hPa (eq. 1 / 6)
double refractivityN(double pTotHpa, double tK, double eHpa);
// P.453-14 / P.1812-8 eq. (6): median effective earth radius factor k50 = 157 / (157 - dN), dN the lapse rate (N-units/km, positive)
double k50FromDeltaN(double deltaN);
// P.618-14 Step 1 / P.839-4: rain height above sea level (km). The recommendation reads it from a 1.5 degree map of the 0 degree
// isotherm h0, hR = h0 + 0.36 km; here h0 is a smooth latitude fit of that map (about 4.6..5.0 km in the tropics falling to 0 at the poles).
double rainHeightKm(double latDeg);

// P.530-19 section 2.4.1: rain attenuation of a terrestrial link exceeded for p % of an average year (0.001..1 %), from R0.01 (mm/h)
double rainTerrestrialDb(double fGHz, double dKm, double r001MmH, double pPct, double tauDeg = 45);

// P.618-14 section 2.2.1.1: rain attenuation on an earth-space path exceeded for p % of an average year (0.001..5 %).
// hsKm: station height above sea level, hrKm: rain height (< 0: rainHeightKm(lat)), elevation in degrees (>= 5 for best accuracy).
double rainSlantDb(double fGHz, double elevDeg, double latDeg, double hsKm, double r001MmH, double pPct, double tauDeg = 45, double hrKm = -1);
// P.618-14 section 2.4.1: scintillation fade exceeded for p % (0.01..50 %), antenna diameter (m) and efficiency, wet refractivity Nwet
// (P.453 map; about 20 dry, 40-60 mid-latitude summer, 100+ humid tropics), elevation >= 5 degrees.
double scintillationDb(double fGHz, double elevDeg, double antennaM, double pPct, double nWet = 42, double efficiency = 0.5);
// P.618-14 section 2.5: A = Agas + sqrt((Arain + Acloud)^2 + Ascint^2)
double totalEarthSpaceDb(double gasDb, double rainDb, double cloudDb, double scintDb);

// P.1812-8 Attachment 2 / P.1057: the inverse complementary cumulative normal distribution I(x), 0 < x < 1 (I(0.1) = 1.2816)
double invQ(double x);

} // namespace dect2::prop::itu
