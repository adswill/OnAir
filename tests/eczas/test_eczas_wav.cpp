// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Decode e-Czas time packets from discriminator WAV recordings (scratchpad/eczas_recordings/).
#include "dect2/eczas/eczas_rx.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2::eczas;

static int fails = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static bool loadWavMono16(const std::string& path, std::vector<int16_t>& pcm, double& rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char h[12];
    if (!f.read(h, 12) || std::memcmp(h, "RIFF", 4)) return false;
    int ch = 1, bits = 16;
    rate = 0;
    for (;;) {
        char id[4];
        uint32_t n = 0;
        if (!f.read(id, 4) || !f.read(reinterpret_cast<char*>(&n), 4)) return false;
        if (!std::memcmp(id, "fmt ", 4)) {
            uint16_t v[8]{};
            f.read(reinterpret_cast<char*>(v), 16);
            ch = v[1];
            bits = v[7];
            uint32_t sr = 0;
            std::memcpy(&sr, reinterpret_cast<char*>(v) + 4, 4);
            rate = sr;
            f.seekg((long)n - 16, std::ios::cur);
        } else if (!std::memcmp(id, "data", 4)) {
            if (bits != 16 || ch != 1) return false;
            pcm.resize(n / 2);
            f.read(reinterpret_cast<char*>(pcm.data()), (long)n);
            return f.good();
        } else
            f.seekg((long)n + (n & 1), std::ios::cur);
    }
}

static std::string formatUtc(int64_t utcSec) {
    std::time_t t = (std::time_t)utcSec;
    std::tm gt{};
#if defined(_WIN32)
    gmtime_s(&gt, &t);
#else
    gmtime_r(&t, &gt);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d UTC", gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday, gt.tm_hour,
                  gt.tm_min, gt.tm_sec);
    return buf;
}

// The streaming receiver the app uses on the same recording (audio input, carrier at 1 kHz): the time packets must agree with the signal time of the
// recording (the stamps of two packets differ by the time between them), carry the Polish offset of the season, and be more than the first chain finds.
static void checkLive(const std::vector<int16_t>& pcm, double rate, const char* name, size_t minPackets, int tz) {
    using namespace dect2;
    EczasLiveReceiver rx;
    rx.setAudioInput(true, 1000);
    rx.configure(rate);
    std::vector<cf32> blk(4096);
    for (size_t pos = 0; pos < pcm.size(); pos += blk.size()) {
        const size_t n = std::min(blk.size(), pcm.size() - pos);
        for (size_t i = 0; i < n; i++) blk[i] = cf32(pcm[pos + i] / 32768.f, 0.f);
        rx.feed(blk.data(), n);
    }
    EczasTelemetry t;
    rx.telemetry(t, 0);
    std::vector<std::pair<double, double>> st;   // signal time, stamp
    for (const auto& l : t.log) {
        if (l.kind != 0) continue;
        int y, mo, d, h, mi, s;
        if (sscanf(l.text.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) continue;
        double days = 0;
        {   // days from civil
            int yy = y - (mo <= 2);
            const long era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            days = (double)(era * 146097 + doe - 719468);
        }
        st.push_back({l.streamSec, days * 86400 + h * 3600 + mi * 60 + s});
    }
    printf("%s (streaming): %llu syncs, %llu frames, %llu time packets, %llu bad CRC, %llu other slots, SNR %.1f dB, %zu in the log\n", name, (unsigned long long)t.syncs,
           (unsigned long long)t.frames, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.otherType, t.snrDb, st.size());
    CHECK(t.blocksOk >= minPackets, "%s: %llu time packets, at least %zu expected", name, (unsigned long long)t.blocksOk, minPackets);
    CHECK(t.pkt.tzHours == tz, "%s: local offset +%d h, +%d h expected (summer time)", name, t.pkt.tzHours, tz);
    CHECK(t.snrDb > 10 && t.snrDb < 40, "%s: carrier SNR %.1f dB", name, t.snrDb);
    int bad = 0;
    for (size_t i = 1; i < st.size(); i++) {
        const double dStream = st[i].first - st[0].first, dStamp = st[i].second - st[0].second;
        if (std::fabs(dStream - dStamp) > 0.5 + 1e-4 * std::fabs(dStamp)) bad++;
        if (std::fmod(st[i].second, 3.0) != 0) bad++;
    }
    CHECK(bad == 0, "%s: %d stamps disagree with the signal time of the recording", name, bad);
}

int main() {
    for (const char* base : {"224k_1836", "224k_2102"}) {
        std::vector<int16_t> pcm;
        double rate = 0;
        const std::string p = std::string(DECT2_SOURCE_DIR) + "/scratchpad/eczas_recordings/" + base + ".wav";
        if (loadWavMono16(p, pcm, rate)) checkLive(pcm, rate, base, std::string(base) == "224k_1836" ? 3 : 25, 2);
        else printf("%s: not found, skipped\n", p.c_str());
    }
    const std::string wav = std::string(DECT2_SOURCE_DIR) + "/scratchpad/eczas_recordings/224k_2102.wav";
    std::vector<int16_t> pcm;
    double rate = 0;
    if (!loadWavMono16(wav, pcm, rate)) {
        printf("%s: not found, skipped\n", wav.c_str());
        return 0;
    }
    printf("loaded %s: %.0f Hz, %zu samples (%.1f s)\n", wav.c_str(), rate, pcm.size(), pcm.size() / rate);

    const auto pkts = eczasDecodeRecording(pcm.data(), pcm.size(), rate);
    CHECK(!pkts.empty(), "no time packet decoded");
    if (pkts.empty()) return fails ? 1 : 0;

    const EczasRxPacket& p = pkts[0];
    CHECK(p.time.status == DecodeStatus::CrcOk, "status %d", (int)p.time.status);
    CHECK(p.time.timeMarker == kTimeMarker, "marker 0x%x", p.time.timeMarker);
    printf("decoded %zu packet(s); first utcSec=%lld (%s), TZ +%d h, periods3s=%u\n", pkts.size(), (long long)p.time.utcSec,
           formatUtc(p.time.utcSec).c_str(), p.time.tzHours, p.time.periods3s);
    printf("raw frame:");
    for (int i = 0; i < kFrameBytes; i++) printf(" %02X", p.frame[i]);
    printf("\n");

    return fails ? 1 : 0;
}
