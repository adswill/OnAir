// DMB video services in DAB (ETSI TS 102 427, TS 102 428): the test transmitter's DMB service against the receiver.
//   1. the H.264 writer of the generator: libavcodec decodes its pictures to exactly the test picture
//   2. the generator's transport stream: continuity counters and PCR across the 12 s loop, the tables within 500 ms
//   3. the outer code: RS(204,188) with the Forney interleaver, decoded by DmbDecoder back to the very packets sent, also with byte errors
//   4. the remux: MPEG-4 Systems (IOD, OD, SL packets) to a plain transport stream that TsDemux and FFmpeg read as H.264 + AAC;
//      every decoded picture equals the test picture of its time stamp, the sound has the two tones. Also what FFmpeg makes of the
//      DMB stream itself (reported, not checked)
//   5. over the air: the DAB signal with noise through DabReceiver (FIC signalling, sub-channel, outer code, remux) to decoded pictures
//   6. the engine as the app uses it: the built-in DAB test signal, the DMB service selected, pictures out of the player (muted)
#include "dect2/dab.h"
#include "dect2/dab_dmb.h"
#include "dect2/dab_gen.h"
#include "dect2/engine.h"
#include "dect2/ts.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
namespace dmb = dabgen::dmb;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ---------------------------------------------------------------- FFmpeg on a transport stream in memory
struct Decoded {
    std::map<int, std::string> streams;          // PID -> codec
    struct Pic { int64_t pts; int w, h; std::vector<uint8_t> y, u, v; };
    std::vector<Pic> pics;
    std::vector<float> ch[2];
    int rate = 0, videoPackets = 0, audioPackets = 0;
};
struct Mem { const std::vector<uint8_t>* d; size_t pos = 0; };
static int readMem(void* o, uint8_t* buf, int n) {
    Mem* m = (Mem*)o;
    const size_t k = std::min((size_t)n, m->d->size() - m->pos);
    if (!k) return AVERROR_EOF;
    memcpy(buf, m->d->data() + m->pos, k);
    m->pos += k;
    return (int)k;
}
static bool decodeTs(const std::vector<uint8_t>& ts, Decoded& out, bool decode) {
    Mem m{&ts};
    AVFormatContext* fmt = avformat_alloc_context();
    uint8_t* iob = (uint8_t*)av_malloc(4096);
    fmt->pb = avio_alloc_context(iob, 4096, 0, &m, readMem, nullptr, nullptr);
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    bool ok = avformat_open_input(&fmt, "", av_find_input_format("mpegts"), nullptr) >= 0;
    if (!ok) { av_freep(&iob); return false; }
    avformat_find_stream_info(fmt, nullptr);
    AVCodecContext* dec[64] = {};
    for (unsigned i = 0; i < fmt->nb_streams && i < 64; i++) {
        AVStream* s = fmt->streams[i];
        out.streams[s->id] = avcodec_get_name(s->codecpar->codec_id);
        if (!decode || (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)) continue;
        const AVCodec* c = avcodec_find_decoder(s->codecpar->codec_id);
        if (!c) continue;
        dec[i] = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec[i], s->codecpar);
        if (avcodec_open2(dec[i], c, nullptr) < 0) avcodec_free_context(&dec[i]);
    }
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    auto drain = [&](unsigned i) {
        while (avcodec_receive_frame(dec[i], fr) == 0) {
            if (dec[i]->codec_type == AVMEDIA_TYPE_VIDEO) {
                Decoded::Pic p;
                p.pts = fr->best_effort_timestamp; p.w = fr->width; p.h = fr->height;
                for (int r = 0; r < p.h; r++) p.y.insert(p.y.end(), fr->data[0] + r * fr->linesize[0], fr->data[0] + r * fr->linesize[0] + p.w);
                for (int r = 0; r < p.h / 2; r++) {
                    p.u.insert(p.u.end(), fr->data[1] + r * fr->linesize[1], fr->data[1] + r * fr->linesize[1] + p.w / 2);
                    p.v.insert(p.v.end(), fr->data[2] + r * fr->linesize[2], fr->data[2] + r * fr->linesize[2] + p.w / 2);
                }
                out.pics.push_back(std::move(p));
            } else if (fr->format == AV_SAMPLE_FMT_FLTP) {
                out.rate = fr->sample_rate;
                for (int c = 0; c < std::min(2, fr->ch_layout.nb_channels); c++) out.ch[c].insert(out.ch[c].end(), (float*)fr->data[c], (float*)fr->data[c] + fr->nb_samples);
            }
            av_frame_unref(fr);
        }
    };
    while (av_read_frame(fmt, pk) >= 0) {
        const unsigned i = (unsigned)pk->stream_index;
        if (i < 64) {
            const auto t = fmt->streams[i]->codecpar->codec_type;
            if (t == AVMEDIA_TYPE_VIDEO) out.videoPackets++;
            if (t == AVMEDIA_TYPE_AUDIO) out.audioPackets++;
            if (dec[i] && avcodec_send_packet(dec[i], pk) >= 0) drain(i);
        }
        av_packet_unref(pk);
    }
    for (unsigned i = 0; i < 64; i++) if (dec[i]) { avcodec_send_packet(dec[i], nullptr); drain(i); avcodec_free_context(&dec[i]); }
    av_packet_free(&pk);
    av_frame_free(&fr);
    AVIOContext* pb = fmt->pb;
    avformat_close_input(&fmt);
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    return true;
}

