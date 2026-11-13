// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT through the transmit IQ-file sink (tx_iqfile.h): the burst is resampled to 1 Msps, written as a raw cf32 file, read back and decoded at that rate.
#include "kait_test_util.h"
#include "dect2/kapp/kapp_frame.h"
#include "dect2/tx/tx_iqfile.h"
#include <cstdio>
#include <filesystem>

using namespace dect2::kait;

int main() {
    const Image img = kaittest::testPicture(960, 540);
    const BurstPlan plan = planBurst(img, "LY1ABC", 0);
    CHECK(plan.ok, "plan");
    const double rate = 1000000.0;
    std::vector<cf> x = dect2::kapp::resample(buildBurst(plan), kInternalRate, rate);
    float pk = 0;
    for (const cf& v : x) pk = std::max(pk, std::max(std::fabs(v.real()), std::fabs(v.imag())));
    for (cf& v : x) v *= 0.8f / pk;   // as the sample tool does: peak at 0.8 of full scale
    const std::string dir = (std::filesystem::temp_directory_path() / "kait_iqfile_test").string();
    std::filesystem::create_directories(dir);
    dect2::IqFileTxConfig c;
    c.format = dect2::IqFileFormat::RawCf32;
    c.path = dect2::iqFilePath(dir + "/", "kait_test", c.format);
    c.description = "kait";
    auto sink = dect2::makeIqFileTxSink(c);
    dect2::TxTune t;
    t.sampleRate = rate;
    std::string err;
    CHECK(sink->open(t, err), "open: %s", err.c_str());
    for (size_t i = 0; i < x.size(); i += 65536) CHECK(sink->write(x.data() + i, std::min<size_t>(65536, x.size() - i), err), "write: %s", err.c_str());
    sink->close();
    // read back
    std::vector<cf> y(x.size());
    FILE* f = fopen(c.path.c_str(), "rb");
    CHECK(f != nullptr, "cannot read %s", c.path.c_str());
    if (f) {
        const size_t n = fread(y.data(), sizeof(cf), y.size(), f);
        fclose(f);
        CHECK(n == y.size(), "read %zu of %zu samples", n, y.size());
    }
    kaittest::Rng rng(9);
    for (cf& v : y) v += cf{(float)(0.03 * rng.gauss()), (float)(0.03 * rng.gauss())};   // a little noise on top
    RxConfig rc;
    rc.sampleRate = rate;
    const RxResult r = decodeBurst(y, rc);
    printf("file: %zu samples at %.0f sps (%.2f s), decoded: header %d, callsign %s, SNR %.1f dB, layers %d of %d, %zu bytes, picture PSNR %.2f dB\n", x.size(), rate, (double)x.size() / rate, r.headerOk,
           r.hdr.callsign.c_str(), r.snrDb, r.layersComplete, r.hdr.nLayers, r.bytesGood, r.image.rgb.empty() ? 0.0 : psnrRgb(img, r.image));
    CHECK(r.headerOk && r.hdr.callsign == "LY1ABC" && r.hdr.width == 960, "header from the file");
    CHECK(r.layersComplete >= 7, "layers %d", r.layersComplete);
    CHECK(!r.image.rgb.empty() && psnrRgb(img, r.image) > 30.0, "picture from the file");
    std::filesystem::remove_all(dir);
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
