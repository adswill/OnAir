// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TV sound in DRA (DTMB, China): a synthetic transport stream with DRA audio PIDs (stream_type 0x14, and private data with the "DRA1" registration)
// goes through the stream parser (codec "DRA", language), FFmpeg's demuxer (the player's source of the PES payloads and times) and TvDraDecoder with a
// FAKE add-on that answers every frame with a constant tone. This checks the plumbing only: it is NOT a test against real DRA audio or a real
// decoder (OnAir ships none; the real add-on is the user's own build).
#include "dect2/cdr/cdr_dra.h"
#include "dect2/tv/ts.h"
#include "dect2/tv/tv_dra.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static std::map<int, int> gCc;
static std::vector<uint8_t> packets(int pid, const std::vector<uint8_t>& pay, bool pusi) {
    std::vector<uint8_t> out;
    size_t pos = 0;
    bool first = true;
    while (pos < pay.size() || first) {
        const size_t n = std::min<size_t>(184, pay.size() - pos);
        uint8_t h[4] = {0x47, (uint8_t)((first && pusi ? 0x40 : 0) | (pid >> 8)), (uint8_t)pid, 0};
        const int cc = gCc[pid]++ & 15;
        if (n < 184) {
            const size_t pad = 184 - n;
            h[3] = (uint8_t)(0x30 | cc);
            out.insert(out.end(), h, h + 4);
            out.push_back((uint8_t)(pad - 1));
            if (pad > 1) { out.push_back(0); out.insert(out.end(), pad - 2, 0xFF); }
        } else {
            h[3] = (uint8_t)(0x10 | cc);
            out.insert(out.end(), h, h + 4);
        }
        out.insert(out.end(), pay.begin() + pos, pay.begin() + pos + n);
        pos += n; first = false;
    }
    return out;
}

static std::vector<uint8_t> section(uint8_t tid, int ext, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> s = {tid, (uint8_t)(0xB0 | ((body.size() + 9) >> 8)), (uint8_t)(body.size() + 9), (uint8_t)(ext >> 8), (uint8_t)ext, 0xC1, 0, 0};
    s.insert(s.end(), body.begin(), body.end());
    const uint32_t c = mpegCrc32(s.data(), (int)s.size());
    for (int i = 3; i >= 0; i--) s.push_back((uint8_t)(c >> (8 * i)));
    return s;
}

static std::vector<uint8_t> pts5(int64_t t) {
    return {(uint8_t)(0x21 | ((t >> 29) & 0x0E)), (uint8_t)(t >> 22), (uint8_t)(0x01 | ((t >> 14) & 0xFE)), (uint8_t)(t >> 7), (uint8_t)(0x01 | ((t << 1) & 0xFE))};
}

static std::vector<uint8_t> pes(const std::vector<uint8_t>& es, int64_t pts) {
    std::vector<uint8_t> p = {0, 0, 1, 0xC0};
    const size_t hl = pts >= 0 ? 5 : 0;
    const size_t len = 3 + hl + es.size();
    p.push_back((uint8_t)(len >> 8)); p.push_back((uint8_t)len);
    p.push_back(0x80); p.push_back(pts >= 0 ? 0x80 : 0x00); p.push_back((uint8_t)hl);
    if (pts >= 0) { auto t = pts5(pts); p.insert(p.end(), t.begin(), t.end()); }
    p.insert(p.end(), es.begin(), es.end());
    return p;
}

// a DRA "frame" of 32 bytes (the fake add-on's format): 7FFF, header with 8 words, filled with `v`
static std::vector<uint8_t> frame(int v) {
    std::vector<uint8_t> f = {0x7F, 0xFF, 0x01, 0x00, 0x00, 0x00};     // (0x01000000 >> 21) & 0x3FF = 8 words = 32 bytes
    f.insert(f.end(), 26, (uint8_t)v);
    return f;
}

