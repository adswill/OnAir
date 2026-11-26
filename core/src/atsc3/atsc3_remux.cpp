// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "dect2/common/thread_name.h"
#include "dect2/atsc3/atsc3_remux.h"
#include "dect2/audio/ac4_dec.h"
#include "dect2/audio/dolby_audio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/mathematics.h>
}

namespace dect2 {

namespace {

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

struct Box { std::array<char, 4> type; size_t start, body, end; };   // body: where the content starts (after the size, type and a 64-bit size)

// The boxes of [from, to) in d, end to end. False when they do not fill it exactly.
bool boxesOf(const uint8_t* d, size_t from, size_t to, std::vector<Box>& out) {
    out.clear();
    size_t pos = from;
    while (pos < to) {
        if (to - pos < 8) return false;
        uint64_t size = rd32(d + pos);
        size_t hdr = 8;
        if (size == 1) {
            if (to - pos < 16) return false;
            size = (uint64_t)rd32(d + pos + 8) << 32 | rd32(d + pos + 12);
            hdr = 16;
        } else if (size == 0) size = to - pos;   // to the end
        if (size < hdr || size > to - pos) return false;
        Box b;
        std::copy_n(d + pos + 4, 4, b.type.begin());
        b.start = pos; b.body = pos + hdr; b.end = pos + (size_t)size;
        out.push_back(b);
        pos = b.end;
    }
    return true;
}

bool is(const Box& b, const char* t) { return !memcmp(b.type.data(), t, 4); }

}   // namespace

// The default sample sizes of the tracks (moov/mvex/trex), for the check of the media segments.
// The track's language (the 'mdhd' box of its initialization segment, ISO 639-2, e.g. "eng", "spa") onto the output stream, so that the
// transport stream carries it (ISO 639 language descriptor) and the player can name its sound tracks. "und" (not given) is not copied.
static void copyLanguage(const AVStream* in, AVStream* os) {
    const AVDictionaryEntry* l = av_dict_get(in->metadata, "language", nullptr, 0);
    const char* v = l ? l->value : nullptr;
    if (v && v[0] && v[1] && v[2] && !v[3] && strcmp(v, "und") != 0) av_dict_set(&os->metadata, "language", v, 0);   // three letters
}

static std::map<uint32_t, uint32_t> trexSizes(const std::vector<uint8_t>& init) {
    std::map<uint32_t, uint32_t> r;
    std::vector<Box> top, moov, mvex;
    if (!boxesOf(init.data(), 0, init.size(), top)) return r;
    for (auto& a : top) {
        if (!is(a, "moov") || !boxesOf(init.data(), a.body, a.end, moov)) continue;
        for (auto& b : moov) {
            if (!is(b, "mvex") || !boxesOf(init.data(), b.body, b.end, mvex)) continue;
            for (auto& t : mvex) if (is(t, "trex") && t.end - t.body >= 24) r[rd32(&init[t.body + 4])] = rd32(&init[t.body + 16]);
        }
    }
    return r;
}

// Whether a media segment can be given to the MP4 demuxer: whole boxes from end to end, every moof followed by an mdat, and the samples that
// the track runs of a moof point to inside that mdat. A segment that a loss cut short or that starts in the middle (a moof without its mdat,
// an mdat that claims more bytes than there are) would make the demuxer read the next segments as the rest of this one: it then never finds a
// box again and the component gives nothing for good. `samples` gets the number of samples in the segment.
static bool segmentUsable(const std::vector<uint8_t>& seg, const std::map<uint32_t, uint32_t>& trex, long* samples) {
    *samples = 0;
    const uint8_t* d = seg.data();
    std::vector<Box> top, moof, traf;
    if (seg.size() < 8 || !boxesOf(d, 0, seg.size(), top)) return false;
    for (size_t i = 0; i < top.size(); i++) {
        if (!is(top[i], "moof")) continue;
        const Box* mdat = nullptr;
        for (size_t j = i + 1; j < top.size() && !is(top[j], "moof") && !mdat; j++) if (is(top[j], "mdat")) mdat = &top[j];
        if (!mdat || !boxesOf(d, top[i].body, top[i].end, moof)) return false;
        bool firstTraf = true;
        for (auto& tb : moof) {
            if (!is(tb, "traf")) continue;
            if (!boxesOf(d, tb.body, tb.end, traf)) return false;
            uint32_t tfFlags = 0, defSize = 0;
            bool haveDef = false, absolute = false;
            for (auto& x : traf) {
                if (!is(x, "tfhd")) continue;
                if (x.end - x.body < 8) return false;
                tfFlags = rd32(d + x.body) & 0xffffff;
                const uint32_t track = rd32(d + x.body + 4);
                size_t p = x.body + 8;
                if (tfFlags & 0x1) { absolute = true; p += 8; }   // base-data-offset: positions in the file, not checked here
                if (tfFlags & 0x2) p += 4;
                if (tfFlags & 0x8) p += 4;
                if (tfFlags & 0x10) { if (p + 4 > x.end) return false; defSize = rd32(d + p); haveDef = true; }
                if (!haveDef) { auto it = trex.find(track); if (it != trex.end()) { defSize = it->second; haveDef = true; } }
            }
            // where the data of the first run starts without a data offset is not checked (it depends on the traf before); with one, it is
            // counted from the moof (default-base-is-moof, or the first traf of the moof)
            const bool fromMoof = !absolute && ((tfFlags & 0x20000) || firstTraf);
            firstTraf = false;
            bool havePos = false;
            uint64_t pos = 0;
            for (auto& x : traf) {
                if (!is(x, "trun")) continue;
                if (x.end - x.body < 8) return false;
                const uint32_t fl = rd32(d + x.body) & 0xffffff, n = rd32(d + x.body + 4);
                size_t p = x.body + 8;
                int64_t off = 0;
                const bool hasOff = fl & 0x1;
                if (hasOff) { if (p + 4 > x.end) return false; off = (int32_t)rd32(d + p); p += 4; }
                if (fl & 0x4) p += 4;
                const size_t per = 4 * (!!(fl & 0x100) + !!(fl & 0x200) + !!(fl & 0x400) + !!(fl & 0x800));
                if (p > x.end || (uint64_t)n * per > x.end - p) return false;
                *samples += n;
                bool sized = (fl & 0x200) || haveDef;
                uint64_t total = 0;
                if (fl & 0x200) {
                    const size_t so = 4 * !!(fl & 0x100);
                    for (uint32_t k = 0; k < n; k++) total += rd32(d + p + k * per + so);
                } else total = (uint64_t)n * defSize;
                if (hasOff && fromMoof) { pos = (uint64_t)((int64_t)top[i].start + off); havePos = true; }
                else if (!hasOff && !havePos) sized = false;   // starts where the data of another track run ends: not known here
                if (sized && havePos) {
                    if (pos < mdat->body || pos > mdat->end || total > mdat->end - pos) return false;
                    pos += total;
                } else havePos = false;
            }
        }
    }
    return true;
}

struct Atsc3Remux::Impl {
    struct Chunk {
        std::vector<uint8_t> data;
        bool samples = false;           // a media segment with samples in it (the demuxer gives packets from it)
    };
    struct Comp {
        int id = 0;
        std::mutex mu;
        std::condition_variable cv;
        std::deque<Chunk> chunks;
        size_t pos = 0;                 // read position in chunks.front()
        bool ended = false;
        // media segments with samples that the demuxer has read to the end since it gave its last packet: a demuxer that has lost the box
        // boundaries reads on and gives nothing, and is started again on the next segment
        std::atomic<int> segsNoPacket{0};
        bool resync = false;            // the read was stopped for that (guarded by mu)
        std::map<uint32_t, uint32_t> trex;   // the default sample sizes of the init segment's tracks (guarded by mu)
        AVFormatContext* ic = nullptr;  // changed (under Impl::pmu) when the demuxer starts again; the old ones are kept until stop()
        std::vector<AVFormatContext*> oldIc;
        AVIOContext* io = nullptr;
        std::vector<AVIOContext*> oldIo;
        std::thread th;
        bool opened = false, failed = false;
        std::vector<int> outStream;     // input stream index -> output stream index or -1
        std::deque<AVPacket*> q;        // packets waiting for the muxer (guarded by Impl::pmu)
        bool done = false;
        bool av = false, video = false;   // the init segment has a picture or sound track / a picture track (set when opened)
        bool playable = false;          // and one of them can go into the output (not only AC-4 sound without the AC-4 library)
        std::vector<uint8_t> init;      // the init segment already given to the demuxer
        bool ac4 = false;               // the init segment has an AC-4 sample entry ('ac-4')
        int ac4Channels = 0;            // its channel count (a hint)
    };

