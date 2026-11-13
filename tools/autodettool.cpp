// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// autodettool: the automatic standard detection (dect2/autodet/autodet.h) on an IQ recording, to check it on real signals.
//   autodettool tv|voice <file> [--rate Hz] [--format cs8|cu8|cs16|cf32|wav] [--skip s] [--sec s] [--rf MHz] [--sat] [--repeat n]
// The format comes from the extension when not given (.cs8 .cu8 .cs16 .cf32 .wav); a .wav file gives its own rate.
// --repeat n: n captures one after the other (the answer should not change).
#include "dect2/autodet/autodet.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;

namespace {

bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

// n complex samples from offset `skip` samples on; false at the end of the file
bool readIq(FILE* f, const std::string& fmt, long dataStart, size_t skip, size_t n, std::vector<cf32>& out) {
    const int bps = fmt == "cs8" || fmt == "cu8" ? 2 : fmt == "cs16" || fmt == "wav" ? 4 : 8;
    if (fseek(f, dataStart + (long)(skip * (size_t)bps), SEEK_SET) != 0) return false;
    std::vector<uint8_t> raw(n * (size_t)bps);
    const size_t got = fread(raw.data(), 1, raw.size(), f) / (size_t)bps;
    if (got < n / 2) return false;
    out.resize(got);
    for (size_t i = 0; i < got; i++) {
        const uint8_t* p = raw.data() + i * (size_t)bps;
        if (fmt == "cs8") out[i] = cf32((int8_t)p[0] / 128.f, (int8_t)p[1] / 128.f);
        else if (fmt == "cu8") out[i] = cf32((p[0] - 127.5f) / 128.f, (p[1] - 127.5f) / 128.f);
        else if (bps == 4) { int16_t a, b; memcpy(&a, p, 2); memcpy(&b, p + 2, 2); out[i] = cf32(a / 32768.f, b / 32768.f); }
        else { float a, b; memcpy(&a, p, 4); memcpy(&b, p + 4, 4); out[i] = cf32(a, b); }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: autodettool tv|voice <file> [--rate Hz] [--format cs8|cu8|cs16|cf32|wav] [--skip s] [--sec s] [--rf MHz] [--sat] [--repeat n]\n"); return 2; }
    const std::string kind = argv[1], path = argv[2];
    double rate = 0, skip = 0, sec = kind == "tv" ? autodet::tvCaptureSec() : autodet::voiceCaptureSec(), rf = 0;
    std::string fmt = endsWith(path, ".cu8") ? "cu8" : endsWith(path, ".cs16") ? "cs16" : endsWith(path, ".cf32") || endsWith(path, ".sigmf-data") ? "cf32" : endsWith(path, ".wav") ? "wav" : "cs8";
    bool sat = false;
    int repeat = 1;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--rate") rate = atof(val());
        else if (a == "--format") fmt = val();
        else if (a == "--skip") skip = atof(val());
        else if (a == "--sec") sec = atof(val());
        else if (a == "--rf") rf = atof(val()) * 1e6;
        else if (a == "--sat") sat = true;
        else if (a == "--repeat") repeat = std::max(1, atoi(val()));
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    long dataStart = 0;
    if (fmt == "wav") {   // RIFF: the "fmt " chunk gives the rate, samples start after the "data" chunk header
        uint8_t h[12];
        if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0) { fprintf(stderr, "not a RIFF file\n"); return 1; }
        for (;;) {
            uint8_t ch[8];
            if (fread(ch, 1, 8, f) != 8) { fprintf(stderr, "no data chunk\n"); return 1; }
            uint32_t len; memcpy(&len, ch + 4, 4);
            if (!memcmp(ch, "fmt ", 4)) {
                std::vector<uint8_t> b(len);
                if (fread(b.data(), 1, len, f) != len) return 1;
                uint32_t sr; memcpy(&sr, b.data() + 4, 4);
                if (rate <= 0) rate = sr;
            } else if (!memcmp(ch, "data", 4)) { dataStart = ftell(f); break; }
            else fseek(f, (long)len, SEEK_CUR);
        }
    }
    if (rate <= 0) { fprintf(stderr, "give --rate\n"); return 2; }
    printf("%s: %s, %.4f Msps, %s, from %.2f s, %.2f s per capture\n", path.c_str(), fmt.c_str(), rate / 1e6, kind.c_str(), skip, sec);
    for (int r = 0; r < repeat; r++) {
        std::vector<cf32> x;
        if (!readIq(f, fmt, dataStart, (size_t)((skip + r * sec) * rate), (size_t)(sec * rate), x)) { printf("end of file\n"); break; }
        if (kind == "tv") {
            autodet::TvOptions o; o.rfHz = rf; o.satellite = sat;
            const autodet::TvResult t = autodet::detectTv(x.data(), x.size(), rate, o);
            printf("[%d] %s  (%.0f ms)\n", r, t.headline.c_str(), t.ms);
            if (r == 0) for (const auto& e : t.evidence) printf("    %s\n", e.c_str());
        } else {
            autodet::VoiceOptions o; o.rfHz = rf;
            const autodet::VoiceResult v = autodet::detectVoice(x.data(), x.size(), rate, o);
            printf("[%d] %s  (%.0f ms)\n", r, v.headline.c_str(), v.ms);
            for (const auto& s : v.signals) printf("    %+9.0f Hz %5.1f kHz %4.0f dB  %-20s %-7s %3.0f %%  %s\n", s.offHz, s.bwHz / 1e3, s.snrDb, autodet::voiceSysName(s.sys),
                                                   autodet::verdictName(s.verdict), s.confidence * 100, s.why.c_str());
        }
    }
    fclose(f);
    return 0;
}
