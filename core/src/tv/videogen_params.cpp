// Digital video generator: encoder list, capabilities, bitrate estimate and the saved settings (videogen_params.h).
#include "dect2/tv/videogen_params.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>
#include <thread>

namespace dect2::vgen {

namespace {

struct Candidate { const char* name; Codec codec; bool hw; };
// best first within a codec; the software encoders before the hardware ones of the same codec. h264_mf / hevc_mf are tried as the
// graphics card's encoder first, then as Windows' own software one (hevc_mf needs the HEVC Video Extensions from the Store).
const Candidate kVideo[] = {
    {"mpeg2video", Codec::Mpeg2, false},
    {"libx264", Codec::H264, false}, {"h264_nvenc", Codec::H264, true}, {"h264_qsv", Codec::H264, true}, {"h264_amf", Codec::H264, true},
    {"h264_videotoolbox", Codec::H264, true}, {"h264_mf", Codec::H264, true}, {"h264_vaapi", Codec::H264, true}, {"libopenh264", Codec::H264, false},
    {"libx265", Codec::Hevc, false}, {"hevc_nvenc", Codec::Hevc, true}, {"hevc_qsv", Codec::Hevc, true}, {"hevc_amf", Codec::Hevc, true},
    {"hevc_videotoolbox", Codec::Hevc, true}, {"hevc_mf", Codec::Hevc, true}, {"hevc_vaapi", Codec::Hevc, true},
};
struct ACandidate { const char* name; AudioCodec codec; };
const ACandidate kAudio[] = {{"aac", AudioCodec::Aac}, {"mp2", AudioCodec::Mp2}, {"ac3", AudioCodec::Ac3}};

bool hasPixFmt(const AVCodec* c, AVPixelFormat want) {
    const AVPixelFormat* f = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&f, nullptr) < 0) return false;
#else
    f = c->pix_fmts;
#endif
    if (!f) return true;   // not stated: anything
    for (; *f != AV_PIX_FMT_NONE; f++) if (*f == want) return true;
    return false;
}

std::string averr(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, b, sizeof b);
    return b;
}

bool isMf(const std::string& e) { return e == "h264_mf" || e == "hevc_mf"; }

// An encoder counts only when it encodes: a name in the build is not enough (nvenc without an NVIDIA card, QSV without an Intel GPU,
// VAAPI without frames on the GPU, a Media Foundation encoder this Windows lacks). A few black pictures go through it, set up as the
// start sets it up (setupVideoEncoder, the second-try way: the start drops the profile and B-frames itself when they are refused).
// Empty when it works, else why not.
std::string tryVideo(const AVCodec* c, bool hwMode) {
    constexpr int W = 320, H = 240;
    Settings s;
    s.videoEncoder = c->name; s.width = W; s.height = H; s.fps = 25; s.autoBitrate = false; s.videoKbps = 500;
    AVCodecContext* cx = avcodec_alloc_context3(c);
    if (!cx) return "out of memory";
    AVDictionary* o = nullptr;
    std::string why;
    if (!setupVideoEncoder(cx, &o, c, c->name, hwMode, s, W, H, 25, 1, false)) why = "takes no YUV 4:2:0 or NV12 pictures from memory (it wants frames on the GPU)";
    if (why.empty() && strcmp(c->name, "libx265") == 0) av_dict_set(&o, "x265-params", "log-level=none", 0);
    int r = 0;
    if (why.empty() && (r = avcodec_open2(cx, c, &o)) < 0) why = "does not open: " + averr(r);
    av_dict_free(&o);
    AVFrame* f = av_frame_alloc();
    AVPacket* pk = av_packet_alloc();
    if (why.empty() && (!f || !pk)) why = "out of memory";
    if (why.empty()) {
        f->format = cx->pix_fmt; f->width = W; f->height = H;
        if (av_frame_get_buffer(f, 0) < 0) why = "out of memory";
    }
    if (why.empty()) {   // black: Y 16, U and V 128 (NV12: U and V side by side on one plane)
        for (int y = 0; y < H; y++) memset(f->data[0] + (size_t)y * f->linesize[0], 16, W);
        for (int y = 0; y < H / 2; y++) {
            if (cx->pix_fmt == AV_PIX_FMT_NV12) memset(f->data[1] + (size_t)y * f->linesize[1], 128, W);
            else { memset(f->data[1] + (size_t)y * f->linesize[1], 128, W / 2); memset(f->data[2] + (size_t)y * f->linesize[2], 128, W / 2); }
        }
    }
    int packets = 0;
    const auto count = [&packets](AVPacket*) { packets++; };
    for (int n = 0; why.empty() && n < 3; n++) {
        f->pts = n;
        if ((r = sendToEncoder(cx, f, pk, count)) < 0) why = "does not encode: " + averr(r);
    }
    if (why.empty() && (r = sendToEncoder(cx, nullptr, pk, count)) < 0) why = "does not finish: " + averr(r);
    if (why.empty() && packets == 0) why = "gives no pictures out";
    av_packet_free(&pk);
    av_frame_free(&f);
    avcodec_free_context(&cx);
    return why;
}