    std::mutex mu;                      // components map, state
    std::map<int, std::unique_ptr<Comp>> comps;
    std::atomic<bool> stopping{false};
    std::string err;
    std::vector<std::string> dropped;

    // packet exchange between the demuxer threads and the muxer thread
    std::mutex pmu;
    std::condition_variable pcv;
    int opens = 0;                      // components opened so far (guarded by pmu): the muxer looks for new ones when it changes

    // output bytes
    std::mutex omu;
    std::condition_variable ocv;
    std::deque<uint8_t> out;
    bool outEnded = false;
    int startupWaitMs = 4000;
    std::atomic<bool> realtime{false};
    std::atomic<int> latencyMs{1200};
    std::atomic<int> nStreams{0};
    std::atomic<bool> hdr{false};
    std::thread muxThread;
    bool muxStarted = false;

    static constexpr int kLostSegments = 3;

    static int readCb(void* opaque, uint8_t* buf, int size) {
        Comp* c = (Comp*)opaque;
        std::unique_lock<std::mutex> lk(c->mu);
        for (;;) {
            if (!c->chunks.empty()) {
                if (c->pos == 0 && c->segsNoPacket.load() >= kLostSegments) {   // at the start of a segment: the demuxer starts again here
                    c->resync = true;
                    return AVERROR_INVALIDDATA;
                }
                auto& f = c->chunks.front();
                int n = (int)std::min<size_t>(size, f.data.size() - c->pos);
                std::copy_n(f.data.data() + c->pos, n, buf);
                c->pos += n;
                if (c->pos >= f.data.size()) {
                    if (f.samples) c->segsNoPacket++;
                    c->chunks.pop_front();
                    c->pos = 0;
                }
                return n;
            }
            if (c->ended) return AVERROR_EOF;
            c->cv.wait(lk);
        }
    }

