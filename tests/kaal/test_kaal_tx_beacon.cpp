// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL beacon: frame bits against the Python kaal_beacon_v2.py, CRC failure, the slice against kaal_beacon_slice.py, decoding at
// Eb/N0 6 dB, two frames in one stream, and the occupied bandwidth.
#include "dect2/kaal/kaal_beacon.h"
#include "kaal_golden_tx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2::kaal;
#include "../common/test_util.h"

typedef std::complex<float> cf;

static std::vector<uint8_t> fromHex(const char* h, int nbits) {
    std::vector<uint8_t> b(nbits);
    for (int i = 0; i < nbits; i++) {
        const char c = h[i / 4];
        const int v = c <= '9' ? c - '0' : c - 'a' + 10;
        b[i] = (uint8_t)((v >> (3 - i % 4)) & 1);
    }
    return b;
}
static BeaconFields station() {
    BeaconFields f;
    f.callsign = "LY2ABC"; f.grid = "KO26ab"; f.lat = 54.687; f.lon = 25.279; f.powerDbm = 20; f.linkQuality = 0;
    return f;
}
static const char* kBio = "Hi, I am testing KAAL, a custom digital voice mode. Tell me what you hear.";

struct Gauss {
    std::mt19937 g;
    explicit Gauss(uint32_t seed) : g(seed) {}
    double u() { return ((double)g() + 0.5) / 4294967296.0; }
    cf next() { const double r = std::sqrt(-2.0 * std::log(u())), a = 2 * 3.141592653589793 * u(); return cf((float)(r * std::cos(a)), (float)(r * std::sin(a))); }   // unit variance per component
};

// leading and trailing noise around the frames, AWGN at Eb/N0 (rate 1/4: four coded symbols per information bit)
static std::vector<cf> channel(const std::vector<std::vector<cf>>& frames, double ebn0Db, uint32_t seed, size_t gap = 14400) {
    std::vector<cf> sig(gap, cf(0, 0));
    double ps = 0;
    size_t n = 0;
    for (const auto& fr : frames) {
        for (const cf& s : fr) { ps += std::norm(s); n++; }
        sig.insert(sig.end(), fr.begin(), fr.end());
        sig.insert(sig.end(), gap, cf(0, 0));
    }
    ps /= (double)n;
    const double esn0 = std::pow(10.0, ebn0Db / 10.0) / 4.0;
    const double sd = std::sqrt(ps * 48.0 / esn0 / 2.0);
    Gauss g(seed);
    for (cf& s : sig) { const cf w = g.next(); s += cf((float)(sd * w.real()), (float)(sd * w.imag())); }
    return sig;
}
static std::vector<std::pair<BeaconFields, std::string>> receive(const std::vector<cf>& sig, size_t chunk = 1000) {
    BeaconSliceRx rx;
    rx.configure(48000);
    std::vector<std::pair<BeaconFields, std::string>> got;
    for (size_t i = 0; i < sig.size(); i += chunk) {
        rx.feed(&sig[i], std::min(chunk, sig.size() - i));
        BeaconFields f; std::string t;
        while (rx.poll(f, t)) got.emplace_back(f, t);
    }
    return got;
}

static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * 3.141592653589793 / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