// the pictures that equal the test picture of their time stamp (composition time = 0.7 s + n * 80 ms of the loop)
static int matchingPictures(const Decoded& d, int& wrongStamp) {
    int good = 0;
    wrongStamp = 0;
    std::vector<uint8_t> y, u, v;
    for (const auto& p : d.pics) {
        const int64_t t = p.pts - 63000;
        if (t % 7200) { wrongStamp++; continue; }
        const int64_t n = ((t / 7200) % dmb::kFrames + dmb::kFrames) % dmb::kFrames;
        dmb::testPicture(n, y, u, v);
        if (p.w == dmb::kWidth && p.h == dmb::kHeight && p.y == y && p.u == u && p.v == v) good++;
    }
    return good;
}

// power of the tone `hz` against the whole signal (Goertzel)
static double toneShare(const std::vector<float>& x, size_t from, int rate, double hz) {
    if (x.size() < from + 4096) return 0;
    const size_t n = std::min<size_t>(x.size() - from, 32000);
    const double w = 2 * M_PI * hz / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0, e = 0;
    for (size_t i = 0; i < n; i++) { const double s0 = x[from + i] + c * s1 - s2; s2 = s1; s1 = s0; e += (double)x[from + i] * x[from + i]; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return e > 0 ? 2 * p / (n * e) : 0;
}

static void checkTones(const Decoded& d, const char* what) {
    const double l = toneShare(d.ch[0], 8192, d.rate, 1000), r = toneShare(d.ch[1], 8192, d.rate, 500);
    printf("  %s: sound %d Hz, %zu samples, 1 kHz holds %.0f%% of the left channel, 500 Hz %.0f%% of the right\n", what, d.rate, d.ch[0].size(), 100 * l, 100 * r);
    CHECK(d.rate == dmb::kAudioRate && l > 0.9 && r > 0.9, "%s: the tones (rate %d, %.2f %.2f)", what, d.rate, l, r);
}

static void checkPlainTs(const std::vector<uint8_t>& ts, const char* what) {
    TsDemux dm;
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) dm.feed(&ts[i]);
    dm.advance(1.0);
    const TsSnapshot sn = dm.snapshot();
    bool pmt = false, video = false, audio = false;
    for (const auto& s : sn.services)
        if (s.id == DmbRemux::kProgram) {
            pmt = s.havePmt;
            for (const auto& e : s.streams) { video |= e.pid == DmbRemux::kVideoPid && e.codec == "H.264"; audio |= e.pid == DmbRemux::kAudioPid && e.codec == "AAC"; }
        }
    printf("  %s: TsDemux: %zu services, program %d PMT %s, H.264 on PID 0x%X %s, AAC on 0x%X %s\n", what, sn.services.size(), DmbRemux::kProgram, pmt ? "yes" : "no", DmbRemux::kVideoPid,
           video ? "yes" : "no", DmbRemux::kAudioPid, audio ? "yes" : "no");
    CHECK(pmt && video && audio, "%s: PAT / PMT / streams of the plain transport stream", what);
}