    // FFmpeg 7 made the data of the write callback const; older versions (Ubuntu 24.04 has 6.1) want it without
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static int writeCb(void* opaque, const uint8_t* buf, int size) {
#else
    static int writeCb(void* opaque, uint8_t* buf, int size) {
#endif
        Impl* im = (Impl*)opaque;
        std::lock_guard<std::mutex> lk(im->omu);
        // a reader that is far behind loses the oldest bytes instead of growing without limit
        if (im->out.size() > (64u << 20)) {
            DIAG_EVERY(698131, 10.0, "The ATSC 3.0 transport stream output buffer holds 64 MB that nobody has read, so its oldest 16 MB are discarded and the picture will glitch.");
            im->out.erase(im->out.begin(), im->out.begin() + (16u << 20));
        }
        im->out.insert(im->out.end(), buf, buf + size);
        im->ocv.notify_all();
        return size;
    }

    // A demuxer on the component's input from where it is now. Null when FFmpeg cannot open it.
    static AVFormatContext* openDemuxer(Comp* c, AVIOContext** io) {
        const int kBuf = 1 << 16;
        uint8_t* buf = (uint8_t*)av_malloc(kBuf);
        *io = avio_alloc_context(buf, kBuf, 0, c, &Impl::readCb, nullptr, nullptr);
        AVFormatContext* ic = avformat_alloc_context();
        ic->pb = *io;
        ic->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&ic, "", av_find_input_format("mov"), nullptr) < 0) return nullptr;   // frees ic
        return ic;
    }

    // The demuxer failed or lost the box boundaries (a damaged segment that got past the check in push()): a new one starts on the next
    // segment, with the init segment in front of it. The streams are those of the same init segment, so the output goes on as it was.
    // The old demuxer is kept until stop(): the muxer may still be looking at its streams. Returns false when stopped or when it cannot open.
    bool restartDemuxer(Comp* c) {
        {
            std::lock_guard<std::mutex> lk(c->mu);
            if (c->ended || c->init.empty()) return false;
            if (c->pos > 0) { c->chunks.pop_front(); c->pos = 0; }   // the rest of the segment it was in
            c->chunks.push_front(Chunk{c->init, false});
            c->resync = false;
            c->segsNoPacket = 0;
        }
        AVIOContext* io = nullptr;
        AVFormatContext* ic = openDemuxer(c, &io);
        std::lock_guard<std::mutex> lk(pmu);
        c->oldIo.push_back(c->io);
        c->io = io;
        if (!ic) return false;
        c->oldIc.push_back(c->ic);
        c->ic = ic;
        return true;
    }

