// MP4 fragments of a video and an audio component (as ROUTE delivers them) are remuxed into one MPEG-2 transport stream. The result is read
// back with libavformat: one HEVC and one AAC stream, with all frames, in step with each other.
//   test_atsc3_remux <video.mp4> <audio.mp4>      (the fragmented files in tests/data)
#include "dect2/atsc3_remux.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
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
    printf(fails ? "atsc3 remux: FAILED\n" : "atsc3 remux: ok\n");
    return fails ? 1 : 0;
}
