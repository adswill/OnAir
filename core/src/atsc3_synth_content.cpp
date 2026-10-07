#include "atsc3_synth_content.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace dect2 {
namespace atsc3synth {

namespace {

constexpr int kW = 640, kH = 360, kFps = 25, kSlots = 8;          // the loop: 8 slots of one second
constexpr int kGop = kFps;                                          // a keyframe at the start of every slot
constexpr int kVFrames = kSlots * kFps;                             // 200 pictures
constexpr int kARate = 48000, kAFrame = 1024, kAFrames = kSlots * kARate / kAFrame;   // 375 AAC frames in 8 s: an exact number

// ---------------------------------------------------------------- the test card
// Colour bars on top, a sweeping ramp, a checkerboard that moves with a bouncing ball, and the number of the picture as eight blocks (binary), so that
// a frozen or skipping picture is obvious. Everything repeats exactly after kVFrames pictures.
void drawCard(AVFrame* f, int n) {
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34}, {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    const double ph = 2 * M_PI * n / kVFrames;
    const int bx = (int)((0.5 + 0.40 * std::sin(ph * 2)) * kW), by = (int)(kH * 0.64 + std::sin(ph * 5) * kH * 0.15);
    for (int y = 0; y < kH; y++) {
        uint8_t* row = f->data[0] + y * f->linesize[0];
        const bool chroma = !(y & 1);
        uint8_t* ru = f->data[1] + (y / 2) * f->linesize[1];
        uint8_t* rv = f->data[2] + (y / 2) * f->linesize[2];
        for (int x = 0; x < kW; x++) {
            int Y, U, V;
            if (y < kH * 0.36) { const auto& b = bars[x * 8 / kW]; Y = b[0]; U = b[1]; V = b[2]; }
            else if (y < kH * 0.42) { Y = 16 + ((x + n * 8) % kW) * 219 / kW; U = V = 128; }
            else {
                const bool sq = (((x + n * 2) / 40) + (y / 40)) & 1;
                Y = sq ? 70 : 52; U = sq ? 134 : 128; V = sq ? 122 : 128;
                const int dx = x - bx, dy = y - by;
                if (dx * dx + dy * dy < 46 * 46) { Y = 210; U = 90; V = 170; }
                if (dx * dx + dy * dy < 14 * 14) { Y = 235; U = 128; V = 128; }
            }
            row[x] = (uint8_t)Y;
            if (chroma && !(x & 1)) { ru[x / 2] = (uint8_t)U; rv[x / 2] = (uint8_t)V; }
        }
    }
    // progress bar of the second and the picture number in binary (eight blocks of 16 x 16)
    const int w = (n % kFps) * (kW - 40) / kFps;
    for (int y = kH - 22; y < kH - 12; y++) for (int x = 20; x < 20 + w; x++) f->data[0][y * f->linesize[0] + x] = 235;
    for (int b = 0; b < 8; b++)
        for (int y = kH - 40; y < kH - 24; y++)
            for (int x = kW - 20 - (b + 1) * 18; x < kW - 20 - (b + 1) * 18 + 16; x++) f->data[0][y * f->linesize[0] + x] = ((n >> b) & 1) ? 235 : 20;
}

// ---------------------------------------------------------------- box helpers
uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }

// the first box of this type between begin and end: its start and size
bool findBox(const std::vector<uint8_t>& d, size_t begin, size_t end, const char* type, size_t& pos, size_t& size) {
    end = std::min(end, d.size());
    while (begin + 8 <= end) {
        const size_t sz = rd32(&d[begin]);
        if (sz < 8 || begin + sz > end) return false;
        if (!memcmp(&d[begin + 4], type, 4)) { pos = begin; size = sz; return true; }
        begin += sz;
    }
    return false;
}

// the box at moov/trak/mdia/mdhd gives the time scale of the track
uint32_t trackTimescale(const std::vector<uint8_t>& init) {
    size_t p, s;
    if (!findBox(init, 0, init.size(), "moov", p, s)) return 0;
    size_t tp, ts, mp, ms, hp, hs;
    if (!findBox(init, p + 8, p + s, "trak", tp, ts)) return 0;
    if (!findBox(init, tp + 8, tp + ts, "mdia", mp, ms)) return 0;
    if (!findBox(init, mp + 8, mp + ms, "mdhd", hp, hs)) return 0;
    const int ver = init[hp + 8];
    const size_t off = hp + 8 + 4 + (ver == 1 ? 16 : 8);
    return off + 4 <= init.size() ? rd32(&init[off]) : 0;
}

// Splits what the muxer wrote: ftyp + moov as the init segment, and every moof + mdat pair as a fragment (other boxes, such as an mfra at the end, are left out).
bool splitFragments(const uint8_t* d, size_t n, std::vector<uint8_t>& init, std::vector<std::vector<uint8_t>>& segs) {
    size_t pos = 0;
    while (pos + 8 <= n) {
        const size_t sz = rd32(d + pos);
        if (sz < 8 || pos + sz > n) break;
        const char* t = (const char*)d + pos + 4;
        if (!memcmp(t, "ftyp", 4) || !memcmp(t, "moov", 4)) init.insert(init.end(), d + pos, d + pos + sz);
        else if (!memcmp(t, "moof", 4)) segs.emplace_back(d + pos, d + pos + sz);
        else if (!memcmp(t, "mdat", 4) && !segs.empty()) segs.back().insert(segs.back().end(), d + pos, d + pos + sz);
        pos += sz;
    }
    return !init.empty() && !segs.empty();
}

// ---------------------------------------------------------------- muxing
struct Track {
    std::vector<uint8_t> init;
    std::vector<std::vector<uint8_t>> seg;
    uint32_t timescale = 0;
    size_t bytes = 0;
};

// Writes the packets as a fragmented MP4 in memory, `perSlot[i]` packets in fragment i. The packets carry times in `tb`.
bool muxFragments(const AVCodecParameters* par, AVRational tb, const std::vector<AVPacket*>& pk, const std::vector<int>& perSlot, bool hvc1, Track& out) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mp4", nullptr) < 0 || !oc) return false;
    if (avio_open_dyn_buf(&oc->pb) < 0) { avformat_free_context(oc); return false; }
    bool ok = false;
    AVDictionary* opt = nullptr;
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (st && avcodec_parameters_copy(st->codecpar, par) >= 0) {
        st->codecpar->codec_tag = hvc1 ? MKTAG('h', 'v', 'c', '1') : 0;
        st->codecpar->initial_padding = 0;
        st->time_base = tb;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) st->avg_frame_rate = AVRational{kFps, 1};
        av_dict_set(&opt, "movflags", "empty_moov+default_base_moof+frag_custom", 0);
        if (avformat_write_header(oc, &opt) >= 0) {
            ok = true;
            size_t i = 0;
            for (size_t s = 0; s < perSlot.size() && ok; s++) {
                for (int k = 0; k < perSlot[s] && i < pk.size(); k++, i++) {
                    AVPacket* c = av_packet_clone(pk[i]);
                    c->stream_index = 0;
                    av_packet_rescale_ts(c, tb, st->time_base);
                    ok = av_write_frame(oc, c) >= 0;
                    av_packet_free(&c);
                    if (!ok) break;
                }
                if (ok) ok = av_write_frame(oc, nullptr) >= 0;   // ends the fragment here
            }
            av_write_trailer(oc);
        }
    }
    av_dict_free(&opt);
    uint8_t* buf = nullptr;
    const int sz = avio_close_dyn_buf(oc->pb, &buf);
    oc->pb = nullptr;
    if (ok && sz > 0) {
        out = Track();
        ok = splitFragments(buf, (size_t)sz, out.init, out.seg) && out.seg.size() == perSlot.size();
        out.timescale = ok ? trackTimescale(out.init) : 0;
        ok = ok && out.timescale > 0;
        for (auto& s : out.seg) out.bytes += s.size();
    } else ok = false;
    av_free(buf);
    avformat_free_context(oc);
    return ok;
}

