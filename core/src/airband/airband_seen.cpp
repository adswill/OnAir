// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band watch (see airband_seen.h).
#include "dect2/airband/airband_seen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/ring.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace dect2 {

namespace {
constexpr double kStep833 = 25000.0 / 3;
constexpr double kAvgSec = 0.02;       // spectra averaged before they enter the peak hold
constexpr double kHoldHalfSec = 6.0;
constexpr double kExcessDb = 11, kSideDb = 9;
constexpr double kSnapHz = 450;
constexpr size_t kMaxSeen = 96;
}   // namespace

struct AirbandBandWatch::Impl {
    double rate = 0, center = 0, bin = 0;
    int N = 0, M = 1, acc = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    double winPow = 1;
    std::vector<cf32> buf;
    int fill = 0;
    std::vector<float> sum, hold, cur;   // cur: the latest averaged spectrum (is it there now?)
    double decay = 1;
    std::map<long, AirbandSeen> map;
    std::vector<AirbandSeen> out;
    double lastShift = 0;

    void configure(double r, double c) {
        rate = r; center = c; map.clear(); out.clear();
        if (r < 200e3) { N = 0; return; }
        N = 512;
        while (N < 16384 && r / N > 600) N *= 2;
        bin = r / N;
        fft = std::make_unique<Fft>(N);
        win.resize((size_t)N);
        winPow = 0;
        for (int i = 0; i < N; i++) { win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * dect2::math::kPi * i / N)); winPow += (double)win[(size_t)i] * win[(size_t)i]; }
        M = std::max(1, (int)std::lround(kAvgSec * r / N));
        decay = std::pow(0.5, M * (double)N / r / kHoldHalfSec);
        buf.assign((size_t)N, cf32());
        fill = acc = 0;
        sum.assign((size_t)N, 0.f); hold.assign((size_t)N, 0.f); cur.assign((size_t)N, 0.f);
    }

    void block() {
        for (int i = 0; i < N; i++) buf[(size_t)i] *= win[(size_t)i];
        fft->forward(buf.data());
        for (int k = 0; k < N; k++) sum[(size_t)k] += (float)(std::norm(buf[(size_t)k]) / winPow);
        if (++acc >= M) {
            for (int k = 0; k < N; k++) {
                const float v = sum[(size_t)k] / (float)M;
                cur[(size_t)k] = v;
                hold[(size_t)k] = std::max(v, hold[(size_t)k] * (float)decay);
                sum[(size_t)k] = 0;
            }
            acc = 0;
        }
    }

    void update(double now, double tuneErr, bool tuneKnown, const std::vector<double>& listed) {
        if (N <= 0) return;
        // local floor: the median of the held spectrum in blocks of about 32 kHz
        const int Bk = std::max(16, (int)(32000 / bin));
        // work on the shifted layout: index i = 0 .. N-1 is the offset (i - N/2) * bin
        std::vector<float> h((size_t)N);
        for (int i = 0; i < N; i++) h[(size_t)i] = hold[(size_t)((i + N / 2) % N)];
        std::vector<float> floorv((size_t)N, 0.f);
        std::vector<float> tmp;
        for (int s = 0; s < N; s += Bk) {
            const int e = std::min(N, s + Bk);
            tmp.assign(h.begin() + s, h.begin() + e);
            std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
            const float med = std::max(tmp[tmp.size() / 2], 1e-20f);
            for (int i = s; i < e; i++) floorv[(size_t)i] = med;
        }
        const int lo = (int)(0.05 * N), hi = N - lo;
        const int sA = std::max(3, (int)std::ceil(4500 / bin)), sB = std::max(sA + 2, (int)std::ceil(9000 / bin));
        const int dcSkip = std::max(2, (int)std::ceil(2500 / bin));   // the radio's own spike
        struct Peak { double off; double excess; bool now; };
        std::vector<Peak> pk;
        const double thrE = std::pow(10.0, kExcessDb / 10), thrS = std::pow(10.0, kSideDb / 10);
        for (int i = lo + sB; i < hi - sB; i++) {
            if (std::abs(i - N / 2) <= dcSkip) continue;
            const float v = h[(size_t)i];
            if (v < floorv[(size_t)i] * thrE) continue;
            if (v < h[(size_t)i - 1] || v < h[(size_t)i + 1] || v < h[(size_t)i - 2] || v < h[(size_t)i + 2]) continue;
            double l = 0, r = 0;
            int nl = 0, nr = 0;
            for (int d = sA; d <= sB; d++) { l += h[(size_t)(i - d)]; r += h[(size_t)(i + d)]; nl++; nr++; }
            const double side = std::min(l / nl, r / nr);
            if (v < side * thrS) continue;
            // parabolic interpolation on the dB values
            const double y0 = std::log(h[(size_t)i - 1] + 1e-30), y1 = std::log((double)v + 1e-30), y2 = std::log(h[(size_t)i + 1] + 1e-30);
            const double dn = y0 - 2 * y1 + y2;
            const double fr = (i - N / 2) + (std::fabs(dn) > 1e-9 ? 0.5 * (y0 - y2) / dn : 0.0);
            double c = 0;   // the latest spectrum around the line
            for (int d = -2; d <= 2; d++) c = std::max(c, (double)cur[(size_t)((i + d + N / 2) % N)]);
            pk.push_back({fr * bin, 10 * std::log10((double)v / floorv[(size_t)i]), c > floorv[(size_t)i] * std::pow(10.0, 0.8)});
        }
        std::sort(pk.begin(), pk.end(), [](const Peak& a, const Peak& b) { return a.excess > b.excess; });
        if (pk.size() > 40) pk.resize(40);
        // the tuning error: the receiver's, else the shift that puts most lines on the 25 kHz raster
        double shift = tuneKnown ? tuneErr : 0;
        bool approx = !tuneKnown;
        if (!tuneKnown && pk.size() >= 2) {
            int best = 0;
            double bestS = 0;
            for (double s = -12000; s <= 12000; s += 100) {
                int n = 0;
                for (const auto& p : pk) {
                    const double f = center + p.off - s, r = f / 25000.0;
                    if (std::fabs(r - std::round(r)) * 25000.0 < 350) n++;
                }
                if (n > best || (n == best && std::fabs(s) < std::fabs(bestS))) { best = n; bestS = s; }
            }
            if (best >= 2 && best >= (int)(0.6 * pk.size())) { shift = bestS; approx = false; }
            else shift = lastShift;
        } else if (!tuneKnown) shift = lastShift;
        if (!tuneKnown && !approx) lastShift = shift;
        for (auto& kv : map) kv.second.activeNow = false;
        for (const auto& p : pk) {
            const double f = center + p.off - shift;
            // VHF: the 8.33 kHz raster from 118 MHz; military UHF (225 - 400 MHz): the 25 kHz raster only
            const bool uhf = f > 224.9e6 && f < 400.1e6;
            const double step = uhf ? 25000.0 : kStep833, base = uhf ? 225e6 : 118e6;
            const double k = std::round((f - base) / step);
            const double fs = base + k * step;
            if (std::fabs(f - fs) > kSnapHz) continue;
            if (uhf ? (fs < 225e6 - 1 || fs > 399.975e6 + 1) : (fs < 117.9e6 || fs > 137.1e6)) continue;
            bool listedAlready = false;
            for (double l : listed) if (std::fabs(l - fs) < 1200) listedAlready = true;
            if (listedAlready) continue;
            const long key = std::lround(fs / 100);   // 100 Hz units: unique on both rasters
            const bool isNew = map.find(key) == map.end();
            AirbandSeen& s = map[key];
            s.freqHz = fs;
            const double r25 = fs / 25000.0;
            s.is833 = std::fabs(r25 - std::round(r25)) * 25000.0 > 10;
            s.name = airbandName(fs, s.is833);
            s.excessDb = std::max(s.excessDb, (float)p.excess);
            if (p.now || isNew) { s.hits++; s.lastSec = now; }
            s.activeNow = p.now;
            s.approx = approx;
        }
        // listed ones that were seen before the user added them go
        for (auto it = map.begin(); it != map.end();) {
            bool l = false;
            for (double f : listed) if (std::fabs(f - it->second.freqHz) < 1200) l = true;
            if (l) it = map.erase(it); else ++it;
        }
        out.clear();
        for (const auto& kv : map) out.push_back(kv.second);
        std::sort(out.begin(), out.end(), [](const AirbandSeen& a, const AirbandSeen& b) { return a.hits != b.hits ? a.hits > b.hits : a.freqHz < b.freqHz; });
        if (out.size() > kMaxSeen) out.resize(kMaxSeen);
    }
};

AirbandBandWatch::AirbandBandWatch() : p_(std::make_unique<Impl>()) {}
AirbandBandWatch::~AirbandBandWatch() = default;
void AirbandBandWatch::configure(double r, double c) { p_->configure(r, c); }
void AirbandBandWatch::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.N <= 0) return;
    size_t i = 0;
    while (i < n) {
        const size_t take = std::min((size_t)(m.N - m.fill), n - i);
        std::copy(x + i, x + i + take, m.buf.begin() + m.fill);
        m.fill += (int)take; i += take;
        if (m.fill == m.N) { m.block(); m.fill = 0; }
    }
}
void AirbandBandWatch::update(double now, double e, bool k, const std::vector<double>& l) { p_->update(now, e, k, l); }
const std::vector<AirbandSeen>& AirbandBandWatch::seen() const { return p_->out; }

} // namespace dect2
