// Cameras / capture cards through FFmpeg libavdevice (v4l2, dshow). Used off macOS.
#include "dect2/common/thread_name.h"
#include "videoin_impl.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string_view>
#ifdef _WIN32
#include "win_privacy.h"
#include <chrono>
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace dect2 {

namespace {
const char* inputFmt() {
#ifdef _WIN32
    return "dshow";
#else
    return "v4l2";
#endif
}

#ifdef _WIN32
// DirectShow opens a device by its name ("video=HP HD Camera"), not by a number. Asking DirectShow takes tens of ms and the
// transmitter panel lists every frame, so the list is kept for a few seconds.
std::vector<VideoInputDevice> dshowCameras() {
    static std::mutex mu;
    static std::vector<VideoInputDevice> cache;
    static std::chrono::steady_clock::time_point at;
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (at != std::chrono::steady_clock::time_point() && now - at < std::chrono::seconds(3)) return cache;
    at = now;
    cache.clear();
    avdevice_register_all();
    const AVInputFormat* inf = av_find_input_format("dshow");
    AVDeviceInfoList* list = nullptr;
    if (!inf || avdevice_list_input_sources(inf, nullptr, nullptr, &list) < 0 || !list) return cache;
    for (int i = 0; i < list->nb_devices; i++) {
        const AVDeviceInfo* d = list->devices[i];
        // one of the two is the friendly name, the other the "@device_pnp_..." moniker (too long for the saved settings)
        std::string_view a = d->device_name ? d->device_name : "", b = d->device_description ? d->device_description : "";
        if (a.starts_with("@device")) std::swap(a, b);
        // a webcam may report no media types at all (seen: HP HD Camera); microphones sit in the audio-input category
        bool video = d->nb_media_types == 0 && b.find("{33D9A762-90C8-11D0-BD43-00A0C911CE86}") == std::string_view::npos;
        for (int k = 0; k < d->nb_media_types; k++) video = video || d->media_types[k] == AVMEDIA_TYPE_VIDEO;
        if (video && !a.empty()) cache.push_back({"video=" + std::string(a), std::string(a)});
    }
    avdevice_free_list_devices(&list);
    return cache;
}
#endif

// FFmpeg says why a device did not open (dshow: "Could not set video options", "Could not run graph (sometimes caused by a device
// already in use by other application)", ...) only through av_log, which goes to stderr: a Windows GUI program has none. While a camera
// opens, its error lines are kept and put into the error message.
std::mutex logMu;
std::string* logSink = nullptr;

void logCb(void* avcl, int level, const char* fmt, va_list vl) {
    if (level <= AV_LOG_ERROR) {
        std::lock_guard<std::mutex> lk(logMu);
        if (logSink && logSink->size() < 600) {
            std::string line(512, '\0');
            int prefix = 1;
            va_list cp;
            va_copy(cp, vl);
            av_log_format_line2(avcl, level, fmt, cp, line.data(), (int)line.size(), &prefix);
            va_end(cp);
            line.resize(std::string_view(line.c_str()).size());
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) *logSink += (logSink->empty() ? "" : "; ") + line;
        }
    }
    av_log_default_callback(avcl, level, fmt, vl);
}

struct KeepLog {
    std::string text;
    KeepLog() {
        static std::once_flag once;
        std::call_once(once, [] { av_log_set_callback(logCb); });
        std::lock_guard<std::mutex> lk(logMu);
        if (!logSink) logSink = &text;   // two cameras opening at once: the first keeps the lines
    }
    ~KeepLog() {
        std::lock_guard<std::mutex> lk(logMu);
        if (logSink == &text) logSink = nullptr;
    }
    KeepLog(const KeepLog&) = delete;
    KeepLog& operator=(const KeepLog&) = delete;
};

std::string privacyHint() {
#ifdef _WIN32
    return winPrivacyCameraHint();
#else
    return {};
#endif
}

bool openCam(VideoIn::Impl* I, const std::string& url, std::string& err) {
    avdevice_register_all();
    KeepLog log;
    const AVInputFormat* inf = av_find_input_format(inputFmt());
    if (!inf) { err = "this build has no capture-device support (libavdevice)"; return false; }
    AVFormatContext* fmt = nullptr;
    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "framerate", "25", 0);
    av_dict_set(&opt, "video_size", "640x480", 0);
    int rc = avformat_open_input(&fmt, url.c_str(), inf, &opt);
    av_dict_free(&opt);
    // a capture card without 640x480 at 25 fps: open it in its own mode
    if (rc < 0) rc = avformat_open_input(&fmt, url.c_str(), inf, nullptr);
    if (rc < 0) {
        std::string e(AV_ERROR_MAX_STRING_SIZE, '\0');
        av_strerror(rc, e.data(), e.size());
        err = "could not open this capture device (" + url + "): " + e.c_str();
        if (!log.text.empty()) err += " (" + log.text + ")";
        err += privacyHint();
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        err = "this capture device sent no video" + (log.text.empty() ? std::string() : " (" + log.text + ")") + privacyHint();
        return false;
    }
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) { avformat_close_input(&fmt); err = "this capture device has no video"; return false; }
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); err = "could not decode this capture device"; return false;
    }
    I->fileTh = dect2::namedThread("onair-vidin", [=]() mutable {
        SwsContext* sws = nullptr;
        AVFrame* fr = av_frame_alloc();
        AVPacket* pk = av_packet_alloc();
        std::vector<uint8_t> rgb;
        while (!I->stop.load()) {
            if (av_read_frame(fmt, pk) < 0) break;
            if (pk->stream_index != si) { av_packet_unref(pk); continue; }
            if (avcodec_send_packet(ctx, pk) >= 0) {
                while (avcodec_receive_frame(ctx, fr) >= 0) {
                    if (!sws) sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format, fr->width, fr->height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    rgb.resize((size_t)fr->width * (size_t)fr->height * 3);
                    uint8_t* dst[4] = {rgb.data(), nullptr, nullptr, nullptr};
                    int ls[4] = {fr->width * 3, 0, 0, 0};
                    if (sws) { sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, ls); I->setRgb(fr->width, fr->height, rgb.data()); }
                }
            }
            av_packet_unref(pk);
        }
        if (sws) sws_freeContext(sws);
        av_frame_free(&fr); av_packet_free(&pk);
        avcodec_free_context(&ctx); avformat_close_input(&fmt);
    });
    return true;
}
}

