// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MP4 fragments of a video and an audio component (as ROUTE delivers them) are remuxed into one MPEG-2 transport stream. The result is read
// back with libavformat: one HEVC and one AAC stream, with all frames, in step with each other.
//   test_atsc3_remux <video.mp4> <audio.mp4>      (the fragmented files in tests/data)
#include "dect2/atsc3/atsc3_remux.h"
#include "dect2/audio/ac4_dec.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// waits until the output has stopped growing (no new bytes in 50 ms), 10 s at most; the reader thread fills it, so its size is asked for anew each time
static void waitQuiet(const std::function<size_t()>& size) {
    for (int i = 0; i < 100; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); const size_t a = size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (a == size() && a > 0) break; }
}

// splits a fragmented MP4 into the init segment (everything before the first moof) and media segments (moof + mdat, possibly several boxes each)
static void split(const std::vector<uint8_t>& f, std::vector<uint8_t>& init, std::vector<std::vector<uint8_t>>& segs) {
    size_t pos = 0;
    bool inInit = true;
    while (pos + 8 <= f.size()) {
        uint32_t sz = (f[pos] << 24) | (f[pos + 1] << 16) | (f[pos + 2] << 8) | f[pos + 3];
        if (sz < 8 || pos + sz > f.size()) break;
        char type[5] = {(char)f[pos + 4], (char)f[pos + 5], (char)f[pos + 6], (char)f[pos + 7], 0};
        bool moof = !strcmp(type, "moof");
        if (moof) { inInit = false; segs.emplace_back(); }
        if (inInit) init.insert(init.end(), f.begin() + pos, f.begin() + pos + sz);
        else if (!segs.empty() && strcmp(type, "sidx") && strcmp(type, "styp")) segs.back().insert(segs.back().end(), f.begin() + pos, f.begin() + pos + sz);
        pos += sz;
    }
}

// Moves the decode times (tfdt) of a fragment by `seconds` at the track's timescale (from the init segment's mdhd): a component on another clock.
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t timescaleOf(const std::vector<uint8_t>& init) {
    for (size_t i = 4; i + 28 < init.size(); i++)
        if (!memcmp(&init[i], "mdhd", 4)) return rd32(&init[i + (init[i + 4] == 1 ? 24 : 16)]);
    return 0;
}
static void shiftTfdt(std::vector<uint8_t>& seg, uint32_t timescale, int seconds) {
    for (size_t i = 4; i + 16 < seg.size(); i++) {
        if (memcmp(&seg[i], "tfdt", 4)) continue;
        uint8_t* t = &seg[i + 8];
        const int len = seg[i + 4] == 1 ? 8 : 4;
        uint64_t v = 0;
        for (int k = 0; k < len; k++) v = v << 8 | t[k];
        v += (uint64_t)timescale * seconds;
        for (int k = len - 1; k >= 0; k--) { t[k] = (uint8_t)v; v >>= 8; }
    }
}