bool audioOpens(const AVCodec* c) {
    const AVSampleFormat* f = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void**)&f, nullptr) < 0) return false;
#else
    f = c->sample_fmts;
#endif
    AVCodecContext* cx = avcodec_alloc_context3(c);
    if (!cx) return false;
    cx->sample_fmt = f ? f[0] : AV_SAMPLE_FMT_FLTP;
    cx->sample_rate = 48000; cx->bit_rate = 96000;
    av_channel_layout_default(&cx->ch_layout, 1);
    cx->time_base = AVRational{1, 48000};
    const bool ok = avcodec_open2(cx, c, nullptr) >= 0;
    avcodec_free_context(&cx);
    return ok;
}

bool starts(std::string_view s, std::string_view p) { return s.starts_with(p); }

std::string kbps(double v) {
    char b[48];
    if (v >= 10000) snprintf(b, sizeof b, "%.2f Mbit/s", v / 1000.0);
    else snprintf(b, sizeof b, "%.0f kbit/s", v);
    return b;
}

std::string fpsText(double f) {
    char b[32];
    if (std::fabs(f - std::round(f)) < 1e-6) snprintf(b, sizeof b, "%.0f fps", f);
    else snprintf(b, sizeof b, "%.1f fps", f);
    return b;
}

} // namespace

Codec codecOf(const std::string& e) {
    if (e == "libx264" || e == "libopenh264" || starts(e, "h264_")) return Codec::H264;
    if (e == "libx265" || starts(e, "hevc_")) return Codec::Hevc;
    return Codec::Mpeg2;
}

AudioCodec audioCodecOf(const std::string& e) {
    if (e == "aac") return AudioCodec::Aac;
    if (e == "mp2" || e == "mp2fixed") return AudioCodec::Mp2;
    if (e == "ac3" || e == "ac3_fixed") return AudioCodec::Ac3;
    return AudioCodec::None;
}

EncoderCaps capsOf(const std::string& e) {
    EncoderCaps c;
    if (e == "libx264") { c.preset = c.zeroLatency = c.profile = c.bFrames = c.crf = true; }
    else if (e == "libx265") { c.preset = c.zeroLatency = c.bFrames = c.crf = true; }   // x265's profiles are main / main10, not the H.264 ones
    else if (e == "mpeg2video") { c.bFrames = true; }
    else if (e == "libopenh264") { c.profile = true; }
    // Media Foundation (Windows' own H.264 encoder) gives B-frames with pts < dts, which the MPEG-TS muxer refuses
    else if (isMf(e)) { c.profile = codecOf(e) == Codec::H264; }
    else if (codecOf(e) == Codec::H264) { c.profile = c.bFrames = true; }
    // hardware HEVC: no B-frames. NVIDIA before Turing (GTX 10xx) and AMD before RDNA 3 refuse them, and the encoder does not open.
    else if (codecOf(e) == Codec::Hevc) { c.bFrames = false; }
    return c;
}

