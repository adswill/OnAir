// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared pieces of the automatic standard detection (autodet.h): spectrum, percentiles, bringing a signal to baseband.
#pragma once
#include "dect2/autodet/autodet.h"
#include <cmath>
#include <vector>

namespace dect2 {
namespace autodet {
namespace detail {

double dB(double x);
double quantile(std::vector<double> v, double q);

// Welch power spectrum (Hann, 50 % overlap), nfft bins, centred: bin k is at (k - nfft / 2) * rate / nfft. maxSegs limits the work.
std::vector<double> welch(const cf32* x, size_t n, int nfft, int maxSegs = 400);

// The signal at centreHz brought to baseband at about wantRate (the rate actually reached is returned in gotRate); occupiedHz sizes the filter
std::vector<cf32> toBaseband(const cf32* x, size_t n, double rate, double centreHz, double occupiedHz, double wantRate, double& gotRate);

// The occupied part of a spectrum: the strongest run above floor, edges half-way (in dB) between floor and plateau
struct Occupied { bool ok = false; double loHz = 0, hiHz = 0, centreHz = 0, bwHz = 0, floorDb = 0, plateauDb = 0, snrDb = 0; int loBin = 0, hiBin = 0;
                  double edge = 0; };   // edge: (width at 20 % of the dB range - width at 80 %) / width: ~0.01 OFDM, 0.1+ a filtered single carrier
Occupied occupied(const std::vector<double>& psd, double rate, double minSnrDb = 4);

} // namespace detail
} // namespace autodet
} // namespace dect2
