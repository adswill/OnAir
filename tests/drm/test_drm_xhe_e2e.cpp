// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM xHE-AAC end to end with a REAL encoder: the access units of an xHE-AAC file (MP4 from exhale: a 1 kHz tone) go through our DRM transmitter
// (super frame packer, SDC type 9, channel coding), the receiver, and the real USAC decoder (libfdk-aac, DECT2_FDKAAC_LIB or the system's, else
// FFmpeg); the sound that comes out must carry the tone. The test needs DECT2_XHE_FILE (the .m4a) and stays quiet without it; the workflow
// .github/workflows/xhe-e2e.yml makes the file with exhale and runs this test.
#include "dect2/drm/drm_audio.h"
#include "dect2/drm/drm_gen.h"
#include "dect2/drm/drm_rx.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::drm;

#include "../common/test_util.h"

// the extradata (AudioSpecificConfig) and the access units of the first audio stream
static bool readFile(const char* path, std::vector<uint8_t>& asc, std::vector<std::vector<uint8_t>>& units, int& rate, int& chans) {
    AVFormatContext* f = nullptr;
    if (avformat_open_input(&f, path, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(f, nullptr) < 0) { avformat_close_input(&f); return false; }
    const int si = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) { avformat_close_input(&f); return false; }
    const AVCodecParameters* cp = f->streams[si]->codecpar;
    asc.assign(cp->extradata, cp->extradata + cp->extradata_size);
    rate = cp->sample_rate;
    chans = cp->ch_layout.nb_channels;
    AVPacket* pk = av_packet_alloc();
    while (av_read_frame(f, pk) >= 0) {
        if (pk->stream_index == si && pk->size > 0) units.emplace_back(pk->data, pk->data + pk->size);
        av_packet_unref(pk);
    }
    av_packet_free(&pk);
    avformat_close_input(&f);
    return true;
}

int main() {
    const char* path = std::getenv("DECT2_XHE_FILE");
    if (!path || !*path) { printf("drm_xhe_e2e: DECT2_XHE_FILE not set, nothing to do\n"); return 0; }
    std::vector<uint8_t> asc;
    std::vector<std::vector<uint8_t>> units;
    int rate = 0, chans = 0;
    CHECK(readFile(path, asc, units, rate, chans), "the file %s can be read", path);
    if (fails) return testSummary("drm_xhe_e2e");
    { std::string hex; char t[4]; for (uint8_t b : asc) { snprintf(t, sizeof t, "%02x", b); hex += t; } printf("ASC: %s\n", hex.c_str()); }
    printf("file: %d Hz, %d channels, ASC %zu bytes, %zu access units\n", rate, chans, asc.size(), units.size());
    CHECK(units.size() > 50, "the file holds access units (%zu)", units.size());
    SdcAudio sdc;
    CHECK(xheSdcFromAsc(asc, sdc), "DRM can carry this stream's configuration");
    if (fails) return testSummary("drm_xhe_e2e");
    printf("DRM: mode %d, rate code %d, static config %zu bytes\n", sdc.mode, sdc.rateCode, sdc.config.size());

    DrmTxConfig c;
    c.audioCoding = 3; c.audioMode = sdc.mode; c.audioRateHz = sdc.rateHz();
    c.audioXheConfig = sdc.config;
    c.textMessage = false;
    DrmTransmitter tx(c, makeDrmXheListSource(units));
    CHECK(tx.ok(), "the transmitter accepts the configuration");
    if (!tx.ok()) return testSummary("drm_xhe_e2e");

    DrmReceiver rx;
    rx.setSilent(true);
    rx.configure(tx.sampleRate());
    std::vector<float> pcm;
    std::string log;
    int frames = 0;
    rx.setAudioTap([&](const float* l, const float*, size_t n) { pcm.insert(pcm.end(), l, l + n); });
    rx.setLogCallback([&](const std::string& s) { log += s + "\n"; });
    rx.setStreamCallback([&](int, const uint8_t*, int, int, const DrmTelemetry&) { frames++; });
    std::vector<cf32> out;
    for (int i = 0; i < 24; i++) { out.clear(); tx.superFrame(out); rx.feed(out.data(), out.size()); }
    printf("receiver: %d logical frames, %zu sound samples\n--- receiver log ---\n%s--------------------\n", frames, pcm.size(), log.c_str());
    CHECK(frames > 20, "the receiver delivers logical frames (%d)", frames);
    CHECK(pcm.size() > 48000 * 4, "the receiver delivers sound (%zu samples)", pcm.size());
    if (pcm.size() > 48000 * 4) {
        // one second from the middle of the sound: the share of the power that is the 1 kHz tone
        const size_t N = 48000, at = (pcm.size() - N) / 2;
        double a = 0, b = 0, p = 0;
        for (size_t i = 0; i < N; i++) {
            const double x = pcm[at + i], w = 2.0 * M_PI * 1000.0 * (double)i / 48000.0;
            a += x * std::cos(w); b += x * std::sin(w); p += x * x;
        }
        a *= 2.0 / (double)N; b *= 2.0 / (double)N; p /= (double)N;
        const double tone = (a * a + b * b) / 2, share = p > 0 ? tone / p : 0;
        printf("middle second: rms %.4f, the 1 kHz tone is %.1f%% of the power\n", std::sqrt(p), share * 100);
        CHECK(p > 1e-6, "the sound is not silent (rms %.6f)", std::sqrt(p));
        CHECK(share > 0.6, "the sound is the encoded tone (%.1f%% at 1 kHz)", share * 100);
    }
    return testSummary("drm_xhe_e2e");
}