std::string profileName(const std::string& e, int p) {
    p = std::clamp(p, 0, kNumProfiles - 1);
    if (e == "h264_amf") { static const char* const amf[] = {"constrained_baseline", "main", "high"}; return amf[p]; }
    if (e == "libx264" || e == "h264_nvenc" || e == "h264_qsv" || e == "h264_videotoolbox") return kProfiles[p];
    return std::string();
}

#ifdef AV_PROFILE_H264_HIGH
#define VGEN_H264_PROFILE(x) AV_PROFILE_H264_##x
#else
#define VGEN_H264_PROFILE(x) FF_PROFILE_H264_##x
#endif

bool setupVideoEncoder(AVCodecContext* vx, AVDictionary** o, const AVCodec* vc, const std::string& venc, bool hwMode, const Settings& s,
                       int W, int H, int fpsNum, int fpsDen, bool full) {
    const bool nvenc = venc.find("_nvenc") != std::string::npos, amf = venc.find("_amf") != std::string::npos, mf = isMf(venc);
    // a hardware encoder works in NV12: Intel's encoder behind Media Foundation lists YUV 4:2:0 too, but gives no pictures out with it
    const bool hw = (mf && hwMode) || nvenc || amf || venc.find("_qsv") != std::string::npos || venc.find("_videotoolbox") != std::string::npos;
    const AVPixelFormat first = hw ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P, second = hw ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
    const AVPixelFormat pf = hasPixFmt(vc, first) ? first : hasPixFmt(vc, second) ? second : AV_PIX_FMT_NONE;
    if (pf == AV_PIX_FMT_NONE) return false;
    const AVRational fr{fpsNum, fpsDen};
    const double fps = av_q2d(fr);
    const EncoderCaps caps = capsOf(venc);
    vx->width = W; vx->height = H;
    vx->pix_fmt = pf;
    vx->time_base = av_inv_q(fr); vx->framerate = fr;
    vx->sample_aspect_ratio = AVRational{1, 1};
    const int gop = s.frames == FrameTypes::IOnly ? 1 : std::max(1, (int)std::lround(s.gopSeconds * fps));
    vx->gop_size = gop;
    int bf = full && s.frames == FrameTypes::IPB && caps.bFrames ? 2 : 0;
    if (caps.profile && s.profile == 0) bf = 0;   // Baseline has no B-frames
    vx->max_b_frames = std::min(bf, gop - 1);
    const int64_t rate = (int64_t)effectiveVideoKbps(s) * 1000;
    const bool mpeg2 = codecOf(venc) == Codec::Mpeg2;
    const int64_t vbvCap = mpeg2 ? (W <= 720 && H <= 576 ? 1835008 : 9781248) : INT64_MAX;   // MPEG-2 Main Level / High Level
    RateControl rc = s.rc;
    if (rc == RateControl::Crf && !caps.crf) rc = RateControl::ConstrainedVbr;
    // the encoder's own rate-control mode: NVENC, AMF and Media Foundation pick their own otherwise (AMF: peak-constrained VBR even
    // when the minimum, mean and maximum rate are the same); QSV takes CBR from rc_max_rate == bit_rate by itself
    if (rc == RateControl::Cbr) {
        vx->bit_rate = vx->rc_min_rate = vx->rc_max_rate = rate;
        vx->rc_buffer_size = (int)std::min<int64_t>(rate, vbvCap);
        if (venc == "libx264") av_dict_set(o, "nal-hrd", "cbr", 0);   // filler data keeps the rate even on a still picture
        if (nvenc) av_dict_set(o, "rc", "cbr", 0);
        if (full && amf) av_dict_set(o, "rc", "cbr", 0);
        if (full && mf) av_dict_set(o, "rate_control", "cbr", 0);
    } else if (rc == RateControl::ConstrainedVbr) {
        vx->bit_rate = rate;
        vx->rc_max_rate = rate * 3 / 2;
        vx->rc_buffer_size = (int)std::min<int64_t>(rate * 3 / 2, vbvCap);
        if (nvenc) av_dict_set(o, "rc", "vbr", 0);
        if (full && amf) av_dict_set(o, "rc", "vbr_peak", 0);
        if (full && mf) av_dict_set(o, "rate_control", "pc_vbr", 0);
    } else {
        av_dict_set_int(o, "crf", s.crf, 0);
        vx->rc_max_rate = rate * 2;   // capped, so the mux rate the estimate gives is enough
        vx->rc_buffer_size = (int)(rate * 2);
    }
    if (mpeg2) {
        // vbv_delay is 16 bits of 90 kHz: a buffer longer than about 0.7 s of the rate makes the stream VBR in the headers
        vx->rc_buffer_size = (int)std::min<int64_t>(vx->rc_buffer_size, vx->rc_max_rate * 7 / 10);
        vx->rc_initial_buffer_occupancy = vx->rc_buffer_size * 3 / 4;
        // a busy picture (camera noise) can still take longer than vbv_delay holds, and FFmpeg aborts on it in CBR: VBR headers
        // then (vbv_delay 0xFFFF); the rate control keeps the set rate, the muxer pads the rest with null packets
        if (vx->rc_min_rate && !mpeg2CbrHeaders(W, H, vx->rc_min_rate)) vx->rc_min_rate = 0;
    }
    if (caps.preset) av_dict_set(o, "preset", kPresets[std::clamp(s.preset, 0, kNumPresets - 1)], 0);
    if (caps.zeroLatency && s.zeroLatency) av_dict_set(o, "tune", "zerolatency", 0);
    if (full && caps.profile) {
        const int p = std::clamp(s.profile, 0, kNumProfiles - 1);
        const std::string name = profileName(venc, p);
        if (!name.empty()) av_dict_set(o, "profile", name.c_str(), 0);
        else vx->profile = p == 0 ? VGEN_H264_PROFILE(CONSTRAINED_BASELINE) : p == 1 ? VGEN_H264_PROFILE(MAIN) : VGEN_H264_PROFILE(HIGH);
    }
    if (mf && hwMode) av_dict_set(o, "hw_encoding", "1", 0);   // without it FFmpeg takes Windows' software encoder
    if (venc == "libx264") av_dict_set(o, "x264-params", "scenecut=0", 0);   // the GOP the user set, not one the scene cuts choose
    if (venc == "libx265") av_dict_set(o, "x265-params", rc == RateControl::Cbr ? "log-level=error:scenecut=0:strict-cbr=1" : "log-level=error:scenecut=0", 0);
    return true;
}

