#include "dect2/atsc3_remux.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/mathematics.h>
}

namespace dect2 {

struct Atsc3Remux::Impl {
    struct Comp {
        int id = 0;
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::vector<uint8_t>> chunks;
        size_t pos = 0;                 // read position in chunks.front()
        bool ended = false;
        AVFormatContext* ic = nullptr;
        AVIOContext* io = nullptr;
        std::thread th;
        bool opened = false, failed = false;
        std::vector<int> outStream;     // input stream index -> output stream index or -1
        std::deque<AVPacket*> q;        // packets waiting for the muxer (guarded by Impl::pmu)
        bool done = false;
        std::vector<uint8_t> init;      // the init segment already given to the demuxer
    };

    std::mutex mu;                      // components map, state
    std::map<int, std::unique_ptr<Comp>> comps;
    std::atomic<bool> stopping{false};
    std::string err;
    std::vector<std::string> dropped;

    // packet exchange between the demuxer threads and the muxer thread
    std::mutex pmu;
    std::condition_variable pcv;

    // output bytes
    std::mutex omu;
    std::condition_variable ocv;
    std::deque<uint8_t> out;
    bool outEnded = false;
    int startupWaitMs = 4000;
    std::atomic<int> nStreams{0};
    std::atomic<bool> hdr{false};
    std::thread muxThread;
    bool muxStarted = false;

    static int readCb(void* opaque, uint8_t* buf, int size) {
        Comp* c = (Comp*)opaque;
        std::unique_lock<std::mutex> lk(c->mu);
        for (;;) {
            if (!c->chunks.empty()) {
                auto& f = c->chunks.front();
                int n = (int)std::min<size_t>(size, f.size() - c->pos);
                std::memcpy(buf, f.data() + c->pos, n);
                c->pos += n;
                if (c->pos >= f.size()) { c->chunks.pop_front(); c->pos = 0; }
                return n;
            }
            if (c->ended) return AVERROR_EOF;
            c->cv.wait(lk);
        }
    }

    static int writeCb(void* opaque, const uint8_t* buf, int size) {
        Impl* im = (Impl*)opaque;
        std::lock_guard<std::mutex> lk(im->omu);
        // a reader that is far behind loses the oldest bytes instead of growing without limit
        if (im->out.size() > (64u << 20)) im->out.erase(im->out.begin(), im->out.begin() + (16u << 20));
        im->out.insert(im->out.end(), buf, buf + size);
        im->ocv.notify_all();
        return size;
    }

    void demuxLoop(Comp* c) {
        const int kBuf = 1 << 16;
        uint8_t* buf = (uint8_t*)av_malloc(kBuf);
        c->io = avio_alloc_context(buf, kBuf, 0, c, &Impl::readCb, nullptr, nullptr);
        c->ic = avformat_alloc_context();
        c->ic->pb = c->io;
        c->ic->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&c->ic, "", av_find_input_format("mov"), nullptr) < 0) {
            std::lock_guard<std::mutex> lk(pmu);
            c->failed = true; c->done = true;
            pcv.notify_all();
            return;
        }
        // the init segment carries the codec parameters; no probing of the media that follows
        {
            std::lock_guard<std::mutex> lk(pmu);
            c->opened = true;
            pcv.notify_all();
        }
        AVPacket* pkt = av_packet_alloc();
        for (;;) {   // runs until the data is used up: stop() marks the input as ended, so what was delivered is still processed
            int r = av_read_frame(c->ic, pkt);
            if (r < 0) break;
            std::unique_lock<std::mutex> lk(pmu);
            int os = pkt->stream_index < (int)c->outStream.size() ? c->outStream[pkt->stream_index] : -1;
            if (os < 0 && !hdr) {   // the header is not written yet: keep early packets, the stream map is applied by the muxer
                AVPacket* cp = av_packet_alloc();
                av_packet_ref(cp, pkt);
                c->q.push_back(cp);
                pcv.notify_all();
            } else if (os >= 0) {
                AVPacket* cp = av_packet_alloc();
                av_packet_ref(cp, pkt);
                c->q.push_back(cp);
                pcv.notify_all();
                // do not let one component run far ahead of the muxer
                while (c->q.size() > 4000 && !stopping.load()) pcv.wait_for(lk, std::chrono::milliseconds(50));
            }
            lk.unlock();
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        std::lock_guard<std::mutex> lk(pmu);
        c->done = true;
        pcv.notify_all();
    }

