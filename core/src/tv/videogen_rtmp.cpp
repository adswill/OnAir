// RTMP server input of the digital video generator (videogen_rtmp.h). One thread, "onair-rtmp": FFmpeg's RTMP protocol listens
// (listen=1) until a sender connects, the FLV it sends is decoded, the newest picture is kept as YUV 4:2:0 and the sound goes into a short
// queue of stereo float samples. When the sender stops (or sends nothing for kIdleSeconds) the connection is closed and the thread listens
// again. A blocking accept or read is ended through FFmpeg's interrupt callback, so stop() never waits for the network.
#include "dect2/tv/videogen_rtmp.h"
#include "dect2/common/thread_name.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>

namespace dect2::vgen {

namespace {

constexpr double kIdleSeconds = 10;     // a sender that sends nothing for this long is dropped (a PC that went away without closing)
constexpr double kAudioStart = 0.10;    // seconds of sound queued before it is played out
constexpr double kAudioHigh = 0.50;     // more than this queued (the sender's clock is faster): back to kAudioLow
constexpr double kAudioLow = 0.20;

std::string averr(int e) {
    std::string b(AV_ERROR_MAX_STRING_SIZE, '\0');
    av_strerror(e, b.data(), b.size());
    b.resize(std::string_view(b.c_str()).size());
    return b;
}

bool allDigits(std::string_view s) {
    return !s.empty() && s.size() <= 5 && std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

// "rtmp://host:port/app/key" in its parts; false when it is not an rtmp:// URL with an app
bool splitUrl(const std::string& url, std::string& host, int& port, std::string& app, std::string& key, std::string& err) {
    std::string_view u(url);
    while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.remove_prefix(1);
    while (!u.empty() && (u.back() == ' ' || u.back() == '\t' || u.back() == '/')) u.remove_suffix(1);
    constexpr std::string_view scheme = "rtmp://";
    if (u.size() < scheme.size() || !std::equal(scheme.begin(), scheme.end(), u.begin(), [](char a, char b) { return a == (char)std::tolower((unsigned char)b); })) {
        err = "the address must start with rtmp:// (rtmps and other protocols are not served)";
        return false;
    }
    u.remove_prefix(scheme.size());
    const size_t slash = u.find('/');
    const std::string_view hostPort = u.substr(0, slash);
    const std::string_view path = slash == std::string_view::npos ? std::string_view() : u.substr(slash + 1);
    port = kRtmpDefaultPort;
    host = std::string(hostPort);
    if (const size_t colon = hostPort.rfind(':'); colon != std::string_view::npos && hostPort.find(']') == std::string_view::npos) {
        const std::string_view p = hostPort.substr(colon + 1);
        if (!allDigits(p)) { err = "the port after the ':' must be a number"; return false; }
        port = std::stoi(std::string(p));
        host = std::string(hostPort.substr(0, colon));
    }
    if (port < 1 || port > 65535) { err = "the port must be 1 to 65535"; return false; }
    if (host.find_first_of(" /?#@") != std::string::npos) { err = "the address is not a host name or IP address"; return false; }
    if (path.empty()) { err = "add the application name after the port, e.g. rtmp://0.0.0.0:1935/live/stream"; return false; }
    const size_t s2 = path.find('/');
    app = std::string(path.substr(0, s2));
    key = s2 == std::string_view::npos ? std::string() : std::string(path.substr(s2 + 1));
    if (app.empty()) { err = "the application name (after the port) is empty"; return false; }
    if (path.find_first_of("?# ") != std::string_view::npos) { err = "the address cannot have ? or # parts or spaces"; return false; }
    return true;
}

} // namespace

bool parseRtmpListenUrl(const std::string& url, std::string& listenUrl, int& port, std::string& err) {
    std::string host, app, key;
    if (!splitUrl(url, host, port, app, key, err)) return false;
    if (host.empty()) host = "0.0.0.0";
    listenUrl = "rtmp://" + host + ":" + std::to_string(port) + "/" + app + (key.empty() ? "" : "/" + key);
    return true;
}

void rtmpObsSettings(const std::string& url, const std::string& thisPc, std::string& server, std::string& key) {
    std::string host, app, err;
    int port = kRtmpDefaultPort;
    if (!splitUrl(url, host, port, app, key, err)) { server.clear(); key.clear(); return; }
    if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]") host = thisPc;
    server = "rtmp://" + host + (port == kRtmpDefaultPort ? std::string() : ":" + std::to_string(port)) + "/" + app;
}

bool rtmpSupported(std::string& why) {
    bool proto = false;
    void* it = nullptr;
    while (const char* p = avio_enum_protocols(&it, 0)) proto = proto || std::string_view(p) == "rtmp";
    if (!proto) { why = "this FFmpeg was built without the RTMP protocol (network support)"; return false; }
    // an FFmpeg built with librtmp (MSYS2's, for one) uses it for rtmp://, and librtmp cannot listen: no server, every sender is refused
    if (const char* cfg = avformat_configuration(); cfg && std::string_view(cfg).find("--enable-librtmp") != std::string_view::npos) {
        why = "this FFmpeg uses librtmp for RTMP, which cannot act as a server (OnAir's own FFmpeg builds use FFmpeg's RTMP)";
        return false;
    }
    if (!av_find_input_format("flv")) { why = "this FFmpeg was built without the FLV demuxer"; return false; }
    return true;
}

struct RtmpInput::Impl {
    mutable std::mutex mu;
    std::thread th;
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> isConnected{false};
    std::atomic<int64_t> lastData{0};   // steady clock, ns: the last packet from the sender
    std::string listenUrl, status;
    int port = kRtmpDefaultPort;
    int rate = 48000;
    // the newest picture
    std::vector<uint8_t> py, pu, pv;
    int pw = 0, ph = 0;
    uint64_t serial = 0;
    // the sound, interleaved stereo
    std::deque<float> fifo;
    bool primed = false;

