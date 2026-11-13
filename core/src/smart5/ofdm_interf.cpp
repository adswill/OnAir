// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OFDM interferer locator (see ofdm_interf.h).
#include "dect2/smart5/ofdm_interf.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

const char* interfName(InterfKind k) {
    switch (k) {
    case InterfKind::CwSpur: return "CW spur";
    case InterfKind::LteBlock: return "LTE / 5G block";
    case InterfKind::AdjacentChannel: return "adjacent channel";
    default: return "wideband interference";
    }
}

std::vector<Interferer> locateInterferers(const std::vector<float>& snr, double f0, double df, OfdmInterfParams p) {
    std::vector<Interferer> out;
    const int n = (int)snr.size();
    if (n < 16 || df <= 0) return out;
    std::vector<float> s(snr);
    // the reference: the 75th percentile, so a block covering half the channel does not drag it down
    std::nth_element(s.begin(), s.begin() + n * 3 / 4, s.end());
    const double med = s[n * 3 / 4];
    const double thr = med - p.dropDb;
    int k = 0;
    while (k < n) {
        if (snr[k] >= thr) { k++; continue; }
        int a = k, b = k, gap = 0;
        for (int j = k + 1; j < n; j++) {
            if (snr[j] < thr) { b = j; gap = 0; }
            else if (++gap > p.maxGap) break;
        }
        double sum = 0;
        for (int j = a; j <= b; j++) sum += snr[j];
        Interferer it;
        it.firstCarrier = a; it.lastCarrier = b;
        it.widthHz = (b - a + 1) * df;
        it.centreHz = f0 + 0.5 * (a + b) * df;
        it.depthDb = med - sum / (b - a + 1);
        const int edge = std::max(1, (int)std::lround(p.edgeFrac * n));
        const int nc = b - a + 1;
        if (nc <= p.cwMaxCarriers) it.kind = InterfKind::CwSpur;
        else if (a < edge || b >= n - edge) it.kind = InterfKind::AdjacentChannel;
        else {
            // LTE occupied bandwidths (resource blocks x 180 kHz) and NR channel widths, within 12 %
            static const double lte[] = {1.08e6, 2.7e6, 4.5e6, 9e6, 13.5e6, 18e6, 4.6e6, 9.4e6, 14.2e6, 19.1e6};
            it.kind = InterfKind::Wideband;
            for (double w : lte) if (std::fabs(it.widthHz - w) < 0.12 * w) { it.kind = InterfKind::LteBlock; break; }
        }
        out.push_back(it);
        k = b + 1;
    }
    std::sort(out.begin(), out.end(), [](const Interferer& x, const Interferer& y) { return x.depthDb * x.widthHz > y.depthDb * y.widthHz; });
    return out;
}

std::string interfText(const Interferer& i) {
    char b[200];
    if (i.widthHz < 1e6) snprintf(b, sizeof b, "%s at %.4f MHz, %.0f kHz wide, %.1f dB down", interfName(i.kind), i.centreHz / 1e6, i.widthHz / 1e3, i.depthDb);
    else snprintf(b, sizeof b, "%s at %.3f MHz, %.2f MHz wide, %.1f dB down", interfName(i.kind), i.centreHz / 1e6, i.widthHz / 1e6, i.depthDb);
    return b;
}

} // namespace smart5
} // namespace dect2
