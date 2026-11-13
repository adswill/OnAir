// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT physical layer: framing, header, one full burst through an AWGN channel with carrier and clock offsets, the layered picture.
// KRIS_MEASURE=1 in the environment adds the SNR sweep (+12 .. -12 dB) of the whole chain and prints KAIT_MEASURE lines (slow: about a minute).
#include "kait_test_util.h"
#include <chrono>
#include <cstdlib>

using namespace dect2::kait;

static const Image& pic() {
    static const Image im = kaittest::testPicture();
    return im;
}

int main() {
    // profile tables fit the burst
    for (const Profile& p : kProfiles) {
        const std::vector<LayerSpec> ls = planLayers(p.id);
        CHECK(!ls.empty(), "profile %d has no layers", p.id);
        Header h;
        h.nLayers = (int)ls.size();
        for (size_t i = 0; i < ls.size(); i++) {
            h.layers[i] = ls[i];
            CHECK(infoBitsPerBlock(ls[i].rate) > 16, "profile %d layer %zu code", p.id, i);
        }
        printf("profile %d %-8s: %d layers, %zu bytes, %ld slots, %.3f s\n", p.id, p.name, h.nLayers, h.streamBytes(), h.slots(), (double)h.burstSamples() / kInternalRate);
        CHECK(h.burstSamples() <= (long)(kBurstSeconds * kInternalRate), "profile %d burst %.3f s", p.id, (double)h.burstSamples() / kInternalRate);
        CHECK(h.burstSamples() > (long)(9.9 * kInternalRate), "profile %d burst is short: %.3f s", p.id, (double)h.burstSamples() / kInternalRate);
    }
    // header round trip
    {
        Header h;
        h.callsign = "LY1ABC/P";
        h.width = 1920; h.height = 1080; h.levels = 5; h.topPlane = 11; h.profileId = 0; h.decisions = 0xDEADBEEFu;
        const std::vector<LayerSpec> ls = planLayers(0);
        h.nLayers = (int)ls.size();
        for (size_t i = 0; i < ls.size(); i++) h.layers[i] = ls[i];
        std::vector<uint8_t> b = headerInfoBits(h);
        CHECK((int)b.size() == kHeaderInfoBits, "header bits %zu", b.size());
        Header g;
        CHECK(parseHeaderBits(b.data(), g), "header parse");
        CHECK(g.callsign == "LY1ABC/P" && g.width == 1920 && g.height == 1080 && g.topPlane == 11 && g.decisions == 0xDEADBEEFu && g.nLayers == h.nLayers, "header fields");
        for (int i = 0; i < g.nLayers; i++) CHECK(g.layers[(size_t)i].blocks == h.layers[(size_t)i].blocks && g.layers[(size_t)i].mod == h.layers[(size_t)i].mod && g.layers[(size_t)i].rep == h.layers[(size_t)i].rep, "layer %d", i);
        b[40] ^= 1;
        CHECK(!parseHeaderBits(b.data(), g), "corrupted header accepted");
    }
    // one burst, +8 dB, carrier offset 3.1 kHz, clock +12 ppm, 0.7 s of lead
    const BurstPlan plan = planBurst(pic(), "LY1ABC", 0);
    CHECK(plan.ok, "plan");
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<cf> burst = buildBurst(plan);
    double pw = 0;
    for (const cf& v : burst) pw += std::norm(v);
    pw /= (double)burst.size();
    printf("burst: %zu samples = %.3f s, stream %zu bytes, mean power %.3f, build %.2f s\n", burst.size(), (double)burst.size() / kInternalRate, plan.stream.size(), pw,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    CHECK(burst.size() == (size_t)plan.hdr.burstSamples(), "burst length");
    CHECK(pw > 0.85 && pw < 1.15, "burst power %.3f", pw);
    {
        kaittest::Rng rng(5);
        const std::vector<cf> rx = kaittest::channel(burst, 8.0, 3100.0, 12.0, 442000, 300000, rng);
        const auto t1 = std::chrono::steady_clock::now();
        const RxResult r = decodeBurst(rx);
        printf("+8 dB: synced %d header %d endsweep %d, SNR %.2f dB (sent 8), CFO %.1f Hz (sent 3100), clock %.2f ppm (sent 12), layers %d of %d, %zu of %zu bytes, %.2f s\n", r.synced,
               r.headerOk, r.endSweep, r.snrDb, r.cfoHz, r.clockPpm, r.layersComplete, plan.hdr.nLayers, r.bytesGood, plan.stream.size(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
        CHECK(r.synced && r.headerOk && r.endSweep, "sync");
        CHECK(std::fabs(r.snrDb - 8.0) < 1.0, "SNR estimate %.2f", r.snrDb);
        CHECK(std::fabs(r.cfoHz - 3100.0) < 20.0, "CFO %.1f", r.cfoHz);
        CHECK(std::fabs(r.clockPpm - 12.0) < 1.0, "clock %.2f", r.clockPpm);
        CHECK(r.hdr.callsign == "LY1ABC" && r.hdr.width == 1920, "header contents");
        CHECK(r.layersComplete >= 7, "only %d layers at +8 dB", r.layersComplete);
        if (!r.image.rgb.empty()) printf("   picture PSNR %.2f dB\n", psnrRgb(pic(), r.image));
        CHECK(!r.image.rgb.empty() && psnrRgb(pic(), r.image) > 36.0, "picture at +8 dB");
    }
    // real-world impairments: carrier +-9.7 kHz, clock +-18 ppm, the burst starting at sample 0, weak signal (-10 dB: the first layers), the other profiles
    struct Case { const char* name; int profile; double snr, cfo, ppm; size_t lead; int minLayers; double minPsnr; };
    const Case cases[] = {
        {"+8 dB, CFO -9.7 kHz, clock -18 ppm, no lead", 0, 8.0, -9700.0, -18.0, 0, 7, 36.0},
        {"+8 dB, CFO +9.7 kHz, clock +18 ppm", 0, 8.0, 9700.0, 18.0, 123457, 7, 36.0},
        {"+8 dB, CFO +2 kHz, clock +50 ppm (header slips 4 samples)", 0, 8.0, 2000.0, 50.0, 20000, 7, 36.0},
        {"+8 dB, CFO -2 kHz, clock -50 ppm", 0, 8.0, -2000.0, -50.0, 20000, 7, 36.0},
        {"-8 dB, CFO +4 kHz", 0, -8.0, 4000.0, 7.0, 31000, 2, 19.0},
        {"-12 dB, CFO -1.2 kHz", 0, -12.0, -1200.0, -3.0, 5000, 1, 17.0},
        {"robust profile, +3 dB", 1, 3.0, 800.0, 2.0, 40000, 5, 25.0},
        {"quality profile, +26 dB", 2, 26.0, -2500.0, 4.0, 40000, 8, 40.0},
        {"direct profile, +8 dB, CFO 6 kHz", 3, 8.0, 6000.0, -9.0, 77000, 4, 42.0},
    };
    for (const Case& cs : cases) {
        const BurstPlan pl = cs.profile == 0 ? plan : planBurst(pic(), "DL2XYZ/P", cs.profile);
        const std::vector<cf> bu = cs.profile == 0 ? burst : buildBurst(pl);
        kaittest::Rng rng(300 + (uint64_t)cs.profile * 7 + (uint64_t)std::fabs(cs.snr));
        const std::vector<cf> rx = kaittest::channel(bu, cs.snr, cs.cfo, cs.ppm, cs.lead, 200000, rng);
        const RxResult r = decodeBurst(rx);
        const double ps = r.image.rgb.empty() ? 0.0 : psnrRgb(pic(), r.image);
        printf("%-48s: header %d, SNR %.1f, CFO %.0f, clock %.1f ppm, layers %d of %d, %zu bytes, PSNR %.2f\n", cs.name, r.headerOk, r.snrDb, r.cfoHz, r.clockPpm, r.layersComplete, r.hdr.nLayers, r.bytesGood, ps);
        CHECK(r.headerOk, "%s: no header", cs.name);
        CHECK(r.layersComplete >= cs.minLayers, "%s: %d layers < %d", cs.name, r.layersComplete, cs.minLayers);
        CHECK(ps >= cs.minPsnr, "%s: PSNR %.2f < %.1f", cs.name, ps, cs.minPsnr);
    }
    // noise only: nothing is found
    {
        kaittest::Rng rng(6);
        std::vector<cf> n(2000000);
        for (cf& v : n) v = cf{(float)rng.gauss(), (float)rng.gauss()};
        const RxResult r = decodeBurst(n);
        CHECK(!r.synced, "false sync on noise");
    }
    if (std::getenv("KRIS_MEASURE") && std::string(std::getenv("KRIS_MEASURE")) == "1") {
        for (double snr : {14.0, 12.0, 11.0, 10.0, 9.0, 8.0, 7.0, 6.0, 5.0, 4.0, 2.0, 0.0, -2.0, -4.0, -6.0, -7.0, -8.0, -9.0, -10.0, -11.0, -12.0}) {
            kaittest::Rng rng(100 + (uint64_t)(snr + 20));
            const std::vector<cf> rx = kaittest::channel(burst, snr, 1500.0, 5.0, 100000, 100000, rng);
            const RxResult r = decodeBurst(rx);
            std::string lay;
            for (const LayerResult& l : r.layers) lay += std::to_string(l.blocksOk) + "/" + std::to_string(l.blocks) + " ";
            printf("KAIT_MEASURE snr=%.0f synced=%d header=%d est=%.1f layersComplete=%d bytes=%zu psnr=%.2f blocks: %s\n", snr, r.synced, r.headerOk, r.snrDb, r.layersComplete, r.bytesGood,
                   r.image.rgb.empty() ? 0.0 : psnrRgb(pic(), r.image), lay.c_str());
            fflush(stdout);
        }
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
