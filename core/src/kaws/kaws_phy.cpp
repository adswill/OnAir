// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS physical layer wrapper. See kaws_phy.h.
#include "dect2/kaws/kaws_phy.h"
#include "dect2/rs/kaws.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2::kaws {

int burstSamples(int profile, int nblocks) {
    int n = onair_kaws_burst_len(profile, nblocks);
    return n < 0 ? 0 : n;
}

std::vector<cf> buildBurst(int profile, const std::vector<Bits>& blocks) {
    if (profile < 0 || profile >= kNumProfiles || blocks.empty()) return {};
    const int K = kProfiles[profile].K;
    std::vector<uint8_t> info;
    for (const Bits& b : blocks) {
        if ((int)b.size() != K) return {};
        info.insert(info.end(), b.begin(), b.end());
    }
    int n = onair_kaws_burst_len(profile, (int)blocks.size());
    if (n <= 0) return {};
    std::vector<float> iq((size_t)n * 2);
    int got = onair_kaws_build_burst(profile, (int)blocks.size(), info.data(), iq.data(), n);
    if (got != n) return {};
    std::vector<cf> out((size_t)n);
    for (int i = 0; i < n; i++) out[(size_t)i] = cf(iq[(size_t)i * 2], iq[(size_t)i * 2 + 1]);
    return out;
}

static std::vector<float> flatten(const std::vector<cf>& v) {
    std::vector<float> f(v.size() * 2);
    for (size_t i = 0; i < v.size(); i++) { f[i * 2] = v[i].real(); f[i * 2 + 1] = v[i].imag(); }
    return f;
}

RxBurst receiveBurst(int profile, int nblocks, const std::vector<cf>& iq, int lo, int hi) {
    RxBurst r;
    if (profile < 0 || profile >= kNumProfiles || nblocks < 1) return r;
    const int K = kProfiles[profile].K;
    std::vector<float> f = flatten(iq);
    std::vector<uint8_t> info((size_t)nblocks * (size_t)K);
    r.iters.assign((size_t)nblocks, -1);
    double st[4] = {0, 0, 0, 0};
    int rc = onair_kaws_rx_burst(profile, nblocks, f.data(), (int)iq.size(), lo, hi, info.data(), r.iters.data(), st);
    r.sync = rc == 1;
    r.stat = st[0]; r.tau = st[1]; r.deltaHz = st[2]; r.snrDb = st[3];
    for (int b = 0; b < nblocks; b++) r.blocks.emplace_back(info.begin() + (long)b * K, info.begin() + (long)(b + 1) * K);
    return r;
}

std::vector<cf> applyChannel(const std::vector<cf>& iq, Channel c, double snrDb, double fo, double drift, double delay, int tail, uint64_t seed) {
    std::vector<float> f = flatten(iq);
    int cap = (int)iq.size() + (int)delay + tail + 64;
    std::vector<float> out((size_t)cap * 2);
    int n = onair_kaws_channel(f.data(), (int)iq.size(), (int)c, snrDb, fo, drift, delay, tail, (uint32_t)seed, (uint32_t)(seed >> 32), out.data(), cap);
    if (n < 0) return {};
    std::vector<cf> y((size_t)n);
    for (int i = 0; i < n; i++) y[(size_t)i] = cf(out[(size_t)i * 2], out[(size_t)i * 2 + 1]);
    return y;
}

std::vector<float> toAudio12k(const std::vector<cf>& iq, float peak) {
    std::vector<float> f = flatten(iq);
    std::vector<float> a(iq.size() * 5);
    int n = onair_kaws_to_audio(f.data(), (int)iq.size(), peak, a.data(), (int)a.size());
    if (n < 0) return {};
    a.resize((size_t)n);
    return a;
}

std::vector<cf> fromAudio12k(const std::vector<float>& audio) {
    std::vector<float> o((audio.size() / 5 + 4) * 2);
    int n = onair_kaws_from_audio(audio.data(), (int)audio.size(), o.data(), (int)o.size() / 2);
    if (n < 0) return {};
    std::vector<cf> y((size_t)n);
    for (int i = 0; i < n; i++) y[(size_t)i] = cf(o[(size_t)i * 2], o[(size_t)i * 2 + 1]);
    return y;
}

static void le32(FILE* f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 255), f); }
static void le16(FILE* f, uint32_t v) { for (int i = 0; i < 2; i++) fputc((int)((v >> (8 * i)) & 255), f); }

bool writeWav(const std::string& path, const std::vector<float>& audio, int rate) {
    FILE* f = fopen(path.c_str(), "wb");   // Flawfinder: ignore
    if (!f) return false;
    uint32_t bytes = (uint32_t)audio.size() * 2;
    fwrite("RIFF", 1, 4, f); le32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16); le16(f, 1); le16(f, 1); le32(f, (uint32_t)rate); le32(f, (uint32_t)rate * 2); le16(f, 2); le16(f, 16);
    fwrite("data", 1, 4, f); le32(f, bytes);
    for (float v : audio) {
        float c = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
        int s = (int)(c * 32767.0f + (c >= 0 ? 0.5f : -0.5f));
        le16(f, (uint32_t)(s & 0xFFFF));
    }
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

bool readWav(const std::string& path, std::vector<float>& audio, int& rate) {
    FILE* f = fopen(path.c_str(), "rb");   // Flawfinder: ignore
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    auto u32 = [&](size_t o) { return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24); };
    auto u16 = [&](size_t o) { return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8); };
    size_t p = 12;
    bool haveFmt = false;
    uint32_t channels = 0, bits = 0;
    while (p + 8 <= d.size()) {
        uint32_t len = u32(p + 4);
        if (!std::memcmp(d.data() + p, "fmt ", 4) && p + 8 + 16 <= d.size()) {
            if (u16(p + 8) != 1) return false;
            channels = u16(p + 10);
            rate = (int)u32(p + 12);
            bits = u16(p + 22);
            haveFmt = true;
        } else if (!std::memcmp(d.data() + p, "data", 4)) {
            if (!haveFmt || bits != 16 || channels != 1) return false;
            size_t avail = std::min<size_t>(len, d.size() - p - 8) / 2;
            audio.resize(avail);
            for (size_t i = 0; i < avail; i++) audio[i] = (float)(int16_t)u16(p + 8 + i * 2) / 32768.0f;
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

}  // namespace dect2::kaws