    void fail(const std::string& m) { std::lock_guard<std::mutex> lk(mu); if (err.empty()) err = m; }

    static int64_t dts90(AVStream* s, const AVPacket* p) {
        int64_t t = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        return t == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(t, s->time_base, AVRational{1, 90000});
    }

    void muxLoop() {
        // wait for the init segments
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Comp*> cs;
        {
            std::unique_lock<std::mutex> lk(pmu);
            for (;;) {
                if (stopping) return;
                int open = 0, total = 0, bad = 0;
                { std::lock_guard<std::mutex> l2(mu); for (auto& c : comps) { total++; open += c.second->opened; bad += c.second->failed; } }
                bool first = open > 0;
                auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (open + bad >= total && total > 0 && open > 0) break;
                if (first && waited > startupWaitMs) break;
                pcv.wait_for(lk, std::chrono::milliseconds(50));
            }
        }
        { std::lock_guard<std::mutex> l2(mu); for (auto& c : comps) if (c.second->opened) cs.push_back(c.second.get()); }
        AVFormatContext* oc = nullptr;
        if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0 || !oc) { fail("the transport stream muxer is not available"); finishOut(); return; }
        uint8_t* obuf = (uint8_t*)av_malloc(1 << 16);
        AVIOContext* oio = avio_alloc_context(obuf, 1 << 16, 1, this, nullptr, &Impl::writeCb, nullptr);
        oc->pb = oio;
        oc->flags |= AVFMT_FLAG_CUSTOM_IO;
        std::vector<AVBSFContext*> bsfs;
        std::vector<std::vector<AVStream*>> inStreams(cs.size());
        for (size_t ci = 0; ci < cs.size(); ci++) {
            Comp* c = cs[ci];
            c->outStream.assign(c->ic->nb_streams, -1);
            for (unsigned si = 0; si < c->ic->nb_streams; si++) {
                AVStream* in = c->ic->streams[si];
                const AVCodecParameters* cp = in->codecpar;
                if (cp->codec_type != AVMEDIA_TYPE_VIDEO && cp->codec_type != AVMEDIA_TYPE_AUDIO) continue;
                AVStream* os = avformat_new_stream(oc, nullptr);
                if (!os || avcodec_parameters_copy(os->codecpar, cp) < 0) continue;
                os->codecpar->codec_tag = 0;
                os->time_base = AVRational{1, 90000};
                c->outStream[si] = os->index;
                // Video: the parameter sets in front of every picture, so that a receiver that joins at any point can start decoding
                // (MP4 keeps them in the header and a transport stream normally repeats them at every random access point only)
                if ((int)bsfs.size() <= os->index) bsfs.resize(os->index + 1, nullptr);
                if (cp->codec_id == AV_CODEC_ID_HEVC || cp->codec_id == AV_CODEC_ID_H264) {
                    AVBSFContext* bsf = nullptr;
                    const char* chain = cp->codec_id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb" : "h264_mp4toannexb";
                    if (av_bsf_list_parse_str(chain, &bsf) >= 0 && bsf) {
                        avcodec_parameters_copy(bsf->par_in, cp);
                        bsf->time_base_in = in->time_base;
                        if (av_bsf_init(bsf) >= 0) { avcodec_parameters_copy(os->codecpar, bsf->par_out); os->codecpar->codec_tag = 0; bsfs[os->index] = bsf; }
                        else av_bsf_free(&bsf);
                    }
                }
                inStreams[ci].push_back(in);
            }
        }
        if (oc->nb_streams == 0) { fail("no picture or sound in the stream"); finishOut(); avformat_free_context(oc); return; }
        if (avformat_write_header(oc, nullptr) < 0) { fail("cannot start the transport stream"); finishOut(); avformat_free_context(oc); return; }
        nStreams = (int)oc->nb_streams;
        hdr = true;
        int64_t base = INT64_MIN;
        struct CompClock { int64_t lastRaw = INT64_MIN; int epoch = 0; };
        std::map<Comp*, CompClock> clocks;
        std::vector<int64_t> epochBases;
        std::vector<int64_t> lastDts(oc->nb_streams, INT64_MIN);
        long idle = 0;
        for (;;) {
            Comp* pick = nullptr;
            int64_t best = INT64_MAX;
            AVPacket* pk = nullptr;
            {
                std::unique_lock<std::mutex> lk(pmu);
                auto ready = [&]() {
                    int waiting = 0, have = 0;
                    for (Comp* c : cs) { if (!c->q.empty()) have++; else if (!c->done) waiting++; }
                    return have > 0 && waiting == 0;
                };
                if (!ready() && !stopping) pcv.wait_for(lk, std::chrono::milliseconds(300));
                bool any = false, allDone = true;
                for (Comp* c : cs) { if (!c->q.empty()) any = true; if (!c->done || !c->q.empty()) allDone = false; }
                if (allDone) break;
                if (!any) continue;
                for (Comp* c : cs) {
                    if (c->q.empty()) continue;
                    AVPacket* h = c->q.front();
                    // streams of this component were mapped when its first packet was queued
                    AVStream* in = c->ic->streams[h->stream_index];
                    int64_t d = dts90(in, h);
                    if (d == INT64_MIN) d = 0;
                    if (!epochBases.empty()) {   // compare on the output clock: after a clock jump the old and the new clock must not be mixed
                        const CompClock& cc = clocks[c];
                        int ep = cc.epoch;
                        if (cc.lastRaw != INT64_MIN && d < cc.lastRaw - 90000 * 5) ep++;
                        if (ep < (int)epochBases.size()) d -= epochBases[ep];
                        else { int64_t top = 0; for (int64_t v : lastDts) top = std::max(top, v); d = top + 3600; }
                    }
                    if (d < best) { best = d; pick = c; }
                }
                if (!pick) continue;
                // a component that has not delivered yet is waited for, but only for a short while
                if (!ready() && !stopping) { if (++idle < 9) continue; }   // segment delivery is bursty: a component may be a segment behind another, so wait up to about 2.7 s
                idle = 0;
                pk = pick->q.front();
                pick->q.pop_front();
                pcv.notify_all();
            }
            AVStream* in = pick->ic->streams[pk->stream_index];
            int os = pk->stream_index < (int)pick->outStream.size() ? pick->outStream[pk->stream_index] : -1;
            if (os >= 0) {
                if (base == INT64_MIN) base = best - 90000 * 2;   // start two seconds in: priming frames and reordered pictures must not get negative timestamps
                AVStream* ostr = oc->streams[os];
                av_packet_rescale_ts(pk, in->time_base, ostr->time_base);
                // the broadcast clock jumped back (a looped recording or a restarted encoder): carry on from where the output stopped.
                // Each component notices the jump on its own, because one can still be sending old-clock packets when the other has restarted.
                int64_t raw = pk->dts != AV_NOPTS_VALUE ? pk->dts : pk->pts;
                CompClock& cc = clocks[pick];
                if (raw != AV_NOPTS_VALUE) {
                    if (cc.lastRaw != INT64_MIN && raw < cc.lastRaw - 90000 * 5) {
                        cc.epoch++;
                        if (cc.epoch >= (int)epochBases.size()) {
                            int64_t top = INT64_MIN;
                            for (int64_t v : lastDts) top = std::max(top, v);
                            epochBases.push_back(raw - (top == INT64_MIN ? 0 : top + 3600));
                        }
                    }
                    cc.lastRaw = raw;
                }
                if (epochBases.empty()) epochBases.push_back(base);
                int64_t off = epochBases[std::min<size_t>(cc.epoch, epochBases.size() - 1)];
                if (pk->pts != AV_NOPTS_VALUE) pk->pts -= off;
                if (pk->dts != AV_NOPTS_VALUE) pk->dts -= off;
                if (pk->dts == AV_NOPTS_VALUE) pk->dts = pk->pts;
                if (pk->dts != AV_NOPTS_VALUE && pk->dts <= lastDts[os] && lastDts[os] != INT64_MIN) {   // keep the stream monotonic
                    int64_t shift = lastDts[os] + 1 - pk->dts;
                    pk->dts += shift;
                    if (pk->pts != AV_NOPTS_VALUE) pk->pts += shift;
                }
                if (pk->pts != AV_NOPTS_VALUE && pk->dts != AV_NOPTS_VALUE && pk->pts < pk->dts) pk->pts = pk->dts;
                lastDts[os] = pk->dts;
                pk->stream_index = os;
                if (pk->pts != AV_NOPTS_VALUE && pk->pts >= 0) {
                    AVBSFContext* bsf = os < (int)bsfs.size() ? bsfs[os] : nullptr;
                    if (!bsf) av_interleaved_write_frame(oc, pk);
                    else if (av_bsf_send_packet(bsf, pk) >= 0) {   // the filter takes the packet over
                        AVPacket* o2 = av_packet_alloc();
                        while (av_bsf_receive_packet(bsf, o2) >= 0) { o2->stream_index = os; av_interleaved_write_frame(oc, o2); av_packet_unref(o2); }
                        av_packet_free(&o2);
                    }
                }
            }
            av_packet_free(&pk);
        }
        av_write_trailer(oc);
        for (auto* b : bsfs) if (b) av_bsf_free(&b);
        avio_flush(oio);
        av_freep(&oio->buffer);
        avio_context_free(&oio);
        avformat_free_context(oc);
        finishOut();
    }

    void finishOut() {
        std::lock_guard<std::mutex> lk(omu);
        outEnded = true;
        ocv.notify_all();
    }

    Comp* find(int id, bool create) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = comps.find(id);
        if (it != comps.end()) return it->second.get();
        if (!create) return nullptr;
        auto c = std::make_unique<Comp>();
        c->id = id;
        Comp* r = c.get();
        comps[id] = std::move(c);
        return r;
    }
};