int main() {
    const BeaconFields F = station();
    const std::string bio = kBio;
    CHECK(bio.size() == 74u, "bio length");

    {   // bits against the Python
        const auto full = encodeBeaconBits(F, bio);
        const auto hdr = encodeBeaconBits(F, "");
        CHECK((int)full.size() == G_BEACON_FULL_BITS, "full length %zu", full.size());
        CHECK(full == fromHex(G_BEACON_FULL_HEX, G_BEACON_FULL_BITS), "full beacon bits differ from the Python");
        BeaconFields h = F; h.frameType = 0;
        CHECK(hdr.size() == 170u, "header length %zu", hdr.size());
        CHECK(encodeBeaconBits(h, "") == fromHex(G_BEACON_HDR_HEX, 170), "header bits differ from the Python");
    }
    {   // round trip and corruption
        const auto bits = encodeBeaconBits(F, bio);
        BeaconFields g; std::string t;
        CHECK(decodeBeaconBits(bits, g, t), "clean decode");
        CHECK(g.callsign == "LY2ABC" && g.grid == "KO26ab" && g.version == 1 && g.frameType == 1 && g.linkQuality == 0, "fields %s %s", g.callsign.c_str(), g.grid.c_str());
        CHECK(std::fabs(g.lat - 54.687) < 2e-5 && std::fabs(g.lon - 25.279) < 2e-5 && g.powerDbm == 20.0, "lat %.6f lon %.6f p %.1f", g.lat, g.lon, g.powerDbm);
        CHECK(t == bio, "text '%s'", t.c_str());
        for (size_t pos : {(size_t)3, (size_t)60, (size_t)140, (size_t)155, (size_t)165, (size_t)176, (size_t)400, bits.size() - 1}) {
            auto bad = bits; bad[pos] ^= 1;
            BeaconFields b2; std::string t2;
            CHECK(!decodeBeaconBits(bad, b2, t2), "a flipped bit at %zu was accepted", pos);
        }
        std::vector<uint8_t> shortBits(bits.begin(), bits.begin() + 300);
        BeaconFields b3; std::string t3;
        CHECK(!decodeBeaconBits(shortBits, b3, t3), "truncated text was accepted");
        CHECK(!decodeBeaconBits(std::vector<uint8_t>(100, 0), b3, t3), "too short was accepted");
        const auto hb = encodeBeaconBits(F, "");
        CHECK(decodeBeaconBits(hb, b3, t3) && t3.empty(), "header-only round trip");
        // text longer than 100 is cut; a bad callsign gives no frame
        const auto longBits = encodeBeaconBits(F, std::string(130, 'x'));
        CHECK(decodeBeaconBits(longBits, b3, t3) && t3.size() == 100u, "long text");
        BeaconFields badCall = F; badCall.callsign = "TOOLONGCALL";
        CHECK(encodeBeaconBits(badCall, "").empty(), "long callsign");
    }
    {   // slice against the Python, including the length
        const auto s = beaconSliceModulate(F, bio, 48000);
        CHECK(s.size() == G_SLICE_LEN, "slice length %zu, Python %zu", s.size(), G_SLICE_LEN);
        double worst = 0;
        for (int i = 0; i < 8 && (size_t)i < s.size(); i++)
            worst = std::max(worst, (double)std::max(std::fabs(s[i].real() - G_SLICE_HEAD[i][0]), std::fabs(s[i].imag() - G_SLICE_HEAD[i][1])));
        for (size_t k = 0; k < sizeof G_SLICE_IDX / sizeof G_SLICE_IDX[0]; k++)
            if (G_SLICE_IDX[k] < s.size())
                worst = std::max(worst, (double)std::max(std::fabs(s[G_SLICE_IDX[k]].real() - G_SLICE_LATER[k][0]), std::fabs(s[G_SLICE_IDX[k]].imag() - G_SLICE_LATER[k][1])));
        CHECK(worst < 2e-4, "slice samples differ from the Python by %.6f", worst);
    }
    {   // clean decode, header-only and full, in one stream
        const auto got = receive(channel({beaconSliceModulate(F, "", 48000), beaconSliceModulate(F, bio, 48000)}, 60, 5));
        CHECK(got.size() == 2u, "clean stream: %zu frames", got.size());
        if (got.size() == 2) {
            CHECK(got[0].second.empty() && got[0].first.frameType == 0 && got[0].first.callsign == "LY2ABC", "first frame");
            CHECK(got[1].second == bio && got[1].first.grid == "KO26ab", "second frame");
        }
    }
    {   // Eb/N0 6 dB: the LY2ABC beacon with the 74-character bio
        int ok = 0;
        const std::vector<cf> frame = beaconSliceModulate(F, bio, 48000);
        for (uint32_t seed = 1; seed <= 5; seed++) {
            const auto got = receive(channel({frame}, 6.0, seed));
            bool good = false;
            for (const auto& g : got)
                good |= g.second == bio && g.first.callsign == "LY2ABC" && g.first.grid == "KO26ab" && std::fabs(g.first.lat - 54.687) < 2e-5 &&
                        std::fabs(g.first.lon - 25.279) < 2e-5 && g.first.powerDbm == 20.0;
            ok += good;
            CHECK(got.size() <= 1u, "seed %u: %zu frames from one", seed, got.size());
        }
        printf("full beacon at Eb/N0 6 dB: %d of 5 noise seeds decoded\n", ok);
        CHECK(ok >= 4, "only %d of 5", ok);
        int okh = 0;
        const std::vector<cf> hdr = beaconSliceModulate(F, "", 48000);
        for (uint32_t seed = 11; seed <= 15; seed++) {
            const auto got = receive(channel({hdr}, 6.0, seed));
            okh += got.size() == 1u && got[0].first.callsign == "LY2ABC";
        }
        printf("header beacon at Eb/N0 6 dB: %d of 5\n", okh);
        CHECK(okh >= 4, "only %d of 5 headers", okh);
    }
    {   // noise only gives nothing
        std::vector<cf> n(48000 * 8);
        Gauss g(99);
        for (cf& s : n) s = g.next();
        CHECK(receive(n).empty(), "a beacon from noise");
    }
    {   // occupied bandwidth (99 % of the power) below 1.5 kHz
        const auto s = beaconSliceModulate(F, bio, 48000);
        size_t N = 1; while (N < 4 * s.size()) N <<= 1;
        std::vector<std::complex<double>> a(N, 0.0);
        for (size_t i = 0; i < s.size(); i++) a[i] = std::complex<double>(s[i]) * (0.5 - 0.5 * std::cos(2 * 3.141592653589793 * (double)i / (double)(s.size() - 1)));
        fft(a);
        std::vector<double> p(N);
        double tot = 0;
        for (size_t k = 0; k < N; k++) { p[k] = std::norm(a[k]); tot += p[k]; }
        // cumulative from the lowest frequency (bin N/2 upwards = negative frequencies first)
        double cum = 0, lo = 0, hi = 0;
        bool gotLo = false;
        for (size_t j = 0; j < N; j++) {
            const size_t k = (j + N / 2) % N;
            const double fr = ((double)j - (double)(N / 2)) * 48000.0 / (double)N;
            cum += p[k];
            if (!gotLo && cum >= 0.005 * tot) { lo = fr; gotLo = true; }
            if (cum >= 0.995 * tot) { hi = fr; break; }
        }
        printf("beacon slice 99%% bandwidth %.0f Hz (%.0f .. %.0f)\n", hi - lo, lo, hi);
        CHECK(hi - lo < 1500.0, "bandwidth %.0f Hz", hi - lo);
        CHECK(lo > 3000.0 && hi < 6000.0, "slice at %.0f..%.0f Hz", lo, hi);
    }
    return testSummary("kaal_tx_beacon");
}