struct MemIn { const uint8_t* d; size_t n, pos = 0; };
static int memRead(void* o, uint8_t* buf, int size) {
    MemIn* m = (MemIn*)o;
    if (m->pos >= m->n) return AVERROR_EOF;
    int k = (int)std::min<size_t>(size, m->n - m->pos);
    memcpy(buf, m->d + m->pos, k);
    m->pos += k;
    return k;
}

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    auto vf = slurp(vp), af = slurp(ap);
    if (vf.empty() || af.empty()) { printf("test files not found (%s, %s)\n", vp, ap); return 1; }
    std::vector<uint8_t> vinit, ainit;
    std::vector<std::vector<uint8_t>> vseg, aseg;
    split(vf, vinit, vseg);
    split(af, ainit, aseg);
    printf("  video: init %zu bytes + %zu segments, audio: init %zu bytes + %zu segments\n", vinit.size(), vseg.size(), ainit.size(), aseg.size());
    CHECK(!vinit.empty() && vseg.size() >= 2 && !ainit.empty() && aseg.size() >= 1, "test files have an init segment and fragments");

    Atsc3Remux rx;
    rx.addComponent(1);
    rx.addComponent(2);
    std::vector<uint8_t> ts;
    std::thread reader([&] {
        uint8_t buf[8192];
        for (;;) { int n = rx.read(buf, sizeof buf); if (n <= 0) break; ts.insert(ts.end(), buf, buf + n); }
    });
    // deliver like a broadcast: init segments, then the fragments of both components interleaved
    rx.push(1, vinit);
    rx.push(2, ainit);
    size_t n = std::max(vseg.size(), aseg.size());
    for (size_t i = 0; i < n; i++) {
        if (i < vseg.size()) rx.push(1, vseg[i]);
        if (i < aseg.size()) rx.push(2, aseg[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // wait until everything has come through, then stop
    for (int i = 0; i < 100; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); size_t a = ts.size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (a == ts.size() && a > 0) break; }
    rx.stop();
    reader.join();
    printf("  output: %zu bytes of transport stream, %d streams\n", ts.size(), rx.streamCount());
    if (argc > 3) { std::ofstream o(argv[3], std::ios::binary); o.write((const char*)ts.data(), (std::streamsize)ts.size()); }
    CHECK(rx.error().empty(), "no error");
    CHECK(rx.started() && rx.streamCount() == 2, "two streams in the output");
    CHECK(ts.size() > 188 * 20 && ts.size() % 188 == 0 && ts[0] == 0x47, "transport stream packets");

    // read it back
    MemIn mi{ts.data(), ts.size()};
    unsigned char* iobuf = (unsigned char*)av_malloc(1 << 16);
    AVIOContext* io = avio_alloc_context(iobuf, 1 << 16, 0, &mi, memRead, nullptr, nullptr);
    AVFormatContext* ic = avformat_alloc_context();
    ic->pb = io;
    CHECK(avformat_open_input(&ic, "", av_find_input_format("mpegts"), nullptr) >= 0 && avformat_find_stream_info(ic, nullptr) >= 0, "read back the transport stream");
    int vid = -1, aud = -1;
    for (unsigned i = 0; ic && i < ic->nb_streams; i++) {
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vid = (int)i;
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) aud = (int)i;
    }
    CHECK(vid >= 0 && ic->streams[vid]->codecpar->codec_id == AV_CODEC_ID_HEVC && ic->streams[vid]->codecpar->width == 160, "HEVC video 160 pixels wide");
    CHECK(aud >= 0 && ic->streams[aud]->codecpar->codec_id == AV_CODEC_ID_AAC && ic->streams[aud]->codecpar->sample_rate == 48000, "AAC audio at 48 kHz");
    long vp2 = 0, ap2 = 0;
    int64_t vfirst = -1, afirst = -1, vlast = -1, alast = -1;
    AVPacket* pk = av_packet_alloc();
    while (ic && av_read_frame(ic, pk) >= 0) {
        AVRational tb = ic->streams[pk->stream_index]->time_base;
        int64_t t = pk->pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(pk->pts, tb, AVRational{1, 1000});
        if (pk->stream_index == vid) { vp2++; if (vfirst < 0) vfirst = t; vlast = t; }
        if (pk->stream_index == aud) { ap2++; if (afirst < 0) afirst = t; alast = t; }
        av_packet_unref(pk);
    }
    // frames in the fragmented input: count with libavformat too
    auto count = [&](const std::vector<uint8_t>& whole, int type) {
        MemIn m2{whole.data(), whole.size()};
        unsigned char* b = (unsigned char*)av_malloc(1 << 16);
        AVIOContext* io2 = avio_alloc_context(b, 1 << 16, 0, &m2, memRead, nullptr, nullptr);
        AVFormatContext* c2 = avformat_alloc_context();
        c2->pb = io2;
        long k = 0;
        if (avformat_open_input(&c2, "", av_find_input_format("mov"), nullptr) >= 0) {
            AVPacket* q = av_packet_alloc();
            while (av_read_frame(c2, q) >= 0) { k += c2->streams[q->stream_index]->codecpar->codec_type == type; av_packet_unref(q); }
            av_packet_free(&q);
            avformat_close_input(&c2);
        }
        av_freep(&io2->buffer);
        avio_context_free(&io2);
        return k;
    };
    long vin = count(vf, AVMEDIA_TYPE_VIDEO), ain = count(af, AVMEDIA_TYPE_AUDIO);
    printf("  video packets: %ld in, %ld out; audio packets: %ld in, %ld out\n", vin, vp2, ain, ap2);
    printf("  video %lld..%lld ms, audio %lld..%lld ms\n", (long long)vfirst, (long long)vlast, (long long)afirst, (long long)alast);
    CHECK(vp2 == vin && vin > 50, "all video frames");
    CHECK(ap2 >= ain - 2 && ap2 <= ain && ain > 50, "all audio frames");
    CHECK(vfirst >= 0 && afirst >= 0 && std::llabs(vfirst - afirst) < 300 && std::llabs(vlast - alast) < 500, "video and audio start and end together");
    av_packet_free(&pk);
    avformat_close_input(&ic);
    av_freep(&io->buffer);
    avio_context_free(&io);

    // A component whose init segment comes after the output has started (lost on air the first time, or late): the output takes it in and
    // goes on with both. Before, the components open at the start were the only ones for good: a picture whose init segment came late was
    // never shown, and when only a sound that cannot be played was open, the output ended.
    {
        Atsc3Remux r2;
        r2.setStartupWaitMs(300);
        std::vector<uint8_t> ts2;
        std::thread rd([&] {
            uint8_t buf[8192];
            for (;;) { int k = r2.read(buf, sizeof buf); if (k <= 0) break; ts2.insert(ts2.end(), buf, buf + k); }
        });
        auto waitFor = [](const std::function<bool()>& f) { for (int i = 0; i < 100 && !f(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(30)); return f(); };
        r2.push(1, vinit);
        r2.push(1, vseg[0]);
        const bool first = waitFor([&] { return r2.started() && r2.streamCount() == 1; });
        r2.push(2, ainit);
        for (size_t i = 0; i < aseg.size(); i++) r2.push(2, aseg[i]);
        const bool both = waitFor([&] { return r2.streamCount() == 2; });
        for (size_t i = 1; i < vseg.size(); i++) r2.push(1, vseg[i]);
        waitQuiet([&] { return ts2.size(); });
        r2.stop();
        rd.join();
        printf("  late audio init: output %zu bytes, started with %s, %d streams after\n", ts2.size(), first ? "1 stream" : "?", r2.streamCount());
        CHECK(first, "late init: the output starts with the component that is there");
        CHECK(both && r2.streamCount() == 2, "late init: the output goes on with both components");
        // read back: both streams, the picture on from start to end, the sound from where it joined to the end, the timestamps in step
        MemIn m3{ts2.data(), ts2.size()};
        unsigned char* b3 = (unsigned char*)av_malloc(1 << 16);
        AVIOContext* io3 = avio_alloc_context(b3, 1 << 16, 0, &m3, memRead, nullptr, nullptr);
        AVFormatContext* c3 = avformat_alloc_context();
        c3->pb = io3;
        CHECK(avformat_open_input(&c3, "", av_find_input_format("mpegts"), nullptr) >= 0, "late init: read back");
        long nv = 0, na = 0, back = 0;
        int64_t vf = -1, vl = -1, af2 = -1, al = -1;
        std::map<int, int64_t> lastD;
        AVPacket* q = av_packet_alloc();
        while (c3 && av_read_frame(c3, q) >= 0) {
            AVStream* st = c3->streams[q->stream_index];
            int64_t t = q->pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(q->pts, st->time_base, AVRational{1, 1000});
            if (q->dts != AV_NOPTS_VALUE) { auto it = lastD.find(q->stream_index); if (it != lastD.end() && q->dts <= it->second) back++; lastD[q->stream_index] = q->dts; }
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { nv++; if (vf < 0) vf = t; vl = t; }
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { na++; if (af2 < 0) af2 = t; al = t; }
            av_packet_unref(q);
        }
        av_packet_free(&q);
        printf("  late audio init: video %ld packets %lld..%lld ms, audio %ld packets %lld..%lld ms, %ld timestamps going back\n", nv, (long long)vf, (long long)vl, na, (long long)af2, (long long)al, back);
        CHECK(nv >= vin - 25 && nv <= vin, "late init: the picture goes on (at most one segment lost to the restart)");
        CHECK(na > ain / 3 && na <= ain, "late init: the sound is there from where it joined");
        CHECK(af2 >= vf && al >= 0 && std::llabs(vl - al) < 500, "late init: picture and sound end together");
        CHECK(back == 0, "late init: timestamps of each stream go forward");
        avformat_close_input(&c3);
        av_freep(&io3->buffer);
        avio_context_free(&io3);
    }

    // A late component on another clock (its times 1000 s away from those of the output, as after a service change): it joins at the point
    // where the output is, instead of having its packets dropped as too old or written far ahead.
    {
        const uint32_t ts = timescaleOf(ainit);
        std::vector<std::vector<uint8_t>> shifted = aseg;
        for (auto& g : shifted) shiftTfdt(g, ts, 1000);
        Atsc3Remux r4;
        r4.setStartupWaitMs(300);
        std::vector<uint8_t> ts4;
        std::thread rd([&] {
            uint8_t buf[8192];
            for (;;) { int k = r4.read(buf, sizeof buf); if (k <= 0) break; ts4.insert(ts4.end(), buf, buf + k); }
        });
        r4.push(1, vinit);
        r4.push(1, vseg[0]);
        for (int i = 0; i < 100 && !r4.started(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        r4.push(2, ainit);
        for (auto& g : shifted) r4.push(2, g);
        for (int i = 0; i < 100 && r4.streamCount() < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        for (size_t i = 1; i < vseg.size(); i++) r4.push(1, vseg[i]);
        waitQuiet([&] { return ts4.size(); });
        r4.stop();
        rd.join();
        MemIn m5{ts4.data(), ts4.size()};
        unsigned char* b5 = (unsigned char*)av_malloc(1 << 16);
        AVIOContext* io5 = avio_alloc_context(b5, 1 << 16, 0, &m5, memRead, nullptr, nullptr);
        AVFormatContext* c5 = avformat_alloc_context();
        c5->pb = io5;
        long nv = 0, na = 0;
        int64_t vl = -1, af = -1, al = -1;
        if (avformat_open_input(&c5, "", av_find_input_format("mpegts"), nullptr) >= 0) {
            AVPacket* q = av_packet_alloc();
            while (av_read_frame(c5, q) >= 0) {
                AVStream* st = c5->streams[q->stream_index];
                const int64_t t = q->pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(q->pts, st->time_base, AVRational{1, 1000});
                if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { nv++; vl = t; }
                if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { na++; if (af < 0) af = t; al = t; }
                av_packet_unref(q);
            }
            av_packet_free(&q);
            avformat_close_input(&c5);
        }
        av_freep(&io5->buffer);
        avio_context_free(&io5);
        printf("  late sound on another clock: video %ld packets to %lld ms, audio %ld packets %lld..%lld ms\n", nv, (long long)vl, na, (long long)af, (long long)al);
        CHECK(na > ain / 3 && af >= 0 && af < 10000, "late sound on another clock: it joins where the output is");
        CHECK(nv >= vin - 25, "late sound on another clock: the picture goes on");
    }

    // A late component that would add no stream (AC-4 sound, no AC-4 library) does not start the output again: one run, one table version,
    // continuity counters unbroken, every picture there.
    {
        ac4SetLibraryDir("/nonexistent-ac4-dir");
        std::vector<uint8_t> ac4init = ainit;
        for (size_t i = 0; i + 4 <= ac4init.size(); i++) if (!memcmp(&ac4init[i], "mp4a", 4)) std::copy_n("ac-4", 4, &ac4init[i]);
        Atsc3Remux r3;
        r3.setStartupWaitMs(300);
        std::vector<uint8_t> ts3;
        std::thread rd([&] {
            uint8_t buf[8192];
            for (;;) { int k = r3.read(buf, sizeof buf); if (k <= 0) break; ts3.insert(ts3.end(), buf, buf + k); }
        });
        r3.push(1, vinit);
        r3.push(1, vseg[0]);
        for (int i = 0; i < 100 && !r3.started(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        r3.push(2, ac4init);
        for (size_t i = 0; i < aseg.size(); i++) r3.push(2, aseg[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (size_t i = 1; i < vseg.size(); i++) r3.push(1, vseg[i]);
        waitQuiet([&] { return ts3.size(); });
        r3.stop();
        rd.join();
        ac4SetLibraryDir("");
        std::set<int> patVersions;
        std::map<int, int> cc;
        long ccBreaks = 0;
        for (size_t i = 0; i + 188 <= ts3.size(); i += 188) {
            const uint8_t* p = &ts3[i];
            const int pid = (p[1] & 0x1f) << 8 | p[2], afc = p[3] >> 4 & 3, c = p[3] & 15;
            if (afc & 1) { auto it = cc.find(pid); if (it != cc.end() && c != ((it->second + 1) & 15)) ccBreaks++; cc[pid] = c; }
            if (pid == 0 && (p[1] & 0x40) && afc == 1) patVersions.insert(p[5 + p[4] + 5] >> 1 & 31);
        }
        MemIn m4{ts3.data(), ts3.size()};
        unsigned char* b4 = (unsigned char*)av_malloc(1 << 16);
        AVIOContext* io4 = avio_alloc_context(b4, 1 << 16, 0, &m4, memRead, nullptr, nullptr);
        AVFormatContext* c4 = avformat_alloc_context();
        c4->pb = io4;
        long nv = 0;
        if (avformat_open_input(&c4, "", av_find_input_format("mpegts"), nullptr) >= 0) {
            AVPacket* q = av_packet_alloc();
            while (av_read_frame(c4, q) >= 0) { if (c4->streams[q->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) nv++; av_packet_unref(q); }
            av_packet_free(&q);
            avformat_close_input(&c4);
        }
        av_freep(&io4->buffer);
        avio_context_free(&io4);
        printf("  late AC-4 init without the library: %d streams, %zu PAT version(s), %ld continuity breaks, %ld of %ld video packets\n", r3.streamCount(), patVersions.size(), ccBreaks, nv, vin);
        CHECK(r3.streamCount() == 1 && patVersions.size() == 1 && ccBreaks == 0, "late AC-4 without the library: the output is not started again");
        CHECK(nv == vin, "late AC-4 without the library: every picture");
    }
    // A media segment damaged by a loss on air (cut short, or the start of it lost) among good ones: the picture goes on from the next good
    // segment. Before, such a segment stopped the component for good: a moof without its mdat could end its demuxer (FFmpeg returned an error on one from the air;
    // on the segments of this file it goes over it), and a segment
    // whose mdat claims more bytes than there are made the demuxer read the following segments as the rest of that mdat, after which it never
    // found a box again and waited for more data in the read callback. The output (and with it the whole transport stream, when the
    // picture was its only stream) stopped while the segments went on arriving.
    {
        // 12 segments of one second: the three of the file, four times over, each round 3 s later, each with a segment type box in front (as
        // ROUTE delivers them)
        const uint32_t vts = timescaleOf(vinit);
        const uint8_t styp[24] = {0, 0, 0, 24, 's', 't', 'y', 'p', 'm', 's', 'd', 'h', 0, 0, 0, 0, 'm', 's', 'd', 'h', 'm', 's', 'i', 'x'};
        std::vector<std::vector<uint8_t>> segs;
        for (int round = 0; round < 4; round++)
            for (auto g : vseg) {
                size_t pos = 0, keep = g.size();   // up to the end of the mdat (the last one carries the file's mfra)
                while (pos + 8 <= g.size()) { const uint32_t sz = rd32(&g[pos]); if (sz < 8) break; if (!memcmp(&g[pos + 4], "mdat", 4)) { keep = pos + sz; break; } pos += sz; }
                g.resize(keep);
                if (round) shiftTfdt(g, vts, 3 * round);
                g.insert(g.begin(), styp, styp + sizeof styp);
                segs.push_back(g);
            }
        const long perSeg = vin / (long)vseg.size();
        // moof only: the bytes up to the end of the moof box
        auto moofOnly = [](std::vector<uint8_t> g) {
            for (size_t pos = 0; pos + 8 <= g.size();) { const uint32_t sz = rd32(&g[pos]); if (sz < 8) break; if (!memcmp(&g[pos + 4], "moof", 4)) { g.resize(pos + sz); break; } pos += sz; }
            return g;
        };
        // a base data offset far ahead in the track fragment header: the boxes add up, but the demuxer looks for the samples 64 MB on and
        // reads every segment that follows on the way there (the check of the segments cannot tell; the demuxer must be started again)
        auto farBase = [](std::vector<uint8_t> g) {
            size_t moof = 0, traf = 0;
            for (size_t i = 4; i + 12 < g.size(); i++) {
                if (!memcmp(&g[i], "moof", 4)) moof = i - 4;
                if (!memcmp(&g[i], "traf", 4)) traf = i - 4;
                if (memcmp(&g[i], "tfhd", 4)) continue;
                const size_t box = i - 4;
                uint8_t* f = &g[i + 4];
                const uint32_t fl = rd32(f) | 1;
                f[0] = 0; f[1] = (uint8_t)(fl >> 16); f[2] = (uint8_t)(fl >> 8); f[3] = (uint8_t)fl;
                const uint64_t base = 64ull << 20;
                std::vector<uint8_t> b(8);
                for (int k = 0; k < 8; k++) b[k] = (uint8_t)(base >> (56 - 8 * k));
                g.insert(g.begin() + i + 12, b.begin(), b.end());   // after version/flags and track_ID
                for (size_t at : {box, traf, moof}) { uint32_t s = rd32(&g[at]) + 8; g[at] = s >> 24; g[at + 1] = s >> 16; g[at + 2] = s >> 8; g[at + 3] = s; }
                break;
            }
            return g;
        };
        struct Case { const char* name; std::vector<uint8_t> bad; long minVideo; };
        std::vector<Case> cases = {
            {"moof without its mdat", moofOnly(segs[4]), (long)(segs.size() - 1) * perSeg},
            {"first half of a segment", std::vector<uint8_t>(segs[4].begin(), segs[4].begin() + segs[4].size() / 2), (long)(segs.size() - 1) * perSeg},
            {"second half of a segment", std::vector<uint8_t>(segs[4].begin() + segs[4].size() / 2, segs[4].end()), (long)(segs.size() - 1) * perSeg},
            {"samples 64 MB ahead", farBase(segs[4]), (long)(segs.size() - 5) * perSeg},
        };
        for (auto& cs : cases) {
            Atsc3Remux r5;
            r5.setStartupWaitMs(200);
            std::vector<uint8_t> ts5;
            std::mutex m5;
            std::thread rd([&] {
                uint8_t buf[8192];
                for (;;) { int k = r5.read(buf, sizeof buf); if (k <= 0) break; std::lock_guard<std::mutex> l(m5); ts5.insert(ts5.end(), buf, buf + k); }
            });
            r5.push(1, vinit);
            for (size_t i = 0; i < segs.size(); i++) {
                r5.push(1, i == 4 ? cs.bad : segs[i]);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            // what came out while the input is still open (stop() would end the input and let a stuck demuxer finish)
            size_t before = 0;
            for (int i = 0, same = 0; i < 200 && same < 10; i++) {   // until nothing more for half a second
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                size_t a; { std::lock_guard<std::mutex> l(m5); a = ts5.size(); }
                same = a == before && a > 0 ? same + 1 : 0;
                before = a;
            }
            std::vector<uint8_t> early;
            { std::lock_guard<std::mutex> l(m5); early = ts5; }
            r5.stop();
            rd.join();
            long nv = 0;
            int64_t vl = -1;
            MemIn m6{early.data(), early.size()};
            unsigned char* b6 = (unsigned char*)av_malloc(1 << 16);
            AVIOContext* io6 = avio_alloc_context(b6, 1 << 16, 0, &m6, memRead, nullptr, nullptr);
            AVFormatContext* c6 = avformat_alloc_context();
            c6->pb = io6;
            if (!early.empty() && avformat_open_input(&c6, "", av_find_input_format("mpegts"), nullptr) >= 0) {
                AVPacket* q = av_packet_alloc();
                while (av_read_frame(c6, q) >= 0) {
                    AVStream* st = c6->streams[q->stream_index];
                    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { nv++; if (q->pts != AV_NOPTS_VALUE) vl = av_rescale_q(q->pts, st->time_base, AVRational{1, 1000}); }
                    av_packet_unref(q);
                }
                av_packet_free(&q);
                avformat_close_input(&c6);
            } else avformat_free_context(c6);
            av_freep(&io6->buffer);
            avio_context_free(&io6);
            // the last picture of the 12 s is at 2 s (the output's start) + 12 s - one frame
            printf("  damaged segment 5 of 12 (%s): %ld of %ld video packets before the input ended, the last at %lld ms\n", cs.name, nv, (long)segs.size() * perSeg, (long long)vl);
            std::array<char, 160> msg;
            snprintf(msg.data(), msg.size(), "damaged segment (%s): the picture goes on after it", cs.name);
            CHECK(nv >= cs.minVideo && vl >= 13000, msg.data());
        }
    }
    printf(fails ? "atsc3 remux: FAILED\n" : "atsc3 remux: ok\n");
    return fails ? 1 : 0;
}
