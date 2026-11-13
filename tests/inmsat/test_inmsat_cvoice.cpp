// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat all-carriers mode, voice: two Aero C (voice) channels in ONE wide capture (2.4 Msps, noise, carrier offsets), found by the carrier
// detector, demodulated by the manager's C-channel receivers, and their coded voice frames handed to the mixer with the call details (aircraft
// address, ground station). A fake decoder stands in for the user-installed AMBE codec. Synthetic signals made from the framing that was checked
// on a real recording (test_aero_cchan_real); no real call has been played through a real codec in a test.
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_cgen.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
using namespace dect2;
using namespace dect2::inmsat;
#include "../common/test_util.h"

namespace {
struct FakeCodec : codec::SpeechCodec {
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override {
        if (coded.size() != 12) return {};
        return std::vector<int16_t>(160, (int16_t)((coded[0] % 100) * 50 + 100));
    }
};
}

int main() {
    const double rate = 2.4e6;
    AeroCGenConfig g1, g2;
    g1.fs = g2.fs = rate;
    g1.carrierHz = 500e3; g1.ebn0Db = 12; g1.seed = 1; g1.aesId = 0x7812E7; g1.gesId = 0x90; g1.callEvery = 4;
    g2.carrierHz = -350e3; g2.ebn0Db = 0; g2.amplitude = 0.8; g2.seed = 2; g2.aesId = 0xABCDEF; g2.gesId = 0x21; g2.callEvery = 4;
    AeroCGenerator c1(g1), c2(g2);

    ManagerConfig mc;
    mc.rate = rate;
    mc.centerHz = 1545.0e6;
    Manager mgr;
    mgr.configure(mc);

    const std::string dir = (std::filesystem::temp_directory_path() / "onair_cvoice_test").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    VoiceMixer mix;
    mix.setDecoderFactory([] { return std::unique_ptr<codec::SpeechCodec>(new FakeCodec()); });
    mix.setRecording(dir);
    std::map<uint32_t, int> framesPerAes;
    std::map<uint32_t, uint64_t> voicePerAes;
    mgr.setVoiceSink([&](const CFrameInfo& f, const uint8_t* v) {
        mix.pushCFrame(f, v);
        if (f.haveCall) framesPerAes[f.aesId]++;
        voicePerAes[f.id]++;
    });

    const double secs = 100;
    const size_t chunk = 65536;
    std::vector<cf32> b1(chunk), b2(chunk);
    Snapshot snap;
    uint64_t last = 0;
    bool sawRow1 = false, sawRow2 = false;
    int16_t pcm[800];
    size_t played = 0;
    double t = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += chunk) {
        c1.generate(b1.data(), chunk);
        c2.generate(b2.data(), chunk);
        for (size_t i = 0; i < chunk; i++) b1[i] += b2[i];
        mgr.feed(b1.data(), chunk);
        t = (double)(done + chunk) / rate;
        if (mgr.snapshot(snap, last)) {
            last = snap.seq;
            for (const auto& r : snap.carriers) {
                if (r.type == CarrierType::AeroC && r.bitRate == 8400 && r.state >= 2) {
                    if (std::fabs(r.offsetHz - 500e3) < 3000) { sawRow1 = true; }
                    if (std::fabs(r.offsetHz + 350e3) < 3000) { sawRow2 = true; }
                }
            }
        }
        const size_t n = mix.mix(pcm, 800);
        for (size_t i = 0; i < n; i++) played += pcm[i] != 0;
    }
    mix.endIdle(t + 10);
    printf("C channels: %d running, %d locked; frames per call unit: ", snap.cChannels, snap.cLocked);
    for (auto& kv : framesPerAes) printf("%06x:%d ", kv.first, kv.second);
    printf("\n");
    CHECK(sawRow1 && sawRow2, "carrier rows of type Aero C: %d %d", sawRow1, sawRow2);
    CHECK(framesPerAes[0x7812E7] >= 8, "call 1 call-progress units: %d", framesPerAes[0x7812E7]);
    CHECK(framesPerAes[0xABCDEF] >= 8, "call 2 call-progress units: %d", framesPerAes[0xABCDEF]);
    const VoiceStatus st = mix.status();
    CHECK(st.codecReady, "fake codec was not used");
    int withAes = 0;
    for (const auto& h : st.history) withAes += (h.aesId == 0x7812E7 && h.gesId == 0x90) || (h.aesId == 0xABCDEF && h.gesId == 0x21);
    CHECK(withAes == 2, "call history with both aircraft: %d of %zu records", withAes, st.history.size());
    for (const auto& h : st.history) {
        CHECK(h.frames >= 25 * 40, "call %06x: %llu voice frames", h.aesId, (unsigned long long)h.frames);
        CHECK(h.avgBer < 0.05f, "call %06x ber %.3f", h.aesId, h.avgBer);
        CHECK(h.decoded == h.frames, "decoded %llu of %llu", (unsigned long long)h.decoded, (unsigned long long)h.frames);
    }
    CHECK(played > 160 * 25 * 20, "mixed samples that are not silence: %zu", played);
    int wavs = 0, bits = 0;
    for (auto& e : std::filesystem::directory_iterator(dir)) { wavs += e.path().extension() == ".wav"; bits += e.path().extension() == ".bits"; }
    CHECK(wavs == 2 && bits == 2, "recordings: %d wav, %d bits", wavs, bits);
    std::filesystem::remove_all(dir);
    // the mixer's mute and solo act per call (carrier id)
    return testSummary("inmsat_cvoice");
}
