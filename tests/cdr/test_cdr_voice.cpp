// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A spoken phrase carried through the CDR test signal and recovered.
// The bytes in the audio units are a small test codec (4-bit ADPCM at 8 kHz), not DRA+.
// A real CDR station still will not play. Pass a 16-bit WAV to use that recording; otherwise a tone is used.
// With a WAV argument the recovered sound is written to /tmp/cdr_voice_out.wav.
#include "../common/tmp_path.h"
#include "dect2/cdr/cdr_gen.h"
#include "dect2/cdr/cdr_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../common/test_util.h"
#include "dect2/common/endian.h"

namespace {

const int kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
const int kIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
int clampIndex(int v) { return v < 0 ? 0 : v > 88 ? 88 : v; }

void adpcmStep(int& pred, int& index, int mag, int sign) {
    int step = kStep[index];
    int delta = step >> 3;
    if (mag & 4) delta += step;
    if (mag & 2) delta += step >> 1;
    if (mag & 1) delta += step >> 2;
    pred = clamp16(pred + (sign ? -delta : delta));
    index = clampIndex(index + kIndex[mag & 7]);
}

uint8_t adpcmEncode(int sample, int& pred, int& index) {
    int diff = sample - pred;
    int sign = 0;
    if (diff < 0) { sign = 1; diff = -diff; }
    int step = kStep[index];
    int mag = 0;
    if (diff >= step) { mag = 4; diff -= step; }
    if (diff >= (step >> 1)) { mag |= 2; diff -= step >> 1; }
    if (diff >= (step >> 2)) mag |= 1;
    adpcmStep(pred, index, mag, sign);
    return (uint8_t)(mag | (sign ? 8 : 0));
}

void put16(std::vector<uint8_t>& d, int v) { dect2::endian::putLe16(d, v); }

// Independent 40 ms packets, so the receiver can start at any packet.
std::vector<uint8_t> encode(const std::vector<int16_t>& pcm) {
    std::vector<uint8_t> out;
    for (size_t at = 0; at < pcm.size();) {
        const int n = (int)std::min<size_t>(320, pcm.size() - at);
        int pred = pcm[at];
        int index = 0;
        std::vector<uint8_t> nib;
        int p = pred, ix = index;
        for (int i = 0; i < n; i++) {
            const uint8_t c = adpcmEncode(pcm[at + (size_t)i], p, ix);
            if (i & 1) nib.back() |= (uint8_t)(c << 4);
            else nib.push_back(c);
        }
        out.push_back('O'); out.push_back('A'); out.push_back('V'); out.push_back(1);
        put16(out, 8000);
        put16(out, n);
        put16(out, pred);
        out.push_back((uint8_t)index);
        out.push_back(0);
        out.insert(out.end(), nib.begin(), nib.end());
        at += (size_t)n;
    }
    return out;
}

bool decodePacket(const uint8_t* d, size_t n, size_t& used, std::vector<int16_t>& pcm) {
    if (n < 12 || d[0] != 'O' || d[1] != 'A' || d[2] != 'V' || d[3] != 1) return false;
    const int rate = d[4] | (d[5] << 8);
    const int ns = d[6] | (d[7] << 8);
    int pred = (int16_t)(d[8] | (d[9] << 8));
    int index = d[10];
    const size_t nbytes = (size_t)(ns + 1) / 2;
    if (rate != 8000 || ns < 1 || ns > 2000 || index > 88 || 12 + nbytes > n) return false;
    for (int i = 0; i < ns; i++) {
        const uint8_t c = (i & 1) ? (d[12 + i / 2] >> 4) : (d[12 + i / 2] & 0x0F);
        const int mag = c & 7, sign = c >> 3;
        adpcmStep(pred, index, mag, sign);
        pcm.push_back((int16_t)pred);
    }
    used = 12 + nbytes;
    return true;
}

std::vector<int16_t> decode(const uint8_t* d, size_t n) {
    std::vector<int16_t> pcm;
    size_t i = 0;
    while (i + 12 <= n) {
        size_t used = 0;
        if (decodePacket(d + i, n - i, used, pcm)) i += used;
        else i++;
    }
    return pcm;
}

bool readWav(const char* path, std::vector<int16_t>& pcm) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});
    if (f.size() < 44 || std::memcmp(f.data(), "RIFF", 4) || std::memcmp(f.data() + 8, "WAVE", 4)) return false;
    size_t at = 12;
    const uint8_t* data = nullptr;
    size_t dataN = 0;
    int channels = 0, rate = 0, bits = 0;
    while (at + 8 <= f.size()) {
        const char* id = (const char*)f.data() + at;
        const uint32_t len = (uint32_t)f[at + 4] | ((uint32_t)f[at + 5] << 8) | ((uint32_t)f[at + 6] << 16) | ((uint32_t)f[at + 7] << 24);
        if (std::memcmp(id, "fmt ", 4) == 0 && at + 8 + 16 <= f.size()) {
            const int fmt = f[at + 8] | (f[at + 9] << 8);
            channels = f[at + 10] | (f[at + 11] << 8);
            rate = (int)((uint32_t)f[at + 12] | ((uint32_t)f[at + 13] << 8) | ((uint32_t)f[at + 14] << 16) | ((uint32_t)f[at + 15] << 24));
            bits = f[at + 22] | (f[at + 23] << 8);
            if (fmt != 1) return false;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = f.data() + at + 8;
            dataN = len;
            break;
        }
        at += 8 + len + (len & 1);
    }
    if (!data || channels != 1 || rate != 8000 || bits != 16 || dataN < 2 || data + dataN > f.data() + f.size()) return false;
    pcm.resize(dataN / 2);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(data[2 * i] | (data[2 * i + 1] << 8));
    return true;
}