// ---------------------------------------------------------------- 1. the H.264 writer
static void testVideoWriter() {
    const auto aus = dmb::encodeTestVideo();
    size_t bytes = 0;
    for (const auto& a : aus) bytes += a.size();
    printf("H.264 writer: %zu pictures, %.0f kbit/s, IDR %zu bytes, P %zu bytes\n", aus.size(), bytes * 8 / 12.0 / 1000, aus[0].size(), aus[1].size());
    // through libavcodec directly: two loops, the second one decodes from the first one's references
    const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(c);
    CHECK(avcodec_open2(ctx, c, nullptr) >= 0, "H.264 decoder");
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    int n = 0, good = 0, errors = 0;
    std::vector<uint8_t> y, u, v;
    for (int pass = 0; pass < 2; pass++)
        for (const auto& a : aus) {
            av_new_packet(pk, (int)a.size());
            memcpy(pk->data, a.data(), a.size());
            if (avcodec_send_packet(ctx, pk) < 0) errors++;
            av_packet_unref(pk);
            while (avcodec_receive_frame(ctx, fr) == 0) {
                dmb::testPicture(n, y, u, v);
                bool same = fr->width == dmb::kWidth && fr->height == dmb::kHeight && fr->format == AV_PIX_FMT_YUV420P;
                for (int r = 0; same && r < dmb::kHeight; r++) same = !memcmp(fr->data[0] + r * fr->linesize[0], &y[(size_t)r * dmb::kWidth], dmb::kWidth);
                for (int r = 0; same && r < dmb::kHeight / 2; r++)
                    same = !memcmp(fr->data[1] + r * fr->linesize[1], &u[(size_t)r * dmb::kWidth / 2], dmb::kWidth / 2) && !memcmp(fr->data[2] + r * fr->linesize[2], &v[(size_t)r * dmb::kWidth / 2], dmb::kWidth / 2);
                good += same;
                n++;
                av_frame_unref(fr);
            }
        }
    printf("  libavcodec: %d pictures, %d equal to the test picture, %d errors\n", n, good, errors);
    CHECK(n >= 2 * dmb::kFrames - 2 && good == n && errors == 0, "H.264 writer: %d of %d pictures exact", good, n);
    av_packet_free(&pk); av_frame_free(&fr); avcodec_free_context(&ctx);
}

