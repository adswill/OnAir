// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT as a transmit mode: the generator (TxRequest -> samples at the radio's rate) makes a burst, then silence, then the burst again; a receiver decodes it at that rate.
#include "kait_test_util.h"
#include "dect2/kait/kait_image.h"
#include "dect2/kait/kait_txgen.h"
#include <cstdio>
#include <filesystem>

using namespace dect2;

int main() {
    // refusals
    std::string err;
    TxRequest bad;
    bad.callsign = "TOOLONGCALLSIGN";
    CHECK(!makeKaitTxGenerator(bad, 1e6, err) && !err.empty(), "long callsign accepted");
    bad.callsign = "LY1!";
    CHECK(!makeKaitTxGenerator(bad, 1e6, err), "bad character accepted");
    bad.callsign = "LY1ABC";
    CHECK(!makeKaitTxGenerator(bad, 300000.0, err), "300 ksps accepted");
    bad.variant = 9;
    CHECK(!makeKaitTxGenerator(bad, 1e6, err), "profile 9 accepted");
    bad.variant = 0;
    bad.file = "/nonexistent/picture.png";
    CHECK(!makeKaitTxGenerator(bad, 1e6, err), "missing file accepted");

    // a picture file: write a PPM (800 x 450), send it at 1.2 Msps with the direct profile
    const kait::Image img = kait::syntheticPicture(800, 450);
    const std::string dir = (std::filesystem::temp_directory_path() / "kait_txgen_test").string();
    std::filesystem::create_directories(dir);
    const std::string ppm = dir + "/pic.ppm";
    CHECK(kait::writePpm(ppm, img), "write ppm");
    kait::Image back;
    CHECK(kait::loadPicture(ppm, back, err) && back.w == 800 && back.h == 450 && back.rgb == img.rgb, "ppm round trip");
    const kait::Image big = kait::syntheticPicture(3000, 1500);
    const kait::Image fit = kait::fitPicture(big);
    CHECK(fit.w == 1920 && fit.h == 960, "fit %dx%d", fit.w, fit.h);
    TxRequest r;
    r.callsign = "ly1abc";
    r.variant = 3;
    r.file = ppm;
    const double rate = 1200000.0;
    auto g = makeKaitTxGenerator(r, rate, err);
    CHECK(g != nullptr, "generator: %s", err.c_str());
    if (g) {
        CHECK(std::fabs(g->sampleRate() - rate) < 1, "rate");
        const size_t total = (size_t)(13.0 * rate);   // a burst (10 s), 2 s of silence, the start of the next
        std::vector<cf32> x(total);
        for (size_t i = 0; i < total; i += 65536) g->generate(x.data() + i, std::min<size_t>(65536, total - i));
        float pk = 0;
        for (const cf32& v : x) pk = std::max(pk, std::max(std::fabs(v.real()), std::fabs(v.imag())));
        CHECK(pk > 0.5f && pk <= 0.95f, "peak %.3f", pk);
        double eSil = 0;
        for (size_t i = (size_t)(10.3 * rate); i < (size_t)(11.8 * rate); i++) eSil += std::norm(x[i]);
        CHECK(eSil < 1e-3, "the gap is not silent: %g", eSil);
        double eNext = 0;
        for (size_t i = (size_t)(12.1 * rate); i < (size_t)(12.4 * rate); i++) eNext += std::norm(x[i]);
        CHECK(eNext > 1.0, "the second burst does not start");
        // receive the first burst at 10 dB with noise at the radio's rate
        kaittest::Rng rng(31);
        std::vector<cf32> y(x.begin(), x.begin() + (long)(10.6 * rate));
        double p = 0;
        for (const cf32& v : y) p += std::norm(v);
        p /= (double)y.size();
        const double sd2 = std::sqrt(p * rate / kait::kBandwidthHz / 10.0 / 2.0);
        for (cf32& v : y) v += cf32((float)(sd2 * rng.gauss()), (float)(sd2 * rng.gauss()));
        kait::RxConfig rc;
        rc.sampleRate = rate;
        const kait::RxResult rr = kait::decodeBurst(y, rc);
        printf("generator: header %d, callsign %s, %dx%d, layers %d of %d, %zu bytes, SNR %.1f dB, PSNR %.2f dB\n", rr.headerOk, rr.hdr.callsign.c_str(), rr.hdr.width, rr.hdr.height, rr.layersComplete,
               rr.hdr.nLayers, rr.bytesGood, rr.snrDb, rr.image.rgb.empty() ? 0.0 : kait::psnrRgb(img, rr.image));
        CHECK(rr.headerOk && rr.hdr.callsign == "LY1ABC" && rr.hdr.width == 800 && rr.hdr.profileId == 3, "header");
        CHECK(rr.layersComplete == 4, "layers %d", rr.layersComplete);
        CHECK(!rr.image.rgb.empty() && kait::psnrRgb(img, rr.image) > 40.0, "picture");
    }
    std::filesystem::remove_all(dir);
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