// ---------------------------------------------------------------- video
struct VideoTry { const char* encoder; int codecPref; };   // codecPref: 1 HEVC, 2 H.264, 3 MPEG-2

bool encodeVideo(const VideoTry& vt, int kbps, Track& out, std::string& codecName) {
    const AVCodec* c = avcodec_find_encoder_by_name(vt.encoder);
    if (!c) return false;
    AVCodecContext* cx = avcodec_alloc_context3(c);
    if (!cx) return false;
    cx->width = kW; cx->height = kH; cx->pix_fmt = AV_PIX_FMT_YUV420P;
    cx->time_base = AVRational{1, kFps}; cx->framerate = AVRational{kFps, 1};
    cx->gop_size = kGop; cx->max_b_frames = 0;
    cx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    const int bps = std::max(50, kbps) * 1000;
    cx->bit_rate = bps; cx->rc_max_rate = bps * 3 / 2; cx->rc_buffer_size = bps;
    AVDictionary* opt = nullptr;
    const std::string name = vt.encoder;
    if (name == "libx265") {
        av_dict_set(&opt, "preset", "ultrafast", 0);
        av_dict_set(&opt, "x265-params", "keyint=25:min-keyint=25:scenecut=0:bframes=0:open-gop=0:log-level=none:frame-threads=1:pools=1", 0);
    } else if (name == "libx264") {
        av_dict_set(&opt, "preset", "veryfast", 0);
        av_dict_set(&opt, "x264-params", "keyint=25:min-keyint=25:scenecut=0:bframes=0:open-gop=0:threads=1:rc-lookahead=0", 0);
    } else if (name == "mpeg2video") {
        cx->bit_rate = bps; cx->rc_max_rate = bps; cx->rc_buffer_size = std::max(bps, 400000);
        cx->rc_min_rate = 0;
        cx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    bool ok = avcodec_open2(cx, c, &opt) >= 0;
    av_dict_free(&opt);
    std::vector<AVPacket*> pk;
    if (ok) {
        AVFrame* f = av_frame_alloc();
        f->format = cx->pix_fmt; f->width = kW; f->height = kH;
        av_frame_get_buffer(f, 0);
        AVPacket* p = av_packet_alloc();
        auto drain = [&]() {
            while (avcodec_receive_packet(cx, p) >= 0) { pk.push_back(av_packet_clone(p)); av_packet_unref(p); }
        };
        for (int n = 0; n < kVFrames; n++) {
            av_frame_make_writable(f);
            drawCard(f, n);
            f->pts = n;
            if (n % kGop == 0) f->pict_type = AV_PICTURE_TYPE_I; else f->pict_type = AV_PICTURE_TYPE_NONE;
            if (avcodec_send_frame(cx, f) < 0) { ok = false; break; }
            drain();
        }
        if (ok) { avcodec_send_frame(cx, nullptr); drain(); }
        av_packet_free(&p);
        av_frame_free(&f);
    }
    // what the fragments need: every picture once, a keyframe at the start of every second, no reordering
    if (ok) {
        ok = (int)pk.size() == kVFrames;
        for (int i = 0; ok && i < (int)pk.size(); i++) {
            if (pk[i]->pts != i) ok = false;   // no reordering: the pictures come out in the order they went in
            if (i % kGop == 0 && !(pk[i]->flags & AV_PKT_FLAG_KEY)) ok = false;
        }
    }
    if (ok) {
        // timestamps from 0, one picture each
        for (int i = 0; i < (int)pk.size(); i++) { pk[i]->pts = pk[i]->dts = i; pk[i]->duration = 1; }
        const bool hevc = cx->codec_id == AV_CODEC_ID_HEVC;
        std::vector<int> perSlot(kSlots, kGop);
        AVCodecParameters* par = avcodec_parameters_alloc();
        avcodec_parameters_from_context(par, cx);
        ok = muxFragments(par, AVRational{1, kFps}, pk, perSlot, hevc, out);
        avcodec_parameters_free(&par);
        codecName = hevc ? "hevc" : cx->codec_id == AV_CODEC_ID_H264 ? "h264" : cx->codec_id == AV_CODEC_ID_MPEG2VIDEO ? "mpeg2video" : "video";
    }
    for (auto* p : pk) av_packet_free(&p);
    avcodec_free_context(&cx);
    return ok;
}

// ---------------------------------------------------------------- audio
// 1 kHz on the left and 3 kHz on the right. The tones have a whole number of cycles in the loop, and the packets are taken from the middle of a
// longer run (two loops) so that the first one does not start from silence: the loop is seamless.
bool encodeAudio(int bitrate, Track& out) {
    const AVCodec* c = avcodec_find_encoder_by_name("aac");
    if (!c) c = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!c) return false;
    AVCodecContext* cx = nullptr;
    bool ok = false;
    for (AVSampleFormat sf : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_FLT}) {
        avcodec_free_context(&cx);
        cx = avcodec_alloc_context3(c);
        cx->sample_rate = kARate; cx->bit_rate = bitrate; cx->time_base = AVRational{1, kARate};
        av_channel_layout_default(&cx->ch_layout, 2);
        cx->sample_fmt = sf;
        cx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(cx, c, nullptr) >= 0) { ok = true; break; }
    }
    if (!ok) { avcodec_free_context(&cx); return false; }
    const int frame = cx->frame_size > 0 ? cx->frame_size : kAFrame;
    ok = frame == kAFrame;
    std::vector<AVPacket*> pk;
    if (ok) {
        AVFrame* f = av_frame_alloc();
        f->format = cx->sample_fmt; f->nb_samples = frame; f->sample_rate = kARate;
        av_channel_layout_copy(&f->ch_layout, &cx->ch_layout);
        av_frame_get_buffer(f, 0);
        AVPacket* p = av_packet_alloc();
        auto drain = [&]() { while (avcodec_receive_packet(cx, p) >= 0) { pk.push_back(av_packet_clone(p)); av_packet_unref(p); } };
        const int total = 2 * kAFrames;
        for (int n = 0; n < total; n++) {
            av_frame_make_writable(f);
            for (int i = 0; i < frame; i++) {
                const long long k = (long long)n * frame + i;
                const double l = 0.25 * std::sin(2 * M_PI * 1000.0 * (double)(k % kARate) / kARate);
                const double r = 0.25 * std::sin(2 * M_PI * 3000.0 * (double)(k % kARate) / kARate);
                switch (cx->sample_fmt) {
                case AV_SAMPLE_FMT_FLTP: ((float*)f->data[0])[i] = (float)l; ((float*)f->data[1])[i] = (float)r; break;
                case AV_SAMPLE_FMT_FLT: ((float*)f->data[0])[2 * i] = (float)l; ((float*)f->data[0])[2 * i + 1] = (float)r; break;
                case AV_SAMPLE_FMT_S16P: ((int16_t*)f->data[0])[i] = (int16_t)(l * 32767); ((int16_t*)f->data[1])[i] = (int16_t)(r * 32767); break;
                default: ((int16_t*)f->data[0])[2 * i] = (int16_t)(l * 32767); ((int16_t*)f->data[0])[2 * i + 1] = (int16_t)(r * 32767); break;
                }
            }
            f->pts = (int64_t)n * frame;
            if (avcodec_send_frame(cx, f) < 0) { ok = false; break; }
            drain();
        }
        if (ok) { avcodec_send_frame(cx, nullptr); drain(); }
        av_packet_free(&p);
        av_frame_free(&f);
    }
    ok = ok && (int)pk.size() >= 2 * kAFrames;
    if (ok) {
        // the second loop; the encoder delay of one frame is ignored (the tones do not change)
        std::vector<AVPacket*> mid(pk.begin() + kAFrames, pk.begin() + 2 * kAFrames);
        for (int i = 0; i < kAFrames; i++) { mid[i]->pts = mid[i]->dts = (int64_t)i * frame; mid[i]->duration = frame; mid[i]->flags |= AV_PKT_FLAG_KEY; }
        std::vector<int> perSlot(kSlots);
        int prev = 0;
        for (int s = 0; s < kSlots; s++) {   // the AAC frames that start before the end of second s + 1
            const int upto = (int)std::lround((double)(s + 1) * kAFrames / kSlots);
            perSlot[s] = upto - prev; prev = upto;
        }
        AVCodecParameters* par = avcodec_parameters_alloc();
        avcodec_parameters_from_context(par, cx);
        ok = muxFragments(par, AVRational{1, kARate}, mid, perSlot, false, out);
        avcodec_parameters_free(&par);
    }
    for (auto* p : pk) av_packet_free(&p);
    avcodec_free_context(&cx);
    return ok;
}

} // namespace