// ---------------------------------------------------------------- 2. the transport stream of the generator
static void testSource() {
    dmb::Source s(408);
    CHECK(s.ok() && s.packetsPerLoop() == 3000 && s.hasAudio(), "source: ok %d, %d packets per loop, audio %d", s.ok(), s.packetsPerLoop(), s.hasAudio());
    CHECK(!dmb::Source(400).ok(), "408 kbit/s works, 400 kbit/s does not hold whole packets in the loop");
    const int P = s.packetsPerLoop();
    std::map<int, int> cc;
    int ccErr = 0, nulls = 0, pcrBad = 0, pcrs = 0;
    int64_t lastPcr = -1, lastPcrK = 0, lastPat = 0, worstPat = 0;
    for (int64_t k = -P; k < 2 * P; k++) {   // over two loop wraps
        uint8_t p[188];
        s.tsPacket(k, p);
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == 0x1FFF) { nulls++; continue; }
        if (pid == 0 && (p[1] & 0x40)) { worstPat = std::max(worstPat, k - lastPat); lastPat = k; }
        if (p[3] & 0x10) {
            if (cc.count(pid) && ((cc[pid] + 1) & 15) != (p[3] & 15)) ccErr++;
            cc[pid] = p[3] & 15;
        }
        if ((p[3] & 0x20) && p[4] >= 7 && (p[5] & 0x10)) {
            const int64_t base = (int64_t)p[6] << 25 | (int64_t)p[7] << 17 | (int64_t)p[8] << 9 | (int64_t)p[9] << 1 | (p[10] >> 7);
            if (lastPcr >= 0 && (((base - lastPcr) & ((1ll << 33) - 1)) != (k - lastPcrK) * 360 || k - lastPcrK > 25)) { pcrBad++; if (pcrBad < 8) printf("  PCR at packet %lld: %lld after %lld packets\n", (long long)k, (long long)(base - lastPcr), (long long)(k - lastPcrK)); }   // 4 ms per packet (modulo 2^33), at most 100 ms apart
            lastPcr = base; lastPcrK = k; pcrs++;
        }
    }
    printf("transport stream: %d packets per 12 s (%.0f kbit/s), %.1f%% null packets, %d PCRs, tables every %.0f ms at most\n", P, P * 188 * 8 / 12.0 / 1000, 100.0 * nulls / (3 * P), pcrs, worstPat * 4.0);
    CHECK(ccErr == 0, "continuity counter errors: %d", ccErr);
    CHECK(pcrBad == 0 && pcrs > 3 * 120, "PCR: %d bad of %d", pcrBad, pcrs);
    CHECK(worstPat > 0 && worstPat * 4 <= 500, "PAT every %lld ms", (long long)worstPat * 4);
}

// ---------------------------------------------------------------- 3. the outer code
static void testOuter() {
    dmb::Source s(408);
    for (int errors : {0, 8}) {
        DmbDecoder d;
        std::vector<std::vector<uint8_t>> raw;
        d.setRawTap([&](const uint8_t* k) { raw.emplace_back(k, k + 188); });
        std::vector<uint8_t> f;
        for (int64_t i = 0; i < 300; i++) {
            s.logicalFrame(i, f);
            // byte errors: exactly `errors` in every code word. Byte o of the interleaved stream is byte r of code word K, where o - (o mod 12) * 204
            // = 204 K + r; (37 r + 11 K) mod 204 runs through every value once per code word
            for (size_t j = 0; j < f.size() && errors; j++) {
                const int64_t o = i * (int64_t)f.size() + (int64_t)j, in = o - (o % 12) * 204;
                const int64_t K = in >= 0 ? in / 204 : -((-in + 203) / 204), r = in - K * 204;
                if ((37 * r + 11 * ((K % 204) + 204)) % 204 < errors) f[j] ^= (uint8_t)(1 + (o * 7) % 255);
            }
            d.push(f.data(), (int)f.size());
        }
        const DmbStats st = d.stats();
        // the first packet out: which one of the stream it is, then all must follow in order
        int64_t k0 = -1;
        uint8_t p[188];
        for (int64_t k = 0; k < 64 && k0 < 0 && !raw.empty(); k++) { s.tsPacket(k, p); if (!memcmp(p, raw[0].data(), 188)) k0 = k; }
        int same = 0;
        for (size_t i = 0; i < raw.size() && k0 >= 0; i++) { s.tsPacket(k0 + (int64_t)i, p); same += !memcmp(p, raw[i].data(), 188); }
        printf("outer code, %s: sync %d, RS blocks %llu ok (%llu bytes fixed), %llu failed; %zu packets, the first is packet %lld, %d equal to the sent ones\n", errors ? "byte errors" : "clean", st.sync,
               (unsigned long long)st.rsOk, (unsigned long long)st.rsCorrected, (unsigned long long)st.rsFailed, raw.size(), (long long)k0, same);
        CHECK(st.sync && st.rsFailed == 0 && raw.size() > 1700 && k0 >= 0 && same == (int)raw.size(), "outer code (%d errors): %d of %zu packets", errors, same, raw.size());
        if (errors) CHECK(st.rsCorrected >= (uint64_t)errors * st.rsOk - 8 * 12, "byte errors fixed: %llu", (unsigned long long)st.rsCorrected);
    }
}

