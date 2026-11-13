// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Position fixing from bearing lines (several stations, or one moving station over time): Stansfield and maximum-likelihood estimators,
// error ellipse, RANSAC outlier rejection, WGS-84 geodesics, and a single-station fix with a range prior.
#pragma once
#include <vector>

namespace dect2::df {

struct BearingLine {
    double lat = 0, lon = 0;      // station position (deg, WGS-84)
    double bearingDeg = 0;        // true bearing to the transmitter
    double sigmaDeg = 3;          // 1-sigma bearing error
};

struct Ellipse { double semiMajorM = 0, semiMinorM = 0, orientationDeg = 0; };   // orientation of the major axis, degrees clockwise from north (0..180)

struct Fix {
    bool ok = false;
    double lat = 0, lon = 0;
    Ellipse e95;                  // 95 % confidence ellipse (chi-square, 2 dof) from the estimator covariance
    double cep50M = 0;            // radius of the circle holding 50 % of the probability (approximation from the ellipse axes)
    double rmsResidualDeg = 0;    // weighted rms bearing residual of the inliers
    std::vector<char> inlier;     // per input line
    int inliers = 0;
};

enum class FixMethod { Stansfield, MaxLikelihood };

// WGS-84 geodesics (Vincenty). Distance in metres, azimuths in degrees clockwise from north.
bool geodInverse(double lat1, double lon1, double lat2, double lon2, double& distM, double& az1Deg, double* az2Deg = nullptr);
void geodDirect(double lat1, double lon1, double az1Deg, double distM, double& lat2, double& lon2);

// All lines used as given (no outlier rejection). Needs at least two non-parallel lines.
Fix fixPosition(const std::vector<BearingLine>& lines, FixMethod method = FixMethod::MaxLikelihood);
// Lines whose bearing disagrees with the consensus by more than max(inlierSigmas * sigma, minGateDeg) are dropped (RANSAC over line pairs).
Fix fixPositionRobust(const std::vector<BearingLine>& lines, FixMethod method = FixMethod::MaxLikelihood, double inlierSigmas = 3.0, double minGateDeg = 6.0);
// One station and a range prior (e.g. a transmitter known to be 20 +- 5 km away): the point on the bearing and an ellipse whose axes are range and cross-range errors.
Fix fixWithRangePrior(const BearingLine& line, double rangeM, double rangeSigmaM);

}   // namespace dect2::df