std::shared_ptr<const Content> getContent(int codecPref, int videoKbps) {
    static std::mutex mu;
    static std::map<std::pair<int, int>, std::shared_ptr<const Content>> cache;
    std::lock_guard<std::mutex> lk(mu);
    const auto key = std::make_pair(codecPref, videoKbps);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    // the encoders to try, best first: HEVC like the broadcasts, then H.264, then MPEG-2 video (which every FFmpeg build of the project has)
    static const VideoTry all[] = {{"libx265", 1}, {"hevc_videotoolbox", 1}, {"hevc_nvenc", 1}, {"libx264", 2}, {"h264_videotoolbox", 2}, {"h264_nvenc", 2}, {"mpeg2video", 3}};
    struct QuietLog { int old; QuietLog() : old(av_log_get_level()) { av_log_set_level(AV_LOG_ERROR); } ~QuietLog() { av_log_set_level(old); } } quiet;   // the encoders chat at the end
    Track v, a;
    std::string enc, codec;
    bool ok = false;
    for (const auto& vt : all) {
        if (codecPref > 0 && vt.codecPref != codecPref) continue;
        const int kbps = vt.codecPref == 3 ? videoKbps * 2 : videoKbps;   // MPEG-2 needs about twice the rate for the same picture
        if (encodeVideo(vt, kbps, v, codec)) { enc = vt.encoder; ok = true; break; }
    }
    if (ok) ok = encodeAudio(96000, a);
    std::shared_ptr<const Content> res;
    if (ok) {
        auto c = std::make_shared<Content>();
        c->vinit = std::move(v.init); c->vseg = std::move(v.seg); c->vTimescale = v.timescale;
        c->ainit = std::move(a.init); c->aseg = std::move(a.seg); c->aTimescale = a.timescale;
        c->slots = kSlots;
        c->vLoopTicks = (int64_t)kSlots * v.timescale;
        c->aLoopTicks = (int64_t)kSlots * a.timescale;
        c->videoEncoder = enc; c->videoCodec = codec;
        c->videoBitrate = (double)v.bytes * 8.0 / kSlots;
        c->audioBitrate = (double)a.bytes * 8.0 / kSlots;
        res = c;
    }
    cache[key] = res;
    return res;
}