    static int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    static int interrupt(void* op) {
        const Impl* I = static_cast<const Impl*>(op);
        if (I->stopFlag.load()) return 1;
        return I->isConnected.load() && (double)(now() - I->lastData.load()) * 1e-9 > kIdleSeconds ? 1 : 0;
    }
    void setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(mu); status = s; }
    void session();
    void run();
};

// one sender: listen, decode until it stops
void RtmpInput::Impl::session() {
    AVFormatContext* fc = avformat_alloc_context();
    if (!fc) { setStatus("out of memory"); return; }
    fc->interrupt_callback.callback = &Impl::interrupt;
    fc->interrupt_callback.opaque = this;
    AVDictionary* o = nullptr;
    av_dict_set(&o, "listen", "1", 0);
    // OBS sends the codec set-up (FLV sequence headers) first: a short look at the stream is enough, the picture starts sooner
    av_dict_set(&o, "analyzeduration", "300000", 0);
    av_dict_set(&o, "fpsprobesize", "0", 0);
    setStatus("Waiting for a sender on port " + std::to_string(port));
    int r = avformat_open_input(&fc, listenUrl.c_str(), av_find_input_format("flv"), &o);   // fc is freed on failure
    av_dict_free(&o);
    if (r < 0) {
        if (!stopFlag) {
            setStatus("Cannot listen on port " + std::to_string(port) + ": " + averr(r));
            for (int i = 0; i < 20 && !stopFlag; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));   // then try again
        }
        return;
    }
    lastData = now();
    isConnected = true;
    setStatus("A sender connected, reading its stream");
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pk = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    int vi = -1, ai = -1;
    std::string why;
    if ((r = avformat_find_stream_info(fc, nullptr)) < 0) why = "cannot read the stream: " + averr(r);
    auto openDec = [&](int si, AVCodecContext*& ctx) {
        const AVCodecParameters* par = fc->streams[si]->codecpar;
        const AVCodec* c = avcodec_find_decoder(par->codec_id);
        if (!c || !(ctx = avcodec_alloc_context3(c))) return false;
        avcodec_parameters_to_context(ctx, par);
        ctx->pkt_timebase = fc->streams[si]->time_base;
        if (avcodec_open2(ctx, c, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
        return true;
    };
    if (why.empty()) {
        vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (vi < 0) why = "the sender's stream has no picture";
        else if (!openDec(vi, vdec)) why = std::string("this FFmpeg cannot decode the sender's ") + avcodec_get_name(fc->streams[vi]->codecpar->codec_id) + " picture";
        if (ai >= 0 && !openDec(ai, adec)) ai = -1;   // a sound this FFmpeg cannot decode: the picture alone
        if (adec) {
            AVChannelLayout stereo;
            av_channel_layout_default(&stereo, 2);
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, rate, &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                avcodec_free_context(&adec);
                ai = -1;
            }
        }
    }
    if (why.empty()) {
        const AVCodecParameters* vp = fc->streams[vi]->codecpar;
        setStatus("Receiving " + std::to_string(vp->width) + "x" + std::to_string(vp->height) + " " + avcodec_get_name(vp->codec_id) +
                  (ai >= 0 ? std::string(" + ") + avcodec_get_name(fc->streams[ai]->codecpar->codec_id) : std::string(", no sound")));
    }
    std::vector<float> abuf;
    while (why.empty() && !stopFlag) {
        r = av_read_frame(fc, pk);
        if (r < 0) break;   // the sender stopped (or went quiet: the interrupt callback)
        lastData = now();
        if (pk->stream_index == vi && avcodec_send_packet(vdec, pk) >= 0) {
            while (avcodec_receive_frame(vdec, fr) >= 0) {
                const int w = fr->width & ~1, h = fr->height & ~1;
                if (w < 2 || h < 2) continue;
                sws = sws_getCachedContext(sws, fr->width, fr->height, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) continue;
                std::vector<uint8_t> y((size_t)w * h), u((size_t)(w / 2) * (h / 2)), v((size_t)(w / 2) * (h / 2));
                uint8_t* d[3] = {y.data(), u.data(), v.data()};
                const int ds[3] = {w, w / 2, w / 2};
                sws_scale(sws, fr->data, fr->linesize, 0, fr->height, d, ds);
                std::lock_guard<std::mutex> lk(mu);
                py.swap(y); pu.swap(u); pv.swap(v);
                pw = w; ph = h;
                serial++;
            }
        } else if (pk->stream_index == ai && adec && avcodec_send_packet(adec, pk) >= 0) {
            while (avcodec_receive_frame(adec, fr) >= 0) {
                const int outN = swr_get_out_samples(swr, fr->nb_samples);
                if (outN <= 0) continue;
                abuf.resize((size_t)outN * 2);
                uint8_t* out[1] = {reinterpret_cast<uint8_t*>(abuf.data())};
                const int n = swr_convert(swr, out, outN, (const uint8_t**)fr->extended_data, fr->nb_samples);
                if (n <= 0) continue;
                std::lock_guard<std::mutex> lk(mu);
                fifo.insert(fifo.end(), abuf.begin(), abuf.begin() + (std::ptrdiff_t)n * 2);
                const size_t cap = (size_t)rate * 2 * 2;   // two seconds: nobody reads (a preview has no sound)
                if (fifo.size() > cap) fifo.erase(fifo.begin(), fifo.begin() + (std::ptrdiff_t)(fifo.size() - cap));
            }
        }
        av_packet_unref(pk);
    }
    isConnected = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        fifo.clear();
        primed = false;
        py.clear(); pu.clear(); pv.clear();
        pw = ph = 0;
        serial++;   // the picture goes black
        if (!why.empty()) status = why;
    }
    if (!why.empty()) for (int i = 0; i < 20 && !stopFlag; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sws_freeContext(sws);
    swr_free(&swr);
    av_frame_free(&fr);
    av_packet_free(&pk);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    avformat_close_input(&fc);
}