int sendToEncoder(AVCodecContext* cx, const AVFrame* f, AVPacket* pk, const std::function<void(AVPacket*)>& take) {
    auto drain = [&] {   // the packets that are ready: AVERROR(EAGAIN) when it needs more time or pictures, AVERROR_EOF at the end
        int r;
        while ((r = avcodec_receive_packet(cx, pk)) >= 0) { take(pk); av_packet_unref(pk); }
        return r;
    };
    int r = avcodec_send_frame(cx, f);
    for (int waited = 0; r == AVERROR(EAGAIN) && waited < 2000; waited++) {
        const int d = drain();
        if (d != AVERROR(EAGAIN)) return d;
        r = avcodec_send_frame(cx, f);
        if (r == AVERROR(EAGAIN)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (r < 0) return r;
    if (f) { const int d = drain(); return d == AVERROR(EAGAIN) || d == AVERROR_EOF ? 0 : d; }
    for (int waited = 0; waited < 5000; waited++) {   // the end: what is still inside the encoder (a hardware one takes its time)
        const int d = drain();
        if (d == AVERROR_EOF) return 0;
        if (d != AVERROR(EAGAIN)) return d;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return AVERROR(EAGAIN);
}

const char* codecName(Codec c) {
    switch (c) {
    case Codec::H264: return "H.264";
    case Codec::Hevc: return "H.265";
    default: return "MPEG-2";
    }
}

const char* audioCodecName(AudioCodec c) {
    switch (c) {
    case AudioCodec::Aac: return "AAC";
    case AudioCodec::Mp2: return "MPEG-1 Layer II";
    case AudioCodec::Ac3: return "AC-3";
    default: return "-";
    }
}

namespace {
struct VideoProbe {
    std::vector<EncoderInfo> list;
    std::vector<EncoderProblem> problems;
};
const VideoProbe& videoProbe() {
    static const VideoProbe probe = [] {
        VideoProbe p;
        for (const Candidate& k : kVideo) {
            const AVCodec* c = avcodec_find_encoder_by_name(k.name);
            if (!c) continue;   // not in this FFmpeg
            bool hw = k.hw;
            std::string why = tryVideo(c, hw);
            if (!why.empty() && isMf(k.name)) {   // no hardware encoder behind Media Foundation: Windows' own
                p.problems.push_back({std::string(k.name) + " (hardware)", why});
                hw = false;
                why = tryVideo(c, false);
            }
            if (!why.empty()) { p.problems.push_back({k.name, why}); continue; }
            EncoderInfo e;
            e.name = k.name; e.codec = k.codec; e.hardware = hw;
            e.label = std::string(codecName(k.codec)) + " (" + k.name + (isMf(k.name) ? (hw ? ", hardware" : ", software") : "") + ")";
            p.list.push_back(e);
        }
        return p;
    }();
    return probe;
}
} // namespace

const std::vector<EncoderInfo>& videoEncoders() { return videoProbe().list; }
const std::vector<EncoderProblem>& videoEncoderProblems() { return videoProbe().problems; }

bool encoderHardware(const std::string& name) {
    for (const EncoderInfo& e : videoEncoders()) if (e.name == name) return e.hardware;
    for (const Candidate& k : kVideo) if (name == k.name) return k.hw;
    return false;
}

const std::vector<EncoderInfo>& audioEncoders() {
    static const std::vector<EncoderInfo> list = [] {
        std::vector<EncoderInfo> v;
        for (const ACandidate& k : kAudio) {
            const AVCodec* c = avcodec_find_encoder_by_name(k.name);
            if (!c || !audioOpens(c)) continue;
            EncoderInfo e;
            e.name = k.name; e.audio = k.codec;
            e.label = audioCodecName(k.codec);
            v.push_back(e);
        }
        return v;
    }();
    return list;
}

std::string pickVideoEncoder(const std::string& wanted, const std::vector<EncoderInfo>& have) {
    for (const EncoderInfo& e : have) if (e.name == wanted) return wanted;
    for (const EncoderInfo& e : have) if (e.codec == codecOf(wanted)) return e.name;
    return "mpeg2video";
}

int audioTrackCount(const Settings& s) {
    if (s.content == TsContent::VideoOnly || s.audioEncoder.empty()) return 0;
    if (s.content == TsContent::VideoAudio) return 1;
    return std::clamp(s.audioTracks, 2, 4);
}

double bitsPerPixel(Codec c, FrameTypes f) {
    // tuned so the spec's defaults (H.264, 720x576, 15 fps, I/P/B) come to 1 Mbit/s; HEVC needs about 30 % less, MPEG-2 about twice as much
    double b = c == Codec::H264 ? 0.161 : c == Codec::Hevc ? 0.113 : 0.32;
    if (f == FrameTypes::IP) b *= 1.15;
    else if (f == FrameTypes::IOnly) b *= c == Codec::Mpeg2 ? 2.5 : 3.0;   // no prediction at all: the modern codecs lose most
    return b;
}

int macroblocks(int width, int height) { return ((std::max(1, width) + 15) / 16) * ((std::max(1, height) + 15) / 16); }

int minVideoKbps(const Settings& s) {
    if (codecOf(s.videoEncoder) != Codec::Mpeg2) return 10;
    const int64_t bits = (int64_t)macroblocks(s.width, s.height) * kMpeg2MinBitsPerMb;
    return std::max(10, (int)((bits + 9999) / 10000 * 10));
}

bool mpeg2CbrHeaders(int width, int height, int64_t bitrate) { return bitrate >= (int64_t)macroblocks(width, height) * kMpeg2CbrBitsPerMb; }

int suggestVideoKbps(const Settings& s) {
    const double kb = bitsPerPixel(codecOf(s.videoEncoder), s.frames) * s.width * s.height * std::max(0.1, s.fps) / 1000.0;
    return std::max({50, (int)std::lround(kb / 10.0) * 10, minVideoKbps(s)});
}

int effectiveVideoKbps(const Settings& s) { return std::max(minVideoKbps(s), s.autoBitrate ? suggestVideoKbps(s) : s.videoKbps); }

Budget estimate(const Settings& s) {
    Budget b;
    const double fps = std::max(0.1, s.fps);
    b.video = effectiveVideoKbps(s);
    b.videoPeak = s.rc == RateControl::Cbr ? b.video : s.rc == RateControl::ConstrainedVbr ? b.video * 1.5 : b.video * 2.0;
    const int tracks = audioTrackCount(s);
    b.audio = (double)tracks * s.audioKbps;
    // A TS packet carries 184 bytes of payload behind its 4-byte header. Each video picture is one PES (19 header bytes with PTS and DTS)
    // and its last packet is half empty on average (stuffing in the adaptation field). The muxer sends audio PES of about 2930 bytes,
    // at least every 0.5 s. The PCR (every 40 ms) takes 8 bytes of adaptation field.
    auto ts = [](double kbit) { return kbit * 4.0 / 184.0; };
    const double vPes = fps * (19 + 92) * 8 / 1000.0;
    double aPes = 0;
    if (tracks > 0) {
        const double pesPerSec = std::max(2.0, s.audioKbps * 1000.0 / 8.0 / 2930.0);
        aPes = tracks * pesPerSec * (14 + 92) * 8 / 1000.0;
    }
    const double pcr = 25 * 8 * 8 / 1000.0;
    b.pesTs = ts(b.video) + ts(b.audio) + vPes + aPes + pcr;
    // PAT and PMT 10 times a second, SDT twice (the periods the generator sets in the muxer)
    b.psiSi = (10 + 10 + 2) * 188 * 8 / 1000.0;
    b.total = b.video + b.audio + b.pesTs + b.psiSi;
    b.peak = b.videoPeak + b.audio + ts(b.videoPeak) + ts(b.audio) + vPes + aPes + pcr + b.psiSi;
    b.suggestedMux = (int)std::ceil(b.peak * 1.03 / 10.0) * 10;
    b.mux = s.autoMux ? b.suggestedMux : std::max(100, s.muxKbps);
    return b;
}

std::vector<std::string> checkSettings(const Settings& s, const Budget& b) {
    std::vector<std::string> w;
    if (b.mux < b.peak) w.push_back("The mux rate is below what the streams need: the muxer will run late and players may stutter.");
    if (s.width % 2 || s.height % 2) w.push_back("Width and height must be even for 4:2:0.");
    if (s.frames == FrameTypes::IPB && !capsOf(s.videoEncoder).bFrames) w.push_back("This encoder makes no B-frames: I/P is used.");
    if (s.profile == 0 && s.frames == FrameTypes::IPB && capsOf(s.videoEncoder).profile) w.push_back("The Baseline profile has no B-frames: I/P is used.");
    if (s.rc == RateControl::Crf && !capsOf(s.videoEncoder).crf) w.push_back("This encoder has no CRF mode: constrained VBR is used.");
    if (codecOf(s.videoEncoder) == Codec::Mpeg2 && (s.width > 1920 || s.height > 1152)) w.push_back("MPEG-2 Main Profile at High Level stops at 1920x1152.");
    if (!s.autoBitrate && s.videoKbps < minVideoKbps(s)) w.push_back("The video bitrate is below the MPEG-2 minimum for this picture size: the minimum is used.");
    return w;
}

const char* paramLabel(Param p) {
    switch (p) {
    case Param::VideoCodec: return "Video codec";
    case Param::VideoBitrate: return "Video bitrate";
    case Param::Resolution: return "Resolution";
    case Param::FrameRate: return "Frame rate";
    case Param::Gop: return "GOP";
    case Param::FrameTypes: return "Frame types";
    case Param::Profile: return "Profile";
    case Param::RateControl: return "Rate control";
    case Param::PixelFormat: return "Pixel format";
    case Param::Preset: return "Encoder preset";
    case Param::Latency: return "Latency tuning";
    case Param::AudioCodec: return "Audio codec";
    case Param::AudioBitrate: return "Audio bitrate";
    case Param::SampleRate: return "Sample rate";
    case Param::Channels: return "Audio channels";
    case Param::TsContent: return "TS content";
    case Param::MuxRate: return "TS mux rate";
    case Param::TotalRate: return "Total TS rate";
    case Param::Service: return "Service";
    default: return "";
    }
}

std::string paramValue(Param p, const Settings& s) {
    char b[128];
    const Budget bu = estimate(s);
    switch (p) {
    case Param::VideoCodec: return std::string(codecName(codecOf(s.videoEncoder))) + " (" + s.videoEncoder + ")";
    case Param::VideoBitrate: return s.rc == RateControl::Crf && capsOf(s.videoEncoder).crf ? "CRF " + std::to_string(s.crf) : kbps(bu.video);
    case Param::Resolution: snprintf(b, sizeof b, "%dx%d", s.width, s.height); return b;
    case Param::FrameRate: return fpsText(s.fps);
    case Param::Gop:
        if (s.frames == FrameTypes::IOnly) return "all-intra";
        snprintf(b, sizeof b, "%.1f s (%d frames)", s.gopSeconds, std::max(1, (int)std::lround(s.gopSeconds * s.fps)));
        return b;
    case Param::FrameTypes: return s.frames == FrameTypes::IPB ? "I/P/B" : s.frames == FrameTypes::IP ? "I/P" : "I-only";
    case Param::Profile: return capsOf(s.videoEncoder).profile ? kProfiles[std::clamp(s.profile, 0, kNumProfiles - 1)] : "-";
    case Param::RateControl: return s.rc == RateControl::Cbr ? "CBR" : s.rc == RateControl::ConstrainedVbr ? "Constrained VBR" : "VBR/CRF";
    case Param::PixelFormat: return "yuv420p 8-bit";
    case Param::Preset: return capsOf(s.videoEncoder).preset ? kPresets[std::clamp(s.preset, 0, kNumPresets - 1)] : "-";
    case Param::Latency: return s.zeroLatency && capsOf(s.videoEncoder).zeroLatency ? "zerolatency" : "default";
    case Param::AudioCodec: return audioTrackCount(s) ? audioCodecName(audioCodecOf(s.audioEncoder)) : "no audio";
    case Param::AudioBitrate: return audioTrackCount(s) ? kbps(s.audioKbps) : "-";
    case Param::SampleRate: snprintf(b, sizeof b, "%.1f kHz", s.sampleRate / 1000.0); return b;
    case Param::Channels: return s.channels >= 2 ? "stereo" : "mono";
    case Param::TsContent: {
        const int n = audioTrackCount(s);
        if (n == 0) return "video only";
        snprintf(b, sizeof b, "video + %d audio", n);
        return b;
    }
    case Param::MuxRate: return kbps(bu.mux);
    case Param::TotalRate: return kbps(bu.total);
    case Param::Service: snprintf(b, sizeof b, "%s (%d)", s.serviceName.c_str(), s.serviceId); return b;
    default: return "";
    }
}

std::string saveSettings(const Settings& s) {
    std::ostringstream o;
    o << "content=" << (int)s.content << "\n" << "venc=" << s.videoEncoder << "\n" << "rc=" << (int)s.rc << "\n"
      << "autobr=" << (s.autoBitrate ? 1 : 0) << "\n" << "vkbps=" << s.videoKbps << "\n" << "crf=" << s.crf << "\n"
      << "fps=" << s.fps << "\n" << "w=" << s.width << "\n" << "h=" << s.height << "\n" << "gop=" << s.gopSeconds << "\n"
      << "frames=" << (int)s.frames << "\n" << "profile=" << s.profile << "\n" << "preset=" << s.preset << "\n"
      << "zerolat=" << (s.zeroLatency ? 1 : 0) << "\n" << "aenc=" << s.audioEncoder << "\n" << "atracks=" << s.audioTracks << "\n"
      << "akbps=" << s.audioKbps << "\n" << "arate=" << s.sampleRate << "\n" << "ach=" << s.channels << "\n"
      << "automux=" << (s.autoMux ? 1 : 0) << "\n" << "mux=" << s.muxKbps << "\n" << "sid=" << s.serviceId << "\n"
      << "autopid=" << (s.autoPids ? 1 : 0) << "\n" << "pmtpid=" << s.pmtPid << "\n" << "vpid=" << s.videoPid << "\n"
      << "apid=" << s.audioPid << "\n" << "sname=" << s.serviceName << "\n" << "sprov=" << s.provider << "\n";
    return o.str();
}

void loadSettings(const std::string& text, Settings& s) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const int i = atoi(v.c_str());
        const double d = atof(v.c_str());
        if (k == "content") s.content = (TsContent)std::clamp(i, 0, 2);
        else if (k == "venc" && !v.empty()) s.videoEncoder = v;
        else if (k == "rc") s.rc = (RateControl)std::clamp(i, 0, 2);
        else if (k == "autobr") s.autoBitrate = i != 0;
        else if (k == "vkbps") s.videoKbps = std::clamp(i, 10, 100000);
        else if (k == "crf") s.crf = std::clamp(i, 0, 51);
        else if (k == "fps") s.fps = std::clamp(d, 1.0, 60.0);
        else if (k == "w") s.width = std::clamp(i, 16, 3840) & ~1;
        else if (k == "h") s.height = std::clamp(i, 16, 2160) & ~1;
        else if (k == "gop") s.gopSeconds = std::clamp(d, 0.1, 10.0);
        else if (k == "frames") s.frames = (FrameTypes)std::clamp(i, 0, 2);
        else if (k == "profile") s.profile = std::clamp(i, 0, kNumProfiles - 1);
        else if (k == "preset") s.preset = std::clamp(i, 0, kNumPresets - 1);
        else if (k == "zerolat") s.zeroLatency = i != 0;
        else if (k == "aenc") s.audioEncoder = v;
        else if (k == "atracks") s.audioTracks = std::clamp(i, 2, 4);
        else if (k == "akbps") s.audioKbps = std::clamp(i, 16, 640);
        else if (k == "arate") s.sampleRate = std::clamp(i, 8000, 48000);
        else if (k == "ach") s.channels = std::clamp(i, 1, 2);
        else if (k == "automux") s.autoMux = i != 0;
        else if (k == "mux") s.muxKbps = std::clamp(i, 100, 200000);
        else if (k == "sid") s.serviceId = std::clamp(i, 1, 65535);
        else if (k == "autopid") s.autoPids = i != 0;
        else if (k == "pmtpid") s.pmtPid = std::clamp(i, 0x20, 0x1FFE);
        else if (k == "vpid") s.videoPid = std::clamp(i, 0x20, 0x1FFE);
        else if (k == "apid") s.audioPid = std::clamp(i, 0x20, 0x1FFA);
        else if (k == "sname") s.serviceName = v;
        else if (k == "sprov") s.provider = v;
    }
}

} // namespace dect2::vgen
