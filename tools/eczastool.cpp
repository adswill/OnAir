// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas tool: decode a recording, or write the test signal.
//   eczastool rx <file.wav> [--tone Hz] [--offline]   mono or stereo 16-bit WAV of the radio's audio (BFO recording, carrier at --tone, default 1000 Hz;
//                                                      the first channel is used), or a stereo IQ WAV / .cf32 with --iq --rate Hz --offset Hz
//   eczastool gen <out.cf32> [--rate Hz] [--secs s] [--snr dB] [--cfo Hz]
// rx runs the streaming receiver the app uses; --offline runs the whole-file chain of the first version (Hilbert transform, band-pass) for comparison.
#include "dect2/eczas/eczas_gen.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_rx.h"
#include "dect2/eczas/eczas_tel.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;

static bool loadWav(const std::string& path, std::vector<int16_t>& pcm, int& ch, double& rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char h[12];
    if (!f.read(h, 12) || std::memcmp(h, "RIFF", 4)) return false;
    ch = 1; rate = 0;
    for (;;) {
        char id[4];
        uint32_t n = 0;
        if (!f.read(id, 4) || !f.read((char*)&n, 4)) return false;
        if (!std::memcmp(id, "fmt ", 4)) {
            uint16_t v[8]{};
            f.read((char*)v, 16);
            ch = v[1];
            uint32_t sr = 0;
            std::memcpy(&sr, (char*)v + 4, 4);
            rate = sr;
            if (v[7] != 16) return false;
            f.seekg((long)n - 16, std::ios::cur);
        } else if (!std::memcmp(id, "data", 4)) {
            pcm.resize(n / 2);
            f.read((char*)pcm.data(), (long)n);
            return f.good() || f.eof();
        } else f.seekg((long)n + (n & 1), std::ios::cur);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: eczastool rx <file.wav> [--tone Hz] [--offline]\n       eczastool gen <out.cf32> [--rate Hz] [--secs s] [--snr dB] [--cfo Hz]\n"); return 2; }
    const std::string cmd = argv[1], path = argv[2];
    double tone = 1000, rate = 256000, secs = 180, snr = 30, cfo = 0;
    bool offline = false, iq = false;
    double offsetHz = 0;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--tone" && i + 1 < argc) tone = atof(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--secs" && i + 1 < argc) secs = atof(argv[++i]);
        else if (a == "--snr" && i + 1 < argc) snr = atof(argv[++i]);
        else if (a == "--cfo" && i + 1 < argc) cfo = atof(argv[++i]);
        else if (a == "--offset" && i + 1 < argc) offsetHz = atof(argv[++i]);
        else if (a == "--offline") offline = true;
        else if (a == "--iq") iq = true;
    }
    if (cmd == "gen") {
        EczasGenConfig c;
        c.rate = rate; c.snrDb = snr; c.cfoHz = cfo; c.dialOffsetHz = offsetHz;
        EczasGenerator g(c);
        std::ofstream o(path, std::ios::binary);
        std::vector<cf32> buf(65536);
        for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { g.generate(buf.data(), buf.size()); o.write((const char*)buf.data(), (long)(buf.size() * sizeof(cf32))); }
        return 0;
    }
    std::vector<int16_t> pcm;
    int ch = 1;
    double wr = 0;
    if (!loadWav(path, pcm, ch, wr)) { fprintf(stderr, "cannot read %s (16-bit WAV)\n", path.c_str()); return 1; }
    printf("%s: %.0f Hz, %d channel(s), %.1f s\n", path.c_str(), wr, ch, pcm.size() / (double)ch / wr);
    std::vector<int16_t> mono(pcm.size() / (size_t)ch);
    for (size_t i = 0; i < mono.size(); i++) mono[i] = pcm[i * (size_t)ch];
    auto utc = [](int64_t s) { int y, mo, d, h, mi, ss; eczas::civilFromUnix(s, y, mo, d, h, mi, ss); char b[48]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, ss); return std::string(b); };
    if (offline) {
        const auto pk = eczas::eczasDecodeRecording(mono.data(), mono.size(), wr);
        for (const auto& p : pk) printf("offline: %s UTC +%d h\n", utc(p.time.utcSec).c_str(), p.time.tzHours);
        printf("offline: %zu time packet(s)\n", pk.size());
        return 0;
    }
    EczasLiveReceiver rx;
    rx.setAudioInput(!iq, tone);
    if (iq) rx.setSignalOffset(offsetHz);
    rx.configure(wr);
    std::vector<cf32> blk(4096);
    uint64_t seq = 0;
    EczasTelemetry t;
    size_t shown = 0;
    for (size_t pos = 0; pos < mono.size(); pos += blk.size()) {
        const size_t n = std::min(blk.size(), mono.size() - pos);
        for (size_t i = 0; i < n; i++) blk[i] = iq ? cf32(pcm[(pos + i) * 2] / 32768.f, pcm[(pos + i) * 2 + 1] / 32768.f) : cf32(mono[pos + i] / 32768.f, 0.f);
        rx.feed(blk.data(), n);
        if (rx.telemetry(t, seq)) {
            seq = t.seq;
            for (size_t k = t.log.size() > shown ? t.log.size() - shown : 0; k-- > 0;) {}
        }
    }
    rx.telemetry(t, 0);
    for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) printf("%8.2f s  %s  %s\n", it->streamSec, it->hex.c_str(), it->text.c_str());
    printf("syncs %llu, frames %llu, time packets %llu ok / %llu bad CRC, other slots %llu, false syncs %llu; carrier SNR %.1f dB, level %.1f dBFS, offset %+.2f Hz\n",
           (unsigned long long)t.syncs, (unsigned long long)t.frames, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
           (unsigned long long)t.otherType, (unsigned long long)t.falseSync, t.snrDb, t.levelDb, t.cfoHz);
    return 0;
}