// ---------------------------------------------------------------- 4. the remux
static void testRemux() {
    dmb::Source s(408);
    std::vector<uint8_t> dmbTs, plain;
    DmbRemux r;
    r.setSink([&](const uint8_t* k) { plain.insert(plain.end(), k, k + 188); });
    for (int64_t k = 0; k < 5 * s.packetsPerLoop() / 2; k++) {
        uint8_t p[188];
        s.tsPacket(k, p);
        dmbTs.insert(dmbTs.end(), p, p + 188);
        r.feed(p);
    }
    DmbStats st;
    r.stats(st);
    printf("remux: %zu DMB packets in, %llu out, video %s, sound %s %d Hz %d ch, note \"%s\"\n", dmbTs.size() / 188, (unsigned long long)st.tsOut, st.video.c_str(), st.audio.c_str(), st.sampleRate,
           st.channels, st.note.c_str());
    CHECK(st.video == "H.264" && st.audio == "AAC-LC" && st.sampleRate == 32000 && st.channels == 2 && st.note.empty(), "remux: stream description");
    checkPlainTs(plain, "remux");
    Decoded d;
    CHECK(decodeTs(plain, d, true), "FFmpeg opens the remuxed stream");
    int wrong = 0;
    const int good = matchingPictures(d, wrong);
    printf("  remux: FFmpeg reads %s on 0x%X and %s on 0x%X; %zu pictures decoded, %d equal to the test picture of their time stamp, %d with another time stamp\n",
           d.streams[DmbRemux::kVideoPid].c_str(), DmbRemux::kVideoPid, d.streams[DmbRemux::kAudioPid].c_str(), DmbRemux::kAudioPid, d.pics.size(), good, wrong);
    CHECK(d.streams[DmbRemux::kVideoPid] == "h264" && d.streams[DmbRemux::kAudioPid] == "aac", "codecs of the plain stream");
    CHECK(good > 2 * dmb::kFrames && good == (int)d.pics.size(), "pictures: %d of %zu", good, d.pics.size());
    checkTones(d, "remux");
    // what FFmpeg makes of the DMB transport stream itself (MPEG-4 SL): reported only
    Decoded raw;
    const int old = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    decodeTs(dmbTs, raw, false);
    av_log_set_level(old);
    std::string list;
    for (const auto& kv : raw.streams) { char b[64]; snprintf(b, sizeof b, " 0x%X %s", kv.first, kv.second.c_str()); list += b; }
    printf("  FFmpeg %s on the DMB stream itself:%s; packets: video %d, audio %d (the object descriptors in the sections are not read, so the sound has no codec)\n",
           av_version_info(), list.c_str(), raw.videoPackets, raw.audioPackets);
}