    void demuxLoop(Comp* c) {
        c->ic = openDemuxer(c, &c->io);
        if (!c->ic) {
            DIAG(413547, "An ATSC 3.0 media component has an initialization segment that FFmpeg cannot open as MP4, so this component is left out of the output.");
            std::lock_guard<std::mutex> lk(pmu);
            c->failed = true; c->done = true;
            pcv.notify_all();
            return;
        }
        // the init segment carries the codec parameters; no probing of the media that follows
        bool playable = false;
        for (unsigned si = 0; si < c->ic->nb_streams && !playable; si++) {
            const AVCodecParameters* cp = c->ic->streams[si]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) playable = true;
            else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) playable = !isAc4(c, cp) || ac4Convertible();
        }
        {
            std::lock_guard<std::mutex> lk(pmu);
            c->opened = true;
            c->playable = playable;
            for (unsigned si = 0; si < c->ic->nb_streams; si++) {
                const int t = c->ic->streams[si]->codecpar->codec_type;
                if (t == AVMEDIA_TYPE_VIDEO) c->video = true;
                if (t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO) c->av = true;
            }
            opens++;
            pcv.notify_all();
        }
        AVPacket* pkt = av_packet_alloc();
        for (;;) {   // runs until the data is used up: stop() marks the input as ended, so what was delivered is still processed
            int r = av_read_frame(c->ic, pkt);
            if (r < 0) {
                bool ended, lost;
                { std::lock_guard<std::mutex> lk(c->mu); ended = c->ended && c->chunks.empty(); lost = c->resync; }
                if (ended) break;
                // not the end of the input: a segment the demuxer could not read. Without a new demuxer this component would give nothing more.
                DIAG_DEBUG_EVERY(552817, 10.0, "The MP4 demuxer of ATSC 3.0 component %d %s, so it starts again on the next media segment.", c->id, lost ? "read three segments without finding a sample (it lost the box boundaries)" : "stopped on a segment it could not read");
                if (!restartDemuxer(c)) break;
                continue;
            }
            c->segsNoPacket = 0;
            std::unique_lock<std::mutex> lk(pmu);
            int os = pkt->stream_index < (int)c->outStream.size() ? c->outStream[pkt->stream_index] : -1;
            // a component the output does not hold yet (no stream map: the muxer waits for it or starts again with it): its packets are kept
            if (os >= 0 || (c->outStream.empty() && c->playable)) {
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

    // AC-4 sound (ATSC A/342-2, sample entry 'ac-4')
    static bool isAc4(const Comp* c, const AVCodecParameters* cp) {
        return cp->codec_type == AVMEDIA_TYPE_AUDIO && (cp->codec_id == AV_CODEC_ID_AC4 || cp->codec_tag == MKTAG('a', 'c', '-', '4') || c->ac4);
    }
    // whether AC-4 sound can be converted for the output now (the optional AC-4 library and an AC-3 encoder are there)
    static bool ac4Convertible() {
        if (!ac4Available()) return false;
        Ac4ToAc3 probe;
        return probe.open(2);
    }

    void fail(const std::string& m) { std::lock_guard<std::mutex> lk(mu); if (err.empty()) err = m; }

    static int64_t dts90(AVStream* s, const AVPacket* p) {
        int64_t t = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        return t == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(t, s->time_base, AVRational{1, 90000});
    }

    // Waits for the components of the first output: until every known one is open and a picture and a sound track are there, or startupWaitMs
    // after the first data when at least one has a picture or sound track. A component whose init segment comes later (lost or late on air)
    // is taken in by starting the output again (runOutput). Returns false when stopped.
    bool waitFirst() {
        auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lk(pmu);
        for (;;) {
            if (stopping) return false;
            int open = 0, total = 0, bad = 0;
            bool av = false, video = false, audio = false;
            {
                std::lock_guard<std::mutex> l2(mu);
                for (auto& c : comps) {
                    total++; open += c.second->opened; bad += c.second->failed;
                    if (c.second->opened && c.second->av) { av = true; video = video || c.second->video; audio = audio || !c.second->video; }
                }
            }
            auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (av && open + bad >= total && video && audio) return true;
            if (av && waited > startupWaitMs) return true;
            pcv.wait_for(lk, std::chrono::milliseconds(50));
        }
    }

    // an open component that the running output does not hold and that would add a stream to it (caller holds pmu). One that would add
    // nothing (AC-4 sound without the AC-4 library) does not start the output again: that would only break the stream for no gain.
    bool newComponent() {
        std::lock_guard<std::mutex> l2(mu);
        for (auto& c : comps) if (c.second->opened && c.second->playable && c.second->outStream.empty()) return true;
        return false;
    }

    // The clock of the output, kept when the output starts again so that the components stay in step and the timestamps go on.
    struct Timeline {
        int64_t base = INT64_MIN;
        struct CompClock { int64_t lastRaw = INT64_MIN; int epoch = 0; };
        std::map<Comp*, CompClock> clocks;
        std::vector<int64_t> epochBases;
        std::map<Comp*, int64_t> lastOut;   // the output time of the last packet taken from each component
        int64_t topOut = INT64_MIN;         // the latest time written on any stream
        bool paced = false;
        std::chrono::steady_clock::time_point wall0;
        int64_t dts0 = 0;
    };
    enum class RunEnd { Ended, Restart, Nothing };

    void muxLoop() {
        if (!waitFirst()) return;
        Timeline tl;
        for (int run = 0;; run++) {
            const RunEnd e = runOutput(run, tl);
            if (e == RunEnd::Ended) break;
            if (e == RunEnd::Nothing) {   // nothing to play yet: wait for another component (a picture whose init segment was lost)
                std::unique_lock<std::mutex> lk(pmu);
                while (!stopping && !newComponent()) pcv.wait_for(lk, std::chrono::milliseconds(100));
                if (stopping) break;
            }
            DIAG_DEBUG_EVERY(415873, 10.0, "An ATSC 3.0 media component started after the transport stream output (its init segment was late or lost), so the output starts again with it (output %d).", run + 2);
        }
        finishOut();
    }

    // One transport stream output over the components open now. Ends when all of them have ended, or when another component opens
    // (Restart; the stream goes on with new tables), or when none of them has a track that can be played (Nothing).
    RunEnd runOutput(int run, Timeline& tl) {
        std::vector<Comp*> cs, fresh;   // fresh: not in the output before this one
        std::map<Comp*, AVFormatContext*> ics;
        {
            std::lock_guard<std::mutex> lk(pmu);
            std::lock_guard<std::mutex> l2(mu);
            for (auto& c : comps) if (c.second->opened) { cs.push_back(c.second.get()); if (c.second->outStream.empty()) fresh.push_back(c.second.get()); }
            // the demuxer of a component may be replaced while this runs (restartDemuxer): its streams are the same, and the old one is kept
            for (Comp* c : cs) ics[c] = c->ic;
        }
        AVFormatContext* oc = nullptr;
        if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0 || !oc) {
            DIAG_ERROR(689053, "FFmpeg has no MPEG transport stream muxer, so the ATSC 3.0 service cannot be remuxed for playback.");
            fail("the transport stream muxer is not available");
            return RunEnd::Ended;
        }
        uint8_t* obuf = (uint8_t*)av_malloc(1 << 16);
        AVIOContext* oio = avio_alloc_context(obuf, 1 << 16, 1, this, nullptr, &Impl::writeCb, nullptr);
        oc->pb = oio;
        oc->flags |= AVFMT_FLAG_CUSTOM_IO;
        std::vector<AVBSFContext*> bsfs;
        std::map<int, std::unique_ptr<Ac4ToAc3>> xcodes;   // output stream index -> AC-4 to AC-3 converter
        std::vector<std::vector<int>> maps(cs.size());
        for (size_t ci = 0; ci < cs.size(); ci++) {
            Comp* c = cs[ci];
            AVFormatContext* ic = ics[c];
            maps[ci].assign(ic->nb_streams, -1);
            for (unsigned si = 0; si < ic->nb_streams; si++) {
                AVStream* in = ic->streams[si];
                const AVCodecParameters* cp = in->codecpar;
                if (cp->codec_type != AVMEDIA_TYPE_VIDEO && cp->codec_type != AVMEDIA_TYPE_AUDIO) continue;
                // AC-4 sound (ATSC A/342-2, sample entry 'ac-4'): the FFmpeg inside OnAir cannot decode it. With the optional AC-4 library it is
                // decoded here and goes on as AC-3; without it the component is left out (and said so), the picture and other sound still play.
                if (isAc4(c, cp)) {
                    ac4NoteSeen("");
                    auto xc = std::make_unique<Ac4ToAc3>();
                    AVStream* os = nullptr;
                    if (xc->open(c->ac4Channels > 0 ? c->ac4Channels : cp->ch_layout.nb_channels) && (os = avformat_new_stream(oc, nullptr))) {
                        os->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                        os->codecpar->codec_id = AV_CODEC_ID_AC3;
                        os->codecpar->sample_rate = xc->outSampleRate();
                        os->codecpar->bit_rate = xc->outBitRate();
                        av_channel_layout_default(&os->codecpar->ch_layout, xc->outChannels());
                        os->time_base = AVRational{1, 90000};
                        copyLanguage(in, os);
                        maps[ci][si] = os->index;
                        if ((int)bsfs.size() <= os->index) bsfs.resize(os->index + 1, nullptr);
                        xcodes[os->index] = std::move(xc);
                    } else {
                        ac4NoteDropped();
                        DIAG_DEBUG_EVERY(683372, 10.0, "The AC-4 sound of an ATSC 3.0 component could not be opened (no AC-4 decoder library or no output stream), so that sound is left out.");
                        std::lock_guard<std::mutex> lk(mu);
                        if (std::find(dropped.begin(), dropped.end(), "AC-4") == dropped.end()) dropped.push_back("AC-4");
                    }
                    continue;
                }
                AVStream* os = avformat_new_stream(oc, nullptr);
                if (!os || avcodec_parameters_copy(os->codecpar, cp) < 0) {
                    DIAG_DEBUG_EVERY(718926, 10.0, "FFmpeg could not create an output stream for track %u of an ATSC 3.0 component, so that track is left out.", si);
                    continue;
                }
                os->codecpar->codec_tag = 0;
                os->time_base = AVRational{1, 90000};
                copyLanguage(in, os);
                maps[ci][si] = os->index;
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
                        else {
                            DIAG_DEBUG_EVERY(273765, 10.0, "The %s bitstream filter did not initialise for an ATSC 3.0 video track, so its parameter sets are not repeated in front of every picture.", chain);
                            av_bsf_free(&bsf);
                        }
                    }
                }
            }
        }
        auto freeOut = [&]() {
            for (auto* b : bsfs) if (b) av_bsf_free(&b);
            avio_flush(oio);
            av_freep(&oio->buffer);
            avio_context_free(&oio);
            avformat_free_context(oc);
        };
        {   // the stream maps; also for a component without a playable track, so that it is not taken for a new one
            std::lock_guard<std::mutex> lk(pmu);
            for (size_t ci = 0; ci < cs.size(); ci++) cs[ci]->outStream = maps[ci];
        }
        // the picture streams of components new to the output start at a key frame; those that were in the output before go on as they were
        std::vector<char> needKey(oc->nb_streams, 0);
        std::vector<int> keyWait(oc->nb_streams, 0);
        for (size_t ci = 0; ci < cs.size(); ci++) {
            if (std::find(fresh.begin(), fresh.end(), cs[ci]) == fresh.end()) continue;
            for (int o : maps[ci]) if (o >= 0 && oc->streams[o]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) needKey[o] = 1;
        }
        // a component that has nothing to put in the output (timed metadata, captions) never delivers a packet: it must not be waited for
        cs.erase(std::remove_if(cs.begin(), cs.end(), [](Comp* c) { for (int o : c->outStream) if (o >= 0) return false; return true; }), cs.end());
        if (oc->nb_streams == 0) {
            DIAG_ERROR(410006, "None of the ATSC 3.0 media components holds a picture or sound track that can be remuxed (%zu components, %zu dropped), so nothing is played until one arrives.", cs.size(), dropped.size());
            fail(!dropped.empty() ? "only AC-4 sound in the stream, and no AC-4 decoder library is installed" : "no picture or sound in the stream");
            freeOut();
            return RunEnd::Nothing;
        }
        // the tables of a new output get a new version, so that a player that keeps the old ones takes the new streams
        AVDictionary* opts = nullptr;
        av_dict_set_int(&opts, "tables_version", run & 31, 0);
        const int wh = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (wh < 0) {
            DIAG_ERROR(406134, "FFmpeg could not write the transport stream header for the ATSC 3.0 service, so nothing is played.");
            fail("cannot start the transport stream");
            freeOut();
            return RunEnd::Ended;
        }
        { std::lock_guard<std::mutex> lk(mu); err.clear(); }
        nStreams = (int)oc->nb_streams;
        hdr = true;
        int64_t& base = tl.base;
        auto& clocks = tl.clocks;
        auto& epochBases = tl.epochBases;
        auto& lastOut = tl.lastOut;
        int64_t& topOut = tl.topOut;
        bool& paced = tl.paced;
        auto& wall0 = tl.wall0;
        int64_t& dts0 = tl.dts0;
        const int64_t startTop = topOut;   // packets of a new component from before this point are too late for the output
        std::vector<int64_t> lastDts(oc->nb_streams, INT64_MIN);
        std::map<int, int64_t> lastAc3;   // last written time of each converted (AC-4) stream;
        long idle = 0;
        std::set<Comp*> silent;   // components waited for in vain, until they send again
        int seenOpens = -1;
        RunEnd end = RunEnd::Ended;
        for (;;) {
            Comp* pick = nullptr;
            int64_t best = INT64_MAX;
            AVPacket* pk = nullptr;
            {
                std::unique_lock<std::mutex> lk(pmu);
                if (!stopping && opens != seenOpens) {
                    seenOpens = opens;
                    if (newComponent()) { end = RunEnd::Restart; break; }
                }
                bool any = false, allDone = true;
                for (Comp* c : cs) { if (!c->q.empty()) any = true; if (!c->done || !c->q.empty()) allDone = false; }
                if (allDone) break;
                if (!any) { if (!stopping) pcv.wait_for(lk, std::chrono::milliseconds(300)); continue; }
                for (Comp* c : cs) {
                    if (c->q.empty()) continue;
                    AVPacket* h = c->q.front();
                    // streams of this component were mapped when its first packet was queued
                    AVStream* in = ics[c]->streams[h->stream_index];
                    const int64_t d0 = dts90(in, h);
                    int64_t d = d0 == INT64_MIN ? 0 : d0;
                    if (!epochBases.empty() && startTop != INT64_MIN && d0 != INT64_MIN && !clocks.count(c)) {
                        // A component new to a running output: find the clock it runs on (the newest one its time fits, within 30 s of where the
                        // output is). One that fits none (another clock, for example after a service change) goes on from where the output is,
                        // instead of having its packets dropped as too old, or written far ahead, until its time catches up.
                        const int64_t kNear = 90000LL * 30;
                        int found = -1;
                        for (int e = (int)epochBases.size() - 1; e >= 0 && found < 0; e--) {
                            const int64_t t = d - epochBases[e];
                            if (t > startTop - kNear && t < startTop + kNear) found = e;
                        }
                        if (found < 0) { epochBases.push_back(d - (startTop + 3600)); found = (int)epochBases.size() - 1; }
                        clocks[c] = Timeline::CompClock{d, found};
                    }
                    if (!epochBases.empty()) {   // compare on the output clock: after a clock jump the old and the new clock must not be mixed
                        const Timeline::CompClock& cc = clocks[c];
                        int ep = cc.epoch;
                        if (cc.lastRaw != INT64_MIN && d < cc.lastRaw - 90000 * 5) ep++;
                        if (ep < (int)epochBases.size()) d -= epochBases[ep];
                        else d = (topOut == INT64_MIN ? 0 : topOut) + 3600;
                    }
                    if (d < best) { best = d; pick = c; }
                }
                if (!pick) continue;
                // A component with nothing queued may still send a packet that is older than the one at hand, so it is waited for, but only for a short
                // while (segment delivery is bursty: a component may be a segment behind another). Once a component has been written past this time
                // its next packet cannot be older, and nothing needs to wait for it: without that every packet waited for the audio, which arrives in
                // bursts and is mostly consumed, and the stream ran far slower than real time.
                // A component that was waited for the full time and still sent nothing is not waited for again until it sends: otherwise every
                // packet would wait the full time for a component that stopped.
                std::vector<Comp*> waitFor;
                for (Comp* c : cs) {
                    if (!c->q.empty() || c->done || silent.count(c)) continue;
                    auto lo = lastOut.find(c);
                    if (lo == lastOut.end() || lo->second < best) waitFor.push_back(c);
                }
                if (!waitFor.empty() && !stopping) {
                    if (++idle < 9) { pcv.wait_for(lk, std::chrono::milliseconds(300)); continue; }
                    silent.insert(waitFor.begin(), waitFor.end());
                }
                idle = 0;
                for (Comp* c : cs) if (!c->q.empty()) silent.erase(c);
                pk = pick->q.front();
                pick->q.pop_front();
                pcv.notify_all();
                // a component taken in by a new output brings the packets it kept while it waited: those older than what was written are dropped
                if (startTop != INT64_MIN && best < startTop && std::find(fresh.begin(), fresh.end(), pick) != fresh.end()) { av_packet_free(&pk); continue; }
                lastOut[pick] = best;
            }
            AVStream* in = ics[pick]->streams[pk->stream_index];
            int os = pk->stream_index < (int)pick->outStream.size() ? pick->outStream[pk->stream_index] : -1;
            // a picture stream new to the output starts at a picture that decodes on its own (the first of a segment): the ones before it
            // would only show as broken pictures. A stream that never marks one is let through after a while.
            if (os >= 0 && os < (int)needKey.size() && needKey[os]) {
                if (!(pk->flags & AV_PKT_FLAG_KEY) && ++keyWait[os] < 600) { av_packet_free(&pk); continue; }
                needKey[os] = 0;
            }
            if (os >= 0) {
                if (base == INT64_MIN) base = best - 90000 * 2;   // start two seconds in: priming frames and reordered pictures must not get negative timestamps
                AVStream* ostr = oc->streams[os];
                av_packet_rescale_ts(pk, in->time_base, ostr->time_base);
                // the broadcast clock jumped back (a looped recording or a restarted encoder): carry on from where the output stopped.
                // Each component notices the jump on its own, because one can still be sending old-clock packets when the other has restarted.
                int64_t raw = pk->dts != AV_NOPTS_VALUE ? pk->dts : pk->pts;
                Timeline::CompClock& cc = clocks[pick];
                if (raw != AV_NOPTS_VALUE) {
                    if (cc.lastRaw != INT64_MIN && raw < cc.lastRaw - 90000 * 5) {
                        cc.epoch++;
                        if (cc.epoch >= (int)epochBases.size()) {
                            epochBases.push_back(raw - (topOut == INT64_MIN ? 0 : topOut + 3600));
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
                if (pk->dts != AV_NOPTS_VALUE) topOut = std::max(topOut, pk->dts);
                if (realtime.load() && pk->dts != AV_NOPTS_VALUE) {   // release the packet when its time has come
                    using clk = std::chrono::steady_clock;
                    if (!paced) { paced = true; wall0 = clk::now() + std::chrono::milliseconds(latencyMs.load()); dts0 = pk->dts; }
                    auto target = wall0 + std::chrono::microseconds((pk->dts - dts0) * 1000000 / 90000);
                    auto now = clk::now();
                    if (now > target + std::chrono::milliseconds(500)) wall0 += now - target;   // behind (nothing arrived for a while): start again from here
                    else if (target > now + std::chrono::seconds(5)) { wall0 -= target - now; target = now; }   // the timestamps jumped ahead: no waiting for them
                    while (!stopping.load() && clk::now() < target) std::this_thread::sleep_for(std::min<clk::duration>(target - clk::now(), std::chrono::milliseconds(20)));
                }
                pk->stream_index = os;
                if (pk->pts != AV_NOPTS_VALUE && pk->pts >= 0) {
                    AVBSFContext* bsf = os < (int)bsfs.size() ? bsfs[os] : nullptr;
                    auto xit = xcodes.find(os);
                    if (xit != xcodes.end()) {
                        std::vector<Ac4TsPacket> outs;
                        xit->second->push(pk->data, (size_t)pk->size, pk->pts, outs);
                        for (auto& o : outs) {
                            AVPacket* o2 = av_packet_alloc();
                            if (o2 && av_new_packet(o2, (int)o.data.size()) >= 0) {
                                memcpy(o2->data, o.data.data(), o.data.size());
                                int64_t t = std::max(o.pts90, lastAc3[os] + 1);
                                o2->pts = o2->dts = t;
                                lastAc3[os] = t;
                                o2->duration = av_rescale(o.samples, 90000, 48000);
                                o2->stream_index = os;
                                av_interleaved_write_frame(oc, o2);
                            }
                            av_packet_free(&o2);
                        }
                    } else if (!bsf) av_interleaved_write_frame(oc, pk);
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
        freeOut();
        return end;
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
        const bool init = data.size() >= 8 && (!memcmp(&data[4], "ftyp", 4) || !memcmp(&data[4], "moov", 4));
        long samples = 0;
        if (init) {
            if (!c->init.empty() && c->init == data) return;
            c->init = data;
            c->trex = trexSizes(data);
            Ac4Dsi dsi;
            int ch = 0;
            if (findAc4InInit(data.data(), data.size(), &dsi, &ch)) {
                c->ac4 = true;
                c->ac4Channels = ch;
                char b[96];
                snprintf(b, sizeof b, "AC-4%s%s", dsi.valid ? (dsi.sampleRate() == 48000 ? ", 48 kHz" : ", 44.1 kHz") : "", ch > 0 ? (ch >= 6 ? ", 5.1 or more" : ", stereo or mono") : "");
                std::string d = b;
                if (dsi.valid) d += ", " + std::to_string(dsi.presentations) + " presentation(s)";
                // Dolby Atmos in AC-4: a presentation with objects / A-JOC or height channels (TS 103 190-2 E.10, dolby_audio.h)
                const Ac4DsiInfo di = parseAc4DsiV1(dsi.raw.data(), dsi.raw.size());
                ac4NoteDsi(di);
                if (di.immersive) d += ", Dolby Atmos (AC-4" + (di.bed.empty() ? std::string() : ", " + di.bed) + ")";
                ac4NoteSeen(d);
            }
        }
        else if (!segmentUsable(data, c->trex, &samples)) {
            DIAG_DEBUG_EVERY(552818, 10.0, "A media segment of ATSC 3.0 component %d is cut short or damaged (%zu bytes, its boxes do not add up), so it is left out instead of being given to the MP4 demuxer.", id, data.size());
            return;
        }
        c->chunks.push_back(Impl::Chunk{data, samples > 0});
        if (!c->th.joinable()) c->th = dect2::namedThread("onair-a3dmx", [this, c] { p_->demuxLoop(c); });
    }
    c->cv.notify_all();
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (!p_->muxStarted) { p_->muxStarted = true; p_->muxThread = dect2::namedThread("onair-a3mux", [this] { p_->muxLoop(); }); }
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
        for (auto* ic : c.second->oldIc) avformat_close_input(&ic);
        for (auto* io : c.second->oldIo) if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        c.second->oldIc.clear();
        c.second->oldIo.clear();
    }
}

std::string Atsc3Remux::error() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->err; }
int Atsc3Remux::streamCount() const { return p_->nStreams; }
bool Atsc3Remux::started() const { return p_->hdr; }
std::vector<std::string> Atsc3Remux::droppedCodecs() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->dropped; }
void Atsc3Remux::setStartupWaitMs(int ms) { p_->startupWaitMs = ms; }
void Atsc3Remux::setRealtime(bool on, int latencyMs) { p_->realtime = on; p_->latencyMs = latencyMs; }

} // namespace dect2