Atsc3Remux::Atsc3Remux() : p_(new Impl) {}

Atsc3Remux::~Atsc3Remux() { stop(); }

void Atsc3Remux::addComponent(int id) { p_->find(id, true); }

void Atsc3Remux::push(int id, const std::vector<uint8_t>& data) {
    Impl::Comp* c = p_->find(id, true);
    {
        std::lock_guard<std::mutex> lk(c->mu);
        // an init segment that comes round again (a repeating or looped broadcast) would confuse the running demuxer: it has it already
        if (data.size() >= 8 && (!memcmp(&data[4], "ftyp", 4) || !memcmp(&data[4], "moov", 4))) {
            if (!c->init.empty() && c->init == data) return;
            c->init = data;
        }
        c->chunks.push_back(data);
        if (!c->th.joinable()) c->th = std::thread([this, c] { p_->demuxLoop(c); });
    }
    c->cv.notify_all();
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (!p_->muxStarted) { p_->muxStarted = true; p_->muxThread = std::thread([this] { p_->muxLoop(); }); }
    }
}

int Atsc3Remux::read(uint8_t* buf, int size) {
    std::unique_lock<std::mutex> lk(p_->omu);
    for (;;) {
        if (!p_->out.empty()) {
            int n = (int)std::min<size_t>(size, p_->out.size());
            std::copy(p_->out.begin(), p_->out.begin() + n, buf);
            p_->out.erase(p_->out.begin(), p_->out.begin() + n);
            return n;
        }
        if (p_->outEnded) return 0;
        p_->ocv.wait_for(lk, std::chrono::milliseconds(200));
    }
}