void writeWav(const char* path, const std::vector<int16_t>& pcm) {
    const uint32_t dataN = (uint32_t)pcm.size() * 2;
    const uint32_t riff = 36 + dataN;
    uint8_t h[44] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E','f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                     0x40,0x1F,0,0, 0x80,0x3E,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0};
    auto p32 = [](uint8_t* d, uint32_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24); };
    p32(h + 4, riff);
    p32(h + 40, dataN);
    std::ofstream o(path, std::ios::binary);
    o.write((const char*)h, 44);
    o.write((const char*)pcm.data(), (std::streamsize)dataN);
}

double corrAt(const int16_t* a, const int16_t* b, int n) {
    double sa = 0, sb = 0;
    for (int i = 0; i < n; i++) { sa += a[i]; sb += b[i]; }
    sa /= n; sb /= n;
    double sab = 0, aa = 0, bb = 0;
    for (int i = 0; i < n; i++) {
        const double da = a[i] - sa, db = b[i] - sb;
        sab += da * db; aa += da * da; bb += db * db;
    }
    return (aa > 0 && bb > 0) ? sab / std::sqrt(aa * bb) : 0;
}

// Best alignment of one copy of src inside got. The recovered stream can start mid-phrase.
double match(const std::vector<int16_t>& src, const std::vector<int16_t>& got, int* lagOut = nullptr) {
    if (src.size() < 1000 || got.size() < 1000) return -1;
    const int win = (int)std::min<size_t>({src.size(), got.size(), (size_t)4000});
    const int lim = (int)got.size() - win;
    int bestLag = 0;
    double best = -1;
    for (int lag = 0; lag <= lim; lag += 40) {
        const double c = corrAt(src.data(), got.data() + lag, win);
        if (c > best) { best = c; bestLag = lag; }
    }
    const int lo = std::max(0, bestLag - 80), hi = std::min(lim, bestLag + 80);
    for (int lag = lo; lag <= hi; lag++) {
        const double c = corrAt(src.data(), got.data() + lag, win);
        if (c > best) { best = c; bestLag = lag; }
    }
    if (lagOut) *lagOut = bestLag;
    return best;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<int16_t> pcm;
    const bool speech = argc > 1;
    if (speech) {
        CHECK(readWav(argv[1], pcm), "cannot read %s as 8 kHz mono 16-bit WAV", argv[1]);
        if (fails) return 1;
    } else {
        pcm.resize(8000);
        for (int i = 0; i < 8000; i++) pcm[(size_t)i] = (int16_t)(12000 * std::sin(2 * M_PI * 440.0 * i / 8000.0));
    }
    const std::vector<uint8_t> es = encode(pcm);
    const std::vector<int16_t> direct = decode(es.data(), es.size());
    CHECK(direct.size() == pcm.size(), "codec length %zu vs %zu", direct.size(), pcm.size());
    {
        double s = 0, e = 0;
        const size_t n = std::min(pcm.size(), direct.size());
        for (size_t i = 0; i < n; i++) {
            const double d = (double)pcm[i] - (double)direct[i];
            s += (double)pcm[i] * pcm[i];
            e += d * d;
        }
        printf("codec snr %.1f dB\n", 10 * std::log10(s / std::max(1.0, e)));
    }
    const double directMatch = match(pcm, direct);
    CHECK(directMatch > 0.95, "codec match %.3f", directMatch);

    dect2::CdrTxConfig cfg;
    cfg.audioA = es;
    dect2::CdrTransmitter tx(cfg);
    CHECK(tx.ok(), "transmitter");
    dect2::CdrReceiver rx;
    rx.configure(dect2::cdr::kFs);
    const TmpPath path = tmpPath("cdr_voice_es.dra");
    CHECK(rx.saveAudioStream(cfg.serviceA, path), "save stream");
    std::vector<dect2::cf32> sf;
    const int subframes = 28;                       // 4.5 s: lock, then several copies of the phrase
    for (int i = 0; i < subframes; i++) {
        tx.nextSubframe(sf);
        rx.feed(sf.data(), sf.size());
    }
    rx.stopSaveAudioStream();
    dect2::CdrTelemetry tl;
    rx.telemetry(tl, 0);
    printf("state %d  services %zu  mux %llu ok / %llu bad  saved audio units of %04X\n",
           tl.state, tl.services.size(), (unsigned long long)tl.muxOk, (unsigned long long)tl.muxBad, cfg.serviceA);
    CHECK(tl.state == 3 && tl.muxOk >= 2 && tl.muxBad == 0, "receiver did not lock");

    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> got((std::istreambuf_iterator<char>(in)), {});
    printf("elementary stream %zu bytes\n", got.size());
    const std::vector<int16_t> back = decode(got.data(), got.size());
    printf("recovered %zu samples (%.2f s)\n", back.size(), back.size() / 8000.0);
    int lag = 0;
    const double c = match(pcm, back, &lag);
    printf("match %.3f at sample %d\n", c, lag);
    CHECK(back.size() > 8000, "not enough audio came back");
    CHECK(c > 0.9, "recovered audio does not match what was sent (%.3f)", c);
    if (speech && !back.empty()) {
        std::vector<int16_t> one = back;
        if (lag >= 0 && (size_t)lag + pcm.size() <= back.size())
            one.assign(back.begin() + lag, back.begin() + lag + (std::ptrdiff_t)pcm.size());
        writeWav(tmpPath("cdr_voice_out.wav"), one);
        printf("wrote /tmp/cdr_voice_out.wav (%.2f s)\n", one.size() / 8000.0);
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