// the same stream with BSAC sound (audio object type 22 in the object descriptor): the video plays alone, the access units reach the BSAC hook
static void testBsac() {
    dmb::Source s(408);
    std::vector<uint8_t> plain;
    DmbRemux r;
    int hook = 0;
    r.setSink([&](const uint8_t* k) { plain.insert(plain.end(), k, k + 188); });
    r.setBsacSink([&](const std::vector<uint8_t>& asc, const uint8_t*, int n, int64_t pts) { hook += asc.size() >= 2 && (asc[0] >> 3) == 22 && n > 0 && pts >= 0; });
    const std::vector<uint8_t>& asc = s.audioSpecificConfig();
    int patched = 0;
    for (int64_t k = 0; k < 3000; k++) {
        uint8_t p[188];
        s.tsPacket(k, p);
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == dmb::kPidOd && (p[1] & 0x40)) {   // the decoder specific info of the sound: AAC-LC -> ER BSAC, same rate and channels; new CRC
            const int off = 4 + ((p[3] & 0x20) ? 1 + p[4] : 0);
            uint8_t* sec = p + off + 1 + p[off];
            const int len = 3 + (((sec[1] & 15) << 8) | sec[2]);
            for (int i = 0; i + 3 < len - 4; i++)
                if (sec[i] == 0x05 && sec[i + 1] == asc.size() && sec[i + 2] == asc[0] && sec[i + 3] == asc[1]) {
                    sec[i + 2] = (uint8_t)((22 << 3) | (asc[0] & 7));
                    patched++;
                }
            const uint32_t crc = mpegCrc32(sec, len - 4);
            for (int b = 0; b < 4; b++) sec[len - 4 + b] = (uint8_t)(crc >> (24 - 8 * b));
        }
        r.feed(p);
    }
    DmbStats st;
    r.stats(st);
    Decoded d;
    decodeTs(plain, d, true);
    int wrong = 0;
    const int good = matchingPictures(d, wrong);
    printf("BSAC sound: %d object descriptors changed; sound \"%s\", note \"%s\", %d access units at the hook, streams of the plain stream %zu, %d pictures exact\n", patched, st.audio.c_str(), st.note.c_str(), hook,
           d.streams.size(), good);
    CHECK(patched >= 7 && st.audio == "BSAC" && st.note == "BSAC sound (Korea) is not supported" && hook > 300, "BSAC: description and hook");
    CHECK(d.streams.size() == 1 && d.streams[DmbRemux::kVideoPid] == "h264" && good > 100 && good == (int)d.pics.size(), "BSAC: the video alone");
}

// ---------------------------------------------------------------- 5. over the air
static void testAir(double snrDb, bool strict) {
    dabgen::TxConfig tc;
    tc.services = {dabgen::defaultServices()[0], dabgen::dmbService()};
    tc.utcSeconds = 1700000000;
    SynthConfig sc;
    sc.snrDb = snrDb;
    auto syn = makeDabSynth(tc, sc, 2.048e6);
    DabReceiver rx;
    rx.configure(2.048e6);
    rx.audio().setSilent(true);
    std::vector<uint8_t> plain;
    uint64_t raw = 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) { plain.insert(plain.end(), pk, pk + n * 188); });
    rx.dmb().setRawTap([&](const uint8_t*) { raw++; });
    rx.select(5);
    std::vector<cf32> x(65536);
    for (size_t done = 0; done < (size_t)(9.0 * 2.048e6); done += x.size()) {
        syn->generate(x.data(), x.size());
        for (auto& v : x) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);   // 8 bit like the source
        rx.feed(x.data(), x.size());
    }
    DabTelemetry t;
    rx.telemetry(t, 0);
    const DabEnsemble e = rx.ensemble();
    char what[48];
    snprintf(what, sizeof what, "C/N %.0f dB", snrDb);
    auto it = e.services.find(dabgen::dmbService().sid);
    const bool listed = it != e.services.end() && it->second.label == "OnAir TV" && it->second.dmb() && it->second.dmb()->subId == 5 && it->second.userApp == 0x009 && it->second.dmbProfile == 2;
    printf("over the air, %s: lock %d, SNR %.1f dB, \"OnAir TV\" listed as DMB %s; DMB sync %d, RS blocks %llu ok (%llu bytes fixed), %llu failed; %llu packets, %zu out\n", what, t.state == 2, t.snrDb,
           listed ? "yes" : "no", t.dmb.sync, (unsigned long long)t.dmb.rsOk, (unsigned long long)t.dmb.rsCorrected, (unsigned long long)t.dmb.rsFailed, (unsigned long long)raw, plain.size() / 188);
    if (!strict) return;
    CHECK(listed, "%s: the DMB service in the ensemble (FIG 0/2 DSCTy 24, FIG 0/13 DMB profile 2, FIG 1/5 label)", what);
    CHECK(t.dmb.active && t.dmb.sub == 5 && t.dmb.sync && t.dmb.rsOk > 2000 && t.dmb.rsFailed == 0, "%s: outer code", what);
    checkPlainTs(plain, what);
    Decoded d;
    decodeTs(plain, d, true);
    int wrong = 0;
    const int good = matchingPictures(d, wrong);
    printf("  %s: %zu pictures decoded, %d equal to the test picture of their time stamp\n", what, d.pics.size(), good);
    CHECK(good >= 60 && good == (int)d.pics.size(), "%s: pictures %d of %zu", what, good, d.pics.size());
    checkTones(d, what);
}