bool videoInCameraStart(VideoIn::Impl* I, const std::string& deviceId, std::string& err) {
#ifdef _WIN32
    std::string url = deviceId;
    if (url.empty()) {
        const auto cams = dshowCameras();
        if (cams.empty()) { err = "Windows reports no camera or capture card" + privacyHint(); return false; }
        url = cams.front().id;
    } else if (url.find('=') == std::string::npos) {
        url = "video=" + url;
    }
#else
    std::string url = deviceId.empty() ? "/dev/video0" : deviceId;
#endif
    return openCam(I, url, err);
}

void videoInCameraStop(VideoIn::Impl* I) {
    I->stop = true;
    if (I->fileTh.joinable()) I->fileTh.join();
    I->backend = nullptr;
}

std::vector<VideoInputDevice> videoInListCameras() {
    std::vector<VideoInputDevice> v;
#ifdef _WIN32
    v = dshowCameras();
#else
    DIR* d = opendir("/dev");
    if (d) {
        while (dirent* e = readdir(d)) {
            if (std::strncmp(e->d_name, "video", 5) != 0) continue;
            std::string p = std::string("/dev/") + e->d_name;
            v.push_back({p, p});
        }
        closedir(d);
    }
#endif
    if (v.empty()) v.push_back({"", "Default capture device"});
    return v;
}

} // namespace dect2