int Atsc3Remux::readTimed(uint8_t* buf, int size, int timeoutMs) {
    std::unique_lock<std::mutex> lk(p_->omu);
    if (p_->out.empty() && !p_->outEnded) p_->ocv.wait_for(lk, std::chrono::milliseconds(timeoutMs));
    if (!p_->out.empty()) {
        int n = (int)std::min<size_t>(size, p_->out.size());
        std::copy(p_->out.begin(), p_->out.begin() + n, buf);
        p_->out.erase(p_->out.begin(), p_->out.begin() + n);
        return n;
    }
    return p_->outEnded ? -1 : 0;
}

void Atsc3Remux::stop() {
    if (p_->stopping.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& c : p_->comps) { std::lock_guard<std::mutex> l2(c.second->mu); c.second->ended = true; c.second->cv.notify_all(); }
    }
    p_->pcv.notify_all();
    p_->ocv.notify_all();
    for (auto& c : p_->comps) if (c.second->th.joinable()) c.second->th.join();
    if (p_->muxThread.joinable()) p_->muxThread.join();
    p_->finishOut();   // also when no data ever arrived
    for (auto& c : p_->comps) {
        for (auto* pk : c.second->q) av_packet_free(&pk);
        c.second->q.clear();
        if (c.second->ic) avformat_close_input(&c.second->ic);
        if (c.second->io) { av_freep(&c.second->io->buffer); avio_context_free(&c.second->io); }
    }
}

std::string Atsc3Remux::error() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->err; }
int Atsc3Remux::streamCount() const { return p_->nStreams; }
bool Atsc3Remux::started() const { return p_->hdr; }
std::vector<std::string> Atsc3Remux::droppedCodecs() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->dropped; }
void Atsc3Remux::setStartupWaitMs(int ms) { p_->startupWaitMs = ms; }

} // namespace dect2