// ---------------------------------------------------------------- 6. the engine and the player
static void testEngine() {
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "Synthetic";
    TuneSettings tune;
    tune.sampleRate = 2.048e6;
    tune.centerHz = 225.648e6;
    tune.bandwidthMhz = 1.536;
    tune.synth.mode = 4;
    tune.synth.snrDb = 20;
    FileOptions fo;
    e.setStandard(4);
    e.player().setVolume(0);
    e.player().setMuted(true);
    CHECK(e.start(dev, tune, fo), "engine started");
    e.dabSelect(5);
    uint64_t vseq = 0;
    int pics = 0, exact = 0, w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
    const auto t0 = std::chrono::steady_clock::now();
    double firstAt = -1;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 25 && pics < 40) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto f = e.player().videoFrame(vseq);
        if (!f || f->w == 0) continue;
        if (firstAt < 0) firstAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        pics++; w = f->w; h = f->h;
        const int64_t t = std::llround(f->pts * 90000) - 63000;
        dmb::testPicture(((t / 7200) % dmb::kFrames + dmb::kFrames) % dmb::kFrames, y, u, v);
        int diff = 0;   // the luma plane; hardware decoders may round chroma conversions differently
        for (size_t i = 0; i < y.size() && i < f->y.size(); i++) diff += std::abs((int)f->y[i] - (int)y[i]) > 2;
        exact += f->y.size() == y.size() && diff < (int)y.size() / 100;
    }
    RxTelemetry t;
    e.latestRx(t, 0);
    const PlayerStats ps = e.player().stats();
    printf("engine: first picture after %.1f s, %d pictures %dx%d, %d like the test picture; player %s / %s %s, %s; DMB %s / %s, RS %llu ok %llu failed\n", firstAt, pics, w, h, exact,
           ps.videoCodec.c_str(), ps.audioCodec.c_str(), ps.hardware ? "(hardware)" : "(software)", ps.status.c_str(), t.dab.dmb.video.c_str(), t.dab.dmb.audio.c_str(),
           (unsigned long long)t.dab.dmb.rsOk, (unsigned long long)t.dab.dmb.rsFailed);
    e.stop();
    CHECK(pics >= 20 && w == dmb::kWidth && h == dmb::kHeight, "engine: %d pictures of %dx%d from the player", pics, w, h);
    CHECK(exact * 10 >= pics * 9, "engine: %d of %d pictures show the test picture of their time", exact, pics);
    CHECK(ps.hasVideo && ps.hasAudio && ps.videoCodec == "h264" && ps.audioCodec == "aac", "engine: player streams %s %s", ps.videoCodec.c_str(), ps.audioCodec.c_str());
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !strcmp(argv[1], "--quick");
    av_log_set_level(AV_LOG_ERROR);
    testVideoWriter();
    testSource();
    testOuter();
    testRemux();
    testBsac();
    testAir(12, true);
    testAir(5, false);
    if (!quick) testEngine();
    printf(fails ? "dab_dmb: %d FAILED\n" : "dab_dmb: all passed\n", fails.load());
    return fails ? 1 : 0;
}