struct MemIo { const std::vector<uint8_t>* d; size_t pos; };
static int memRead(void* o, uint8_t* b, int n) {
    MemIo* m = (MemIo*)o;
    if (m->pos >= m->d->size()) return AVERROR_EOF;
    const size_t k = std::min<size_t>((size_t)n, m->d->size() - m->pos);
    memcpy(b, m->d->data() + m->pos, k); m->pos += k;
    return (int)k;
}

int main() {
    const int kFrames = 40;
    // ---- no add-on (first: a library that loaded stays loaded): nothing breaks, the notice says what to do
    setEnv("DECT2_DRA_PATH", "/nonexistent/libonair_dra_missing");
    CHECK(!cdr::draRescan(), "a missing file was accepted");
    {
        TvDraDecoder dec;
        std::vector<DraPcm> out;
        const auto f = frame(1);
        CHECK(!dec.usable() && dec.feed(f.data(), f.size(), 1.0, out) == 0 && out.empty(), "without the add-on nothing is decoded");
        std::string prob;
        const std::string note = draTvNote(cdr::draAvailable(nullptr, &prob), prob);
        CHECK(note.find("install the DRA add-on") != std::string::npos, "notice '%s'", note.c_str());
        CHECK(draTvNote(true, "").empty(), "no notice when the add-on works");
        CHECK(draTvNote(false, "x").find("(x)") != std::string::npos, "unusable add-on notice");
    }
    setEnv("DECT2_DRA_PATH", FAKE_DRA_GOOD);
    CHECK(cdr::draRescan(), "the fake add-on was not accepted");

    // ---- frame cutting
    {
        const auto f = frame(5);
        bool more = true;
        CHECK(draFrameLength(f.data(), f.size(), &more) == 32 && !more, "whole frame");
        CHECK(draFrameLength(f.data(), 20, &more) == 0 && more, "partial frame asks for more");
        const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(draFrameLength(junk, 8, &more) == 0 && !more, "no sync");
    }

    // ---- the synthetic stream: PID 0x101 = type 0x14, frame per PES; PID 0x102 = private + DRA1, every frame split over two PES (the second without a time)
    std::vector<uint8_t> ts;
    auto add = [&](const std::vector<uint8_t>& v) { ts.insert(ts.end(), v.begin(), v.end()); };
    std::vector<uint8_t> pat = {0x00, 0x01, 0xE1, 0x00};
    std::vector<uint8_t> pmtBody = {0xE1, 0x01, 0xF0, 0x00,
        0x14, 0xE1, 0x01, 0xF0, 6, 0x0A, 4, 'c', 'h', 'i', 0,
        0x06, 0xE1, 0x02, 0xF0, 12, 0x05, 4, 'D', 'R', 'A', '1', 0x0A, 4, 'e', 'n', 'g', 0};
    auto withPointer = [](std::vector<uint8_t> s) { s.insert(s.begin(), 0); return s; };
    for (int rep = 0; rep < 2; rep++) {
        add(packets(0, withPointer(section(0x00, 1, pat)), true));
        add(packets(0x100, withPointer(section(0x02, 1, pmtBody)), true));
    }
    const int64_t base = 90000;
    for (int i = 0; i < kFrames; i++) {
        const auto f = frame(i + 1);
        add(packets(0x101, pes(f, base + i * 1920), true));
        add(packets(0x102, pes(std::vector<uint8_t>(f.begin(), f.begin() + 11), base + i * 1920), true));
        add(packets(0x102, pes(std::vector<uint8_t>(f.begin() + 11, f.end()), -1), true));
    }

    // ---- the stream parser
    {
        TsDemux d;
        for (size_t i = 0; i + 188 <= ts.size(); i += 188) d.feed(ts.data() + i);
        const TsSnapshot s = d.snapshot();
        const TsService* sv = nullptr;
        for (const auto& x : s.services) if (x.id == 1) sv = &x;
        CHECK(sv && sv->streams.size() == 2, "service with two streams");
        if (sv && sv->streams.size() == 2) {
            CHECK(sv->streams[0].codec == "DRA" && sv->streams[0].kind == "audio" && sv->streams[0].lang == "chi", "type 0x14: %s %s %s", sv->streams[0].codec.c_str(), sv->streams[0].kind.c_str(), sv->streams[0].lang.c_str());
            CHECK(sv->streams[1].codec == "DRA" && sv->streams[1].kind == "audio" && sv->streams[1].lang == "eng", "DRA1: %s %s %s", sv->streams[1].codec.c_str(), sv->streams[1].kind.c_str(), sv->streams[1].lang.c_str());
        }
    }

    // ---- FFmpeg's demuxer -> TvDraDecoder -> PCM
    std::map<int, std::vector<DraPcm>> got;
    {
        MemIo mio{&ts, 0};
        uint8_t* buf = (uint8_t*)av_malloc(4096);
        AVIOContext* io = avio_alloc_context(buf, 4096, 0, &mio, memRead, nullptr, nullptr);
        AVFormatContext* fmt = avformat_alloc_context();
        fmt->pb = io; fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        CHECK(avformat_open_input(&fmt, "", av_find_input_format("mpegts"), nullptr) >= 0, "open");
        avformat_find_stream_info(fmt, nullptr);
        std::map<int, int> pidOf;     // stream index -> pid
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            CHECK(fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO, "FFmpeg claims it can decode DRA?");
            pidOf[(int)i] = fmt->streams[i]->id;
        }
        CHECK(pidOf.size() == 2, "FFmpeg lists %zu streams", pidOf.size());
        TvDraDecoder dec[2];
        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(fmt, pkt) >= 0) {
            const int pid = pidOf[pkt->stream_index];
            if (pid == 0x101 || pid == 0x102) {
                const double t = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(fmt->streams[pkt->stream_index]->time_base) : -1.0;
                dec[pid - 0x101].feed(pkt->data, (size_t)pkt->size, t, got[pid]);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avformat_close_input(&fmt);
        av_freep(&io->buffer); avio_context_free(&io);
        for (int k = 0; k < 2; k++) CHECK(dec[k].framesIn == (uint64_t)kFrames && dec[k].framesBad == 0 && dec[k].sourceRate == 48000, "pid 0x10%d: %llu frames in, %llu bad", k + 1, (unsigned long long)dec[k].framesIn, (unsigned long long)dec[k].framesBad);
    }
    for (int pid : {0x101, 0x102}) {
        const auto& v = got[pid];
        CHECK((int)v.size() == kFrames, "pid 0x%x: %zu PCM chunks", pid, v.size());
        double worstPts = 0; bool tone = true; bool size = true;
        for (size_t i = 0; i < v.size(); i++) {
            const double want = (double)base / 90000.0 + i * 1024.0 / 48000.0;
            worstPts = std::max(worstPts, std::fabs(v[i].pts - want));
            size = size && v[i].stereo.size() == 2048 && v[i].rate == 48000;
            tone = tone && !v[i].stereo.empty() && std::fabs(v[i].stereo[0] - (float)((i + 1) * 100) / 32768.f) < 1e-6f;
        }
        CHECK(size, "pid 0x%x: chunk size or rate", pid);
        CHECK(tone, "pid 0x%x: the fake add-on's tone did not come out in frame order", pid);
        CHECK(worstPts < 0.0005, "pid 0x%x: worst time error %.6f s", pid, worstPts);
    }

    // ---- another sample rate is converted (24 kHz output: half the samples)
    {
        TvDraDecoder dec(nullptr, 24000);
        std::vector<DraPcm> out;
        for (int i = 0; i < 10; i++) { const auto f = frame(i + 1); dec.feed(f.data(), f.size(), i == 0 ? 1.0 : -1.0, out); }
        size_t total = 0;
        for (const auto& c : out) total += c.stereo.size() / 2;
        CHECK(out.size() == 10 && total >= 5115 && total <= 5125 && out[0].rate == 24000, "resampled: %zu chunks, %zu samples", out.size(), total);
    }

    printf(fails ? "tv_dra: FAILED\n" : "tv_dra: ok\n");
    return fails ? 1 : 0;
}