std::vector<uint8_t> shiftedFragment(const Content& c, bool video, int slot, uint64_t cycle) {
    std::vector<uint8_t> f = (video ? c.vseg : c.aseg)[(size_t)slot];
    if (cycle == 0) return f;
    const int64_t loop = video ? c.vLoopTicks : c.aLoopTicks;
    size_t mp, ms;
    if (!findBox(f, 0, f.size(), "moof", mp, ms)) return f;
    size_t hp, hs;
    if (findBox(f, mp + 8, mp + ms, "mfhd", hp, hs) && hs >= 16) wr32(&f[hp + 12], rd32(&f[hp + 12]) + (uint32_t)(cycle * (uint64_t)c.slots));
    size_t tp, ts;
    for (size_t pos = mp + 8; findBox(f, pos, mp + ms, "traf", tp, ts); pos = tp + ts) {
        size_t dp, ds;
        if (!findBox(f, tp + 8, tp + ts, "tfdt", dp, ds)) continue;
        const int ver = f[dp + 8];
        if (ver == 1 && ds >= 20) wr64(&f[dp + 12], rd64(&f[dp + 12]) + cycle * (uint64_t)loop);
        else if (ds >= 16) wr32(&f[dp + 12], rd32(&f[dp + 12]) + (uint32_t)(cycle * (uint64_t)loop));
    }
    return f;
}

} // namespace atsc3synth
} // namespace dect2
