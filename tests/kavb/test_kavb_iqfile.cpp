// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB transmit-to-file: a profile A burst of synthetic video / audio packets (FEC 14 + 2) is modulated to baseband IQ, written as a little-endian complex float32
// file (2.8 Msps = 4 x 700 ksym/s) with a small JSON description next to it, read back, passed through a noisy channel and decoded; the packets must come back
// identical. KAVB_IQ_OUT=<path.cf32> keeps the file at that path (default: a temporary file that is deleted).
#include "kavb_test_util.h"
#include "dect2/kavb/kavb_av.h"
#include <cstdlib>
#include <string>
using namespace dect2::kavb;
using namespace kavbtest;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    const Profile p = profileOf(0, false);
    const int fec = 1;
    Packetiser pk(p, fec);
    SyntheticAv src(1.2e6, 30.0, 96000.0, 1200);
    std::vector<AvPacket> sent;
    AvPacket a;
    while (src.timeSec() < 0.25) {
        src.next(a);
        sent.push_back(a);
        pk.push(a);
    }
    auto frames = pk.takeFrames(true);
    Transmitter tx(0);
    const auto iq = tx.modulate(frames, fec, 0, 0, 0);
    CHECK(!iq.empty(), "no samples");

    std::string path = std::getenv("KAVB_IQ_OUT") ? std::getenv("KAVB_IQ_OUT") : "";
    const bool keep = !path.empty();
    if (!keep) path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/kavb_profile_a_test.cf32";
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr, "cannot open %s", path.c_str());
    if (!f) return 1;
    std::fwrite(iq.data(), sizeof(cf), iq.size(), f);
    std::fclose(f);
    {
        FILE* j = std::fopen((path + ".json").c_str(), "w");
        if (j) {
            std::fprintf(j, "{\"format\":\"cf32_le\",\"sample_rate\":%.0f,\"symbol_rate\":%.0f,\"profile\":\"A\",\"modulation\":\"16QAM rate 3/4\",\"frames\":%zu,\"fec_profile\":%d,\"samples\":%zu}\n",
                         p.symRate * kSps, p.symRate, frames.size(), fec, iq.size());
            std::fclose(j);
        }
    }

    // read back
    std::vector<cf> back(iq.size());
    f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr, "cannot reopen");
    if (!f) return 1;
    const size_t got = std::fread(back.data(), sizeof(cf), back.size(), f);
    std::fclose(f);
    CHECK(got == iq.size(), "read %zu of %zu samples", got, iq.size());
    CHECK(back == iq, "file contents differ");

    Rng rng(4242);
    Channel ch;
    ch.snrDb = 14.0;
    ch.cfoCycPerSym = -0.006;
    ch.delaySamples = 0.6;
    ch.phase = 2.0;
    const auto rxIq = applyChannel(back, ch, rng, 2000);
    Receiver rx;
    Depacketiser dp(p);
    int frOk = 0;
    const auto fr = rx.push(rxIq.data(), rxIq.size());
    for (const auto& x : fr) { frOk += x.allOk; dp.pushFrame(x); }
    dp.flush();
    const auto pkts = dp.takePackets();
    CHECK(fr.size() == frames.size(), "frames %zu of %zu", fr.size(), frames.size());
    CHECK(pkts.size() == sent.size(), "packets %zu of %zu", pkts.size(), sent.size());
    for (size_t i = 0; i < pkts.size() && i < sent.size(); i++) CHECK(pkts[i].payload == sent[i].payload && pkts[i].seq == sent[i].seq, "packet %zu differs", i);
    printf("KAVB_IQFILE %s: %zu samples (%.3f s at %.1f Msps), %zu frames, %zu packets, frames ok %d, file kept: %s\n", path.c_str(), iq.size(), iq.size() / (p.symRate * kSps),
           p.symRate * kSps / 1e6, frames.size(), sent.size(), frOk, keep ? "yes" : "no");
    if (!keep) { std::remove(path.c_str()); std::remove((path + ".json").c_str()); }
    if (g_fail) { printf("test_kavb_iqfile: %d FAILED\n", g_fail); return 1; }
    printf("test_kavb_iqfile: ok\n");
    return 0;
}
