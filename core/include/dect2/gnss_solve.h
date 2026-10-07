// Position and clock solution from pseudoranges: iterative weighted least squares with one clock unknown per system (inter-system bias),
// atmosphere corrections from the elevation of each satellite, and a residual check that drops a bad satellite.
#pragma once
#include "gnss_nav.h"
#include "gnss_tel.h"
#include <vector>

namespace dect2 {

struct GnssObs {
    int sys = 0, prn = 0;
    double pr = 0;               // pseudorange in metres with the satellite clock, relativity and group delay already applied
    double sat[3] = {0, 0, 0};   // ECEF position at the transmit time, in the earth-fixed frame of that instant
    double weight = 1;           // extra weight (1 / variance factor), e.g. from the C/N0; the elevation weight is applied on top
    double iono = 0, tropo = 0;  // filled by the solver (metres) for the report
};

struct GnssSolveOptions {
    bool hasIono = false;
    GpsIono iono;
    double tow = 0;              // GPS time of week of the measurement, for the Klobuchar model
    bool hasPrior = false;
    double prior[3] = {0, 0, 0}; // the previous fix: starts the iteration and gives the elevations for the first atmosphere corrections
    double sigmaNominal = 5.0;   // metres, the pseudorange error that the residual check expects of a zenith satellite
    double maskDeg = 0;          // satellites below this elevation (from the iteration's own position) are dropped
    bool outlierCheck = true;
    bool tropo = true;           // subtract the troposphere model (turn it off for pseudoranges that have none)
};

struct GnssSolution {
    bool ok = false;
    double x[3] = {0, 0, 0};
    double bias[GnssSystems] = {0, 0, 0, 0};     // receiver clock error against each system, metres
    bool sysUsed[GnssSystems] = {false, false, false, false};
    std::vector<char> used;                      // per observation: in the solution
    std::vector<double> residual;                // metres, per observation (0 when not used)
    std::vector<double> elDeg, azDeg;            // per observation
    int nUsed = 0, nUnknowns = 0;
    float hdop = 0, vdop = 0, pdop = 0, tdop = 0;
    float hErrM = 0, vErrM = 0;                  // estimated 1-sigma errors: the weighted geometry times the measured scatter of the residuals
    double sigmaUere = 0;                        // the measured scatter of the residuals (floor 1.5 m), metres
    double rmsResidual = 0;
    int iterations = 0;
    int excluded = 0;                            // satellites dropped by the residual check
};

bool gnssSolve(std::vector<GnssObs>& obs, const GnssSolveOptions& opt, GnssSolution& out);

} // namespace dect2