void RtmpInput::Impl::run() {
    while (!stopFlag) session();
}

RtmpInput::RtmpInput() : p_(new Impl) {}
RtmpInput::~RtmpInput() { stop(); }

bool RtmpInput::start(const std::string& url, int sampleRate, std::string& err) {
    stop();
    if (!rtmpSupported(err)) return false;
    if (!parseRtmpListenUrl(url, p_->listenUrl, p_->port, err)) return false;
    static std::once_flag net;
    std::call_once(net, [] { avformat_network_init(); });
    p_->rate = std::clamp(sampleRate, 8000, 192000);
    p_->stopFlag = false;
    p_->isConnected = false;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->status = "Waiting for a sender on port " + std::to_string(p_->port);
        p_->fifo.clear();
        p_->primed = false;
        p_->py.clear(); p_->pu.clear(); p_->pv.clear();
        p_->pw = p_->ph = 0;
    }
    Impl* I = p_.get();
    p_->th = dect2::namedThread("onair-rtmp", [I]() { I->run(); });
    return true;
}

void RtmpInput::stop() {
    p_->stopFlag = true;
    if (p_->th.joinable()) p_->th.join();
    p_->isConnected = false;
}

bool RtmpInput::connected() const { return p_->isConnected.load(); }

std::string RtmpInput::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->status;
}

bool RtmpInput::picture(std::vector<uint8_t>& y, std::vector<uint8_t>& u, std::vector<uint8_t>& v, int& w, int& h, uint64_t& serial) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->serial == serial) return false;
    serial = p_->serial;
    y = p_->py; u = p_->pu; v = p_->pv;
    w = p_->pw; h = p_->ph;
    return true;
}

void RtmpInput::readAudio(float* stereo, int n) {
    if (n <= 0) return;
    const size_t want = (size_t)n * 2;
    std::fill_n(stereo, want, 0.f);
    std::lock_guard<std::mutex> lk(p_->mu);
    std::deque<float>& f = p_->fifo;
    const auto secs = [&](double s) { return (size_t)(s * p_->rate) * 2; };
    if (!p_->primed) {
        if (f.size() < secs(kAudioStart)) return;
        p_->primed = true;
    }
    if (f.size() > secs(kAudioHigh)) f.erase(f.begin(), f.begin() + (std::ptrdiff_t)(f.size() - secs(kAudioLow)));
    const size_t m = std::min(want, f.size());
    std::copy_n(f.begin(), m, stereo);
    f.erase(f.begin(), f.begin() + (std::ptrdiff_t)m);
    if (f.empty()) p_->primed = false;   // ran dry: wait for a little sound again, no stutter
}

} // namespace dect2::vgen
