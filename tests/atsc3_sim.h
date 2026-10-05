// Shared pieces of the ATSC 3.0 simulations: the test MP4 fragments, a sender that turns them into an IP packet stream, ALP and baseband packets.
#pragma once
#include "dect2/atsc3_alp.h"
#include "dect2/atsc3_bb.h"
#include "dect2/atsc3_receiver.h"
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace sim {
using namespace dect2;
using namespace dect2::atsc3;

inline std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline void split(const std::vector<uint8_t>& f, std::vector<uint8_t>& init, std::vector<std::vector<uint8_t>>& segs) {
    size_t pos = 0;
    bool inInit = true;
    while (pos + 8 <= f.size()) {
        uint32_t sz = (f[pos] << 24) | (f[pos + 1] << 16) | (f[pos + 2] << 8) | f[pos + 3];
        if (sz < 8 || pos + sz > f.size()) break;
        char type[5] = {(char)f[pos + 4], (char)f[pos + 5], (char)f[pos + 6], (char)f[pos + 7], 0};
        if (!strcmp(type, "moof")) { inInit = false; segs.emplace_back(); }
        if (inInit) init.insert(init.end(), f.begin() + pos, f.begin() + pos + sz);
        else if (!segs.empty() && strcmp(type, "sidx") && strcmp(type, "styp")) segs.back().insert(segs.back().end(), f.begin() + pos, f.begin() + pos + sz);
        pos += sz;
    }
}

struct MemIn { const uint8_t* d; size_t n, pos = 0; };
inline int memRead(void* o, uint8_t* buf, int size) {
    MemIn* m = (MemIn*)o;
    if (m->pos >= m->n) return AVERROR_EOF;
    int k = (int)std::min<size_t>(size, m->n - m->pos);
    memcpy(buf, m->d + m->pos, k);
    m->pos += k;
    return k;
}

inline const uint32_t kSrc = 0x0A010105u, kDst = 0xEFFF0501u;
inline std::vector<std::vector<uint8_t>> ipStream;   // IP packets in transmission order
inline int ipId = 1;

inline void sendUdp(uint32_t dst, int port, const std::vector<uint8_t>& payload) {
    for (auto& p : makeUdpPackets(kSrc, dst, 4000, port, payload, ipId++ & 0xFFFF)) ipStream.push_back(p);
}

inline void sendObject(uint32_t tsi, uint32_t toi, int cp, const std::vector<uint8_t>& data, int piece) {
    for (size_t off = 0; off < data.size(); off += piece) {
        LctPacket p;
        p.tsi = tsi; p.toi = toi; p.codePoint = cp; p.startOffset = (uint32_t)off;
        size_t n = std::min<size_t>(piece, data.size() - off);
        p.payload.assign(data.begin() + off, data.begin() + off + n);
        p.transferLength = (int64_t)data.size();
        sendUdp(kDst, 3000, makeRoutePacket(p, true));
    }
}


// The IP packet stream of a service: the SLT, the SLS package, the init segments and the media segments of both components.
struct Stream {
    std::vector<std::vector<uint8_t>> ip;
    std::vector<uint8_t> alp;
    std::vector<size_t> alpStarts;
    std::vector<std::vector<uint8_t>> bb;   // baseband packets of `bytes` bytes
    size_t vframes = 0, aframes = 0;
};

inline Stream buildStream(const char* vp, const char* ap, int bytes) {
    Stream out;
    auto vf = slurp(vp), af = slurp(ap);
    std::vector<uint8_t> vinit, ainit;
    std::vector<std::vector<uint8_t>> vseg, aseg;
    split(vf, vinit, vseg);
    split(af, ainit, aseg);
    ipStream.clear();
    ipId = 1;
    const std::string slt = "<SLT bsid=\"4660\"><Service serviceId=\"1001\" majorChannelNo=\"7\" minorChannelNo=\"1\" serviceCategory=\"1\" shortServiceName=\"TEST\" sltSvcSeqNum=\"0\">"
                            "<BroadcastSvcSignaling slsProtocol=\"1\" slsDestinationIpAddress=\"239.255.5.1\" slsDestinationUdpPort=\"3000\" slsSourceIpAddress=\"10.1.1.5\"/></Service></SLT>";
    sendUdp(kLlsAddress, kLlsPort, makeLls(1, 0, 1, slt));
    const std::string stsid =
        "<S-TSID><RS dIpAddr=\"239.255.5.1\" dPort=\"3000\">"
        "<LS tsi=\"10\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"v1\" contentType=\"video\"/></ContentInfo></SrcFlow></LS>"
        "<LS tsi=\"20\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"a1\" contentType=\"audio\" lang=\"en\"/></ContentInfo></SrcFlow></LS></RS></S-TSID>";
    std::vector<MimePart> parts(3);
    parts[0].headers["content-type"] = "application/route-usd+xml";
    parts[0].body = std::vector<uint8_t>({'<', 'B', 'u', 'n', 'd', 'l', 'e', 'D', 'e', 's', 'c', 'r', 'i', 'p', 't', 'i', 'o', 'n', '/', '>'});
    parts[1].headers["content-type"] = "application/route-s-tsid+xml";
    parts[1].body.assign(stsid.begin(), stsid.end());
    const std::string mpd = "<MPD type=\"dynamic\"><Period/></MPD>";
    parts[2].headers["content-type"] = "application/dash+xml";
    parts[2].body.assign(mpd.begin(), mpd.end());
    sendObject(0, 1, 3, makeMultipart(parts, "b1"), 1100);
    sendObject(10, 0, 5, vinit, 1100);
    sendObject(20, 0, 5, ainit, 1100);
    size_t n = std::max(vseg.size(), aseg.size());
    for (size_t i = 0; i < n; i++) {
        if (i < vseg.size()) sendObject(10, (uint32_t)i + 1, 8, vseg[i], 1100);
        if (i < aseg.size()) sendObject(20, (uint32_t)i + 1, 8, aseg[i], 1100);
        if (i == 0) sendUdp(kLlsAddress, kLlsPort, makeLls(1, 0, 1, slt));
    }
    out.ip = ipStream;
    for (auto& p : ipStream) {
        AlpPacket a; a.type = AlpIpv4; a.data = p;
        auto w = alpSingle(a);
        out.alpStarts.push_back(out.alp.size());
        out.alp.insert(out.alp.end(), w.begin(), w.end());
    }
    size_t pos = 0, next = 0;
    while (pos < out.alp.size()) {
        const int room = bytes - 2;
        while (next < out.alpStarts.size() && out.alpStarts[next] < pos) next++;
        int pointer = (next < out.alpStarts.size() && out.alpStarts[next] < pos + room) ? (int)(out.alpStarts[next] - pos) : 8191;
        size_t take = std::min<size_t>(room, out.alp.size() - pos);
        std::vector<uint8_t> chunk(out.alp.begin() + pos, out.alp.begin() + pos + take), pk;
        if ((int)take == room) {
            BbHeader h; h.pointer = pointer; h.twoByteBase = true;
            pk = makeBbHeader(h);
            pk.insert(pk.end(), chunk.begin(), chunk.end());
        } else pk = makeBbPacket(bytes, chunk, pointer == 8191 ? -1 : pointer, -1);
        out.bb.push_back(pk);
        pos += take;
    }
    out.vframes = 75; out.aframes = 142;
    return out;
}

// counts the video and audio frames of a transport stream; returns false if it cannot be read
inline bool countTs(const std::vector<uint8_t>& ts, long& video, long& audio, bool& hevc, bool& aac) {
    MemIn mi{ts.data(), ts.size()};
    unsigned char* iobuf = (unsigned char*)av_malloc(1 << 16);
    AVIOContext* io = avio_alloc_context(iobuf, 1 << 16, 0, &mi, memRead, nullptr, nullptr);
    AVFormatContext* ic = avformat_alloc_context();
    ic->pb = io;
    bool opened = avformat_open_input(&ic, "", av_find_input_format("mpegts"), nullptr) >= 0 && avformat_find_stream_info(ic, nullptr) >= 0;
    video = audio = 0; hevc = aac = false;
    if (opened) {
        int vid = -1, aud = -1;
        for (unsigned i = 0; i < ic->nb_streams; i++) {
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vid = (int)i;
            if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) aud = (int)i;
        }
        hevc = vid >= 0 && ic->streams[vid]->codecpar->codec_id == AV_CODEC_ID_HEVC;
        aac = aud >= 0 && ic->streams[aud]->codecpar->codec_id == AV_CODEC_ID_AAC;
        AVPacket* pk = av_packet_alloc();
        while (av_read_frame(ic, pk) >= 0) { video += pk->stream_index == vid; audio += pk->stream_index == aud; av_packet_unref(pk); }
        av_packet_free(&pk);
        avformat_close_input(&ic);
    }
    av_freep(&io->buffer);
    avio_context_free(&io);
    return opened;
}

} // namespace sim
