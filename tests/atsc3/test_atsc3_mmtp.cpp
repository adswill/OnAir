// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MMTP: packet and MPU payload headers, the MP table, and whole services built here from the fragmented test files: each media segment
// becomes an MPU the way ATSC 3.0 senders make them (ftyp + mmpu + moov with an MMT hint track in front of the media track, a moof with a
// hint traf, MFUs that carry a 34-byte hint sample in front of the media and a 14-byte header on every fragment), split into MMTP packets.
// Checked: the rebuilt fragmented MP4 (media samples byte for byte, one init segment, a running timeline that FFmpeg reads), shuffled,
// duplicated and lost packets (MPUs repaired down to the samples that can be played, which FFmpeg decodes without an error), a corrupted MPU sequence number, the limits, the alignment of two assets, and the whole receiver from an
// MMTP service in the service list to a transport stream. Needs the test files: test_atsc3_mmtp <video.mp4> <audio.mp4>
#include "atsc3_sim.h"
#include "dect2/atsc3/atsc3_mmtp.h"
#include "dect2/common/endian.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;
using dect2::endian::loadBe32;
using dect2::endian::putBe16;
using dect2::endian::putBe32;
using dect2::endian::putBe64;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

typedef std::vector<uint8_t> Bytes;
static const uint32_t kIp = 0xEFFF0501u;
static const int kPort = 3000;
static const uint32_t kHint = 34;

static Bytes box(const char* t, const Bytes& b) { Bytes v; putBe32(v, (uint32_t)b.size() + 8); for (int i = 0; i < 4; i++) v.push_back((uint8_t)t[i]); v.insert(v.end(), b.begin(), b.end()); return v; }
static void add(Bytes& v, const Bytes& b) { v.insert(v.end(), b.begin(), b.end()); }
struct B { size_t pos, size; std::string type; };
static std::vector<B> boxes(const Bytes& d, size_t from, size_t to) {
    std::vector<B> v;
    for (size_t p = from; p + 8 <= to;) {
        const size_t sz = loadBe32(&d[p]);
        if (sz < 8 || p + sz > to) break;
        v.push_back({p, sz, std::string((const char*)&d[p + 4], 4)});
        p += sz;
    }
    return v;
}
static Bytes sub(const Bytes& d, const B& b) { return Bytes(d.begin() + b.pos, d.begin() + b.pos + b.size); }
static Bytes inner(const Bytes& d, const B& b) { return Bytes(d.begin() + b.pos + 8, d.begin() + b.pos + b.size); }

// ---- an MPU as a sender makes it
struct Mpu { Bytes meta, moof; std::vector<Bytes> samples; };

static Bytes hintTrak(uint32_t id, uint32_t timescale) {
    Bytes tkhd(84, 0); dect2::endian::storeBe32(&tkhd[12], id);
    Bytes mdhd(24, 0); dect2::endian::storeBe32(&mdhd[12], timescale);
    Bytes hdlr(25, 0); memcpy(&hdlr[8], "hint", 4);
    return box("trak", [&] { Bytes t = box("tkhd", tkhd); add(t, box("mdia", [&] { Bytes m = box("mdhd", mdhd); add(m, box("hdlr", hdlr)); return m; }())); return t; }());
}

// `init` and `seg` from the test file; `moovPatch` changes a byte of the mvhd (a new encoder configuration)
static Mpu makeMpu(const Bytes& init, const Bytes& seg, uint32_t seq, const std::string& assetId, bool moovPatch = false) {
    Mpu m;
    MpuInit in;
    readMpuMetadata(init.data(), init.size(), in);
    for (const B& b : boxes(init, 0, init.size())) {
        if (b.type == "ftyp") {
            add(m.meta, sub(init, b));
            Bytes mm; putBe32(mm, 0); mm.push_back(0x80); putBe32(mm, seq); putBe32(mm, 1); putBe32(mm, (uint32_t)assetId.size()); mm.insert(mm.end(), assetId.begin(), assetId.end());
            add(m.meta, box("mmpu", mm));
        }
        if (b.type != "moov") continue;
        Bytes mv;
        bool hintDone = false;
        for (const B& c : boxes(init, b.pos + 8, b.pos + b.size)) {
            if (c.type == "trak" && !hintDone) { add(mv, hintTrak(in.trackId + 1, in.timescale)); hintDone = true; }   // the hint track first
            if (c.type == "mvex") {
                Bytes x = inner(init, c);
                Bytes trex(24, 0); dect2::endian::storeBe32(&trex[4], in.trackId + 1); dect2::endian::storeBe32(&trex[8], 1); dect2::endian::storeBe32(&trex[16], kHint);
                add(x, box("trex", trex));
                add(mv, box("mvex", x));
            } else if (c.type == "mvhd" && moovPatch) { Bytes h = sub(init, c); h[30] ^= 1; add(mv, h); }
            else add(mv, sub(init, c));
        }
        add(m.meta, box("moov", mv));
    }
    MpuFragment f;
    CHECK(readMpuFragment(seg.data(), seg.size(), in, f), "read the test segment");
    const auto top = boxes(seg, 0, seg.size());
    Bytes moofIn, mdat;
    for (const B& b : top) { if (b.type == "moof") moofIn = inner(seg, b); if (b.type == "mdat") mdat = inner(seg, b); }
    size_t pos = 0;
    for (uint32_t s : f.sizes) { m.samples.emplace_back(mdat.begin() + pos, mdat.begin() + pos + s); pos += s; }
    Bytes mf;
    for (const B& c : boxes(moofIn, 0, moofIn.size())) {
        if (c.type == "mfhd") { Bytes h; putBe32(h, 0); putBe32(h, 1); add(mf, box("mfhd", h)); }   // every MPU says fragment 1
        else add(mf, sub(moofIn, c));
    }
    Bytes tfhd; putBe32(tfhd, 0x020018); putBe32(tfhd, in.trackId + 1); putBe32(tfhd, 0); putBe32(tfhd, kHint);
    Bytes trun; putBe32(trun, 0x000001); putBe32(trun, (uint32_t)f.sizes.size()); putBe32(trun, 0);
    Bytes hintTraf = box("tfhd", tfhd); add(hintTraf, box("trun", trun));
    add(mf, box("traf", hintTraf));
    m.moof = box("moof", mf);
    putBe32(m.moof, (uint32_t)(8 + f.mediaBytes + kHint * f.sizes.size()));
    m.moof.insert(m.moof.end(), {'m', 'd', 'a', 't'});
    return m;
}

struct Sender {
    explicit Sender(uint16_t p) : pid(p) {}
    uint16_t pid;
    uint32_t psn = 0;
    int metaPieces = 1, moofPieces = 1;
    size_t mfuPiece = 400;
    std::vector<UdpDatagram> out;
    void packet(int type, const Bytes& payload) {
        MmtpPacket p; p.version = 1; p.type = type; p.packetId = pid; p.sequence = psn++;
        UdpDatagram d; d.dstIp = kIp; d.dstPort = kPort; d.payload = makeMmtpPacket(p, payload);
        out.push_back(d);
    }
    static Bytes payload(int ft, int fi, int counter, uint32_t seq, const Bytes& du, bool timed = true) {
        Bytes v; putBe16(v, (uint32_t)(du.size() + 6)); v.push_back((uint8_t)(ft << 4 | (timed ? 8 : 0) | fi << 1)); v.push_back((uint8_t)counter); putBe32(v, seq); add(v, du);
        return v;
    }
    void pieces(int ft, uint32_t seq, const Bytes& unit, int n) {
        const size_t step = (unit.size() + n - 1) / n;
        for (int k = 0; k < n; k++) {
            Bytes part(unit.begin() + std::min(unit.size(), k * step), unit.begin() + std::min(unit.size(), (k + 1) * step));
            packet(MmtpMpu, payload(ft, n == 1 ? 0 : k == 0 ? 1 : k == n - 1 ? 3 : 2, n - 1 - k, seq, part));
        }
    }
    // `lose` (sample index, fragment index) leaves MFU fragments out; `noMoof` the movie fragment
    void mpu(const Mpu& m, uint32_t seq, bool moofLast = false, const std::function<bool(size_t, size_t)>& lose = nullptr, bool noMoof = false) {
        pieces(MpuMetadata, seq, m.meta, metaPieces);
        if (!moofLast && !noMoof) pieces(MpuMovieFragment, seq, m.moof, moofPieces);
        for (size_t s = 0; s < m.samples.size(); s++) {
            Bytes all(kHint, (uint8_t)(0xA0 + s));   // the hint sample, then the media
            add(all, m.samples[s]);
            std::vector<std::pair<uint32_t, Bytes>> frags;
            for (size_t o = 0; o < all.size(); o += mfuPiece) frags.push_back({(uint32_t)(o == 0 ? 0 : o - kHint), Bytes(all.begin() + o, all.begin() + std::min(all.size(), o + mfuPiece))});
            for (size_t k = 0; k < frags.size(); k++) {
                if (lose && lose(s, k)) { psn++; continue; }
                Bytes du; putBe32(du, 1); putBe32(du, (uint32_t)s + 1); putBe32(du, frags[k].first); du.push_back(0); du.push_back(0);
                add(du, frags[k].second);
                const int n = (int)frags.size();
                packet(MmtpMpu, payload(MpuMfu, n == 1 ? 0 : k == 0 ? 1 : k + 1 == frags.size() ? 3 : 2, n - 1 - (int)k, seq, du));
            }
        }
        if (moofLast && !noMoof) pieces(MpuMovieFragment, seq, m.moof, moofPieces);
    }
};

struct Asset { uint16_t pid; std::string type; std::vector<std::pair<uint32_t, uint64_t>> times; };
static Bytes mpTable(int tableId, const std::vector<Asset>& as) {
    Bytes t;
    t.push_back((uint8_t)tableId); t.push_back(3); putBe16(t, 0); t.push_back(0xFE);
    if (tableId == 0x20 || tableId == 0x11) { const std::string pkg = "PKG-1"; t.push_back((uint8_t)pkg.size()); t.insert(t.end(), pkg.begin(), pkg.end()); putBe16(t, 0); }
    t.push_back((uint8_t)as.size());
    for (auto& a : as) {
        const std::string id = "asset-" + std::to_string(a.pid);
        t.push_back(0); putBe32(t, 1); putBe32(t, (uint32_t)id.size()); t.insert(t.end(), id.begin(), id.end());
        t.insert(t.end(), a.type.begin(), a.type.end());
        t.push_back(0xFE); t.push_back(1); t.push_back(0); putBe16(t, a.pid);
        Bytes desc;
        if (!a.times.empty()) { putBe16(desc, 1); desc.push_back((uint8_t)(12 * a.times.size())); for (auto& x : a.times) { putBe32(desc, x.first); putBe64(desc, x.second); } }
        putBe16(t, (uint32_t)desc.size()); add(t, desc);
    }
    dect2::endian::storeBe16(&t[2], (uint32_t)t.size() - 4);
    return t;
}
static UdpDatagram signalling(int tableId, const std::vector<Asset>& as, uint32_t psn, bool junk = false) {
    Bytes tbl = mpTable(tableId, as);
    if (junk) { tbl.push_back(0x55); tbl.push_back(0xAA); }
    Bytes msg; putBe16(msg, (uint32_t)tableId); msg.push_back(3); putBe16(msg, (uint32_t)tbl.size()); add(msg, tbl);
    Bytes pl{0, 0}; add(pl, msg);
    MmtpPacket p; p.version = 1; p.type = MmtpSignalling; p.packetId = 0; p.sequence = psn;
    UdpDatagram d; d.dstIp = kIp; d.dstPort = kPort; d.payload = makeMmtpPacket(p, pl);
    return d;
}

struct Out { uint16_t pid; bool init; Bytes data; };
static std::vector<Out> run(const std::vector<UdpDatagram>& in, MmtpStats* st = nullptr, bool bySeq = false) {
    MmtpService s(0, kIp, kPort);
    s.setAlignBySequence(bySeq);
    std::vector<Out> out;
    s.onObject = [&](uint16_t pid, const std::string&, bool init, const Bytes& d) { out.push_back({pid, init, d}); };
    for (auto& d : in) s.push(d);
    if (st) *st = s.stats();
    return out;
}

// the media bytes of a rebuilt segment, its decode time, and whether its moof has one traf whose data offset points into the mdat
static bool readSegment(const Bytes& seg, Bytes& media, uint64_t& tfdt, uint32_t& mfhd) {
    const auto top = boxes(seg, 0, seg.size());
    if (top.size() != 2 || top[0].type != "moof" || top[1].type != "mdat") return false;
    media = inner(seg, top[1]);
    int trafs = 0;
    bool ok = false;
    for (const B& c : boxes(seg, 8, top[0].size)) {
        if (c.type == "mfhd") mfhd = loadBe32(&seg[c.pos + 12]);
        if (c.type != "traf") continue;
        trafs++;
        for (const B& e : boxes(seg, c.pos + 8, c.pos + c.size)) {
            if (e.type == "tfdt") tfdt = dect2::endian::loadBe64(&seg[e.pos + 12]);
            if (e.type == "trun") ok = (loadBe32(&seg[e.pos + 8]) & 1) && loadBe32(&seg[e.pos + 16]) == top[0].size + 8;
        }
    }
    return trafs == 1 && ok;
}

// reads init + segments with FFmpeg's MP4 demuxer: packets and whether the decode times only go up
static long ffmpegPackets(const Bytes& file, bool& rising, double& firstSec, double& lastSec) {
    sim::MemIn mi{file.data(), file.size()};
    unsigned char* iobuf = (unsigned char*)av_malloc(1 << 16);
    AVIOContext* io = avio_alloc_context(iobuf, 1 << 16, 0, &mi, sim::memRead, nullptr, nullptr);
    AVFormatContext* ic = avformat_alloc_context();
    ic->pb = io;
    long n = -1;
    rising = true;
    if (avformat_open_input(&ic, "", av_find_input_format("mov"), nullptr) >= 0) {
        n = 0;
        int64_t last = INT64_MIN;
        AVPacket* pk = av_packet_alloc();
        while (av_read_frame(ic, pk) >= 0) {
            const AVRational tb = ic->streams[pk->stream_index]->time_base;
            if (n == 0) firstSec = pk->dts * av_q2d(tb);
            lastSec = pk->dts * av_q2d(tb);
            if (pk->dts <= last) rising = false;
            last = pk->dts;
            n++;
            av_packet_unref(pk);
        }
        av_packet_free(&pk);
        avformat_close_input(&ic);
    }
    av_freep(&io->buffer);
    avio_context_free(&io);
    return n;
}

// the number of samples of a rebuilt segment and the sum of their durations (explicit in the trun, or the tfhd's default)
static bool trunTimes(const Bytes& seg, uint32_t& count, uint64_t& dur) {
    const auto top = boxes(seg, 0, seg.size());
    if (top.empty() || top[0].type != "moof") return false;
    for (const B& c : boxes(seg, 8, top[0].size)) {
        if (c.type != "traf") continue;
        uint32_t defDur = 0;
        for (const B& e : boxes(seg, c.pos + 8, c.pos + c.size)) {
            const uint32_t fl = loadBe32(&seg[e.pos + 8]) & 0xFFFFFF;
            if (e.type == "tfhd") {
                size_t f = e.pos + 16 + (fl & 1 ? 8 : 0) + (fl & 2 ? 4 : 0);
                if (fl & 8) defDur = loadBe32(&seg[f]);
            }
            if (e.type != "trun") continue;
            count = loadBe32(&seg[e.pos + 12]);
            size_t f = e.pos + 16 + (fl & 1 ? 4 : 0) + (fl & 4 ? 4 : 0);
            const size_t per = (fl & 0x100 ? 4 : 0) + (fl & 0x200 ? 4 : 0) + (fl & 0x400 ? 4 : 0) + (fl & 0x800 ? 4 : 0);
            dur = 0;
            for (uint32_t i = 0; i < count; i++) dur += (fl & 0x100) ? loadBe32(&seg[f + i * per]) : defDur;
            return true;
        }
    }
    return false;
}

// decodes the video of init + segments with FFmpeg: pictures out, and errors (failed calls and messages at error level)
static long gDecodeErrors = 0;
static void countErrors(void*, int level, const char*, va_list) { if (level <= AV_LOG_ERROR) gDecodeErrors++; }
static void decodeVideo(const Bytes& file, long& frames, long& errors) {
    frames = 0;
    gDecodeErrors = 0;
    av_log_set_callback(countErrors);
    sim::MemIn mi{file.data(), file.size()};
    unsigned char* iobuf = (unsigned char*)av_malloc(1 << 16);
    AVIOContext* io = avio_alloc_context(iobuf, 1 << 16, 0, &mi, sim::memRead, nullptr, nullptr);
    AVFormatContext* ic = avformat_alloc_context();
    ic->pb = io;
    if (avformat_open_input(&ic, "", av_find_input_format("mov"), nullptr) >= 0 && avformat_find_stream_info(ic, nullptr) >= 0 && ic->nb_streams > 0) {
        const AVCodec* codec = avcodec_find_decoder(ic->streams[0]->codecpar->codec_id);
        AVCodecContext* c = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (c && avcodec_parameters_to_context(c, ic->streams[0]->codecpar) >= 0 && avcodec_open2(c, codec, nullptr) >= 0) {
            AVPacket* pk = av_packet_alloc();
            AVFrame* fr = av_frame_alloc();
            auto drain = [&] { while (avcodec_receive_frame(c, fr) >= 0) frames++; };
            while (av_read_frame(ic, pk) >= 0) {
                if (avcodec_send_packet(c, pk) < 0) gDecodeErrors++;
                av_packet_unref(pk);
                drain();
            }
            avcodec_send_packet(c, nullptr);
            drain();
            av_frame_free(&fr);
            av_packet_free(&pk);
        } else gDecodeErrors++;
        avcodec_free_context(&c);
        avformat_close_input(&ic);
    } else gDecodeErrors++;
    av_freep(&io->buffer);
    avio_context_free(&io);
    av_log_set_callback(av_log_default_callback);
    errors = gDecodeErrors;
}

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    Bytes vf = sim::slurp(vp), af = sim::slurp(ap);
    Bytes vinit, ainit;
    std::vector<Bytes> vseg, aseg;
    sim::split(vf, vinit, vseg);
    sim::split(af, ainit, aseg);
    if (vseg.size() < 3 || aseg.empty()) { printf("test files not found\n"); return 1; }
    std::mt19937 rng(23);

    // ---- 1. packet header: version 0 and 1, packet counter, header extension, lengths that do not fit
    {
        for (int ver = 0; ver < 2; ver++)
            for (int c = 0; c < 2; c++)
                for (int x = 0; x < 2; x++) {
                    MmtpPacket p; p.version = ver; p.counterFlag = c; p.extensionFlag = x; p.rap = true; p.type = 2; p.packetId = 0x1234; p.timestamp = 0xCAFEBABE; p.sequence = 77; p.counter = 9;
                    const Bytes pl{1, 2, 3}, ext{9, 9, 9, 9, 9};
                    Bytes w = makeMmtpPacket(p, pl, ext);
                    MmtpPacket q;
                    const bool ok = parseMmtpPacket(w.data(), w.size(), q);
                    CHECK(ok && q.version == ver && q.counterFlag == (bool)c && q.extensionFlag == (bool)x && q.rap && q.type == 2 && q.packetId == 0x1234 && q.timestamp == 0xCAFEBABE &&
                          q.sequence == 77 && (!c || q.counter == 9) && q.payloadSize == 3 && q.payload[0] == 1 && q.payload[2] == 3, "packet header round trip");
                    if (x) { w.resize(w.size() - pl.size() - 2); CHECK(!parseMmtpPacket(w.data(), w.size(), q), "an extension longer than the packet is refused"); }
                }
        Bytes shortPk(11, 0x40);
        MmtpPacket q;
        CHECK(!parseMmtpPacket(shortPk.data(), shortPk.size(), q), "a packet shorter than the header is refused");
    }
    // ---- 2. MPU payload: aggregation, a length that does not fit, non-timed data
    {
        Bytes du1{1, 2, 3}, du2{4, 5};
        Bytes v; putBe16(v, 6 + 2 + 3 + 2 + 2); v.push_back((uint8_t)(MpuMfu << 4 | 8 | 1)); v.push_back(0); putBe32(v, 42);
        putBe16(v, 3); add(v, du1); putBe16(v, 2); add(v, du2);
        MpuPayload m;
        CHECK(parseMpuPayload(v.data(), v.size(), m) && m.aggregated && m.units.size() == 2 && m.units[1].second == 2 && m.units[1].first[1] == 5 && m.mpuSequence == 42, "aggregated data units");
        v[1] += 1;
        CHECK(!parseMpuPayload(v.data(), v.size(), m), "an MPU payload longer than its packet is refused");
        Sender s{100};
        s.packet(MmtpMpu, Sender::payload(MpuMfu, 0, 0, 5, Bytes(20, 1), false));
        MmtpStats st;
        CHECK(run(s.out, &st).empty() && st.nonTimed == 1, "non-timed MFUs are counted and left out");
    }
    // ---- 11. MP table: asset ids with a 32-bit scheme and length, the complete table, a subset with MPU timestamps, bytes after the table
    {
        Bytes t = mpTable(0x20, {{100, "hvc1", {}}, {200, "mp4a", {{7, 0x0102030405060708ull}}}});
        t.push_back(0xEE);
        MpTable mt;
        CHECK(parseMpTable(t.data(), t.size(), mt) && mt.tableId == 0x20 && mt.packageId == "PKG-1" && mt.assets.size() == 2 && mt.assets[0].type == "hvc1" &&
              mt.assets[0].packetIds == std::vector<uint16_t>{100} && mt.assets[1].id == "asset-200" && mt.assets[1].mpuTimes.size() == 1 && mt.assets[1].mpuTimes[0].second == 0x0102030405060708ull, "complete MP table");
        Bytes s = mpTable(0x13, {{200, "mp4a", {{8, 99}}}});
        CHECK(parseMpTable(s.data(), s.size(), mt) && mt.tableId == 0x13 && mt.packageId.empty() && mt.assets.size() == 1 && mt.assets[0].mpuTimes[0].first == 8, "subset MP table without the package id");
        t.resize(t.size() - 6);
        CHECK(!parseMpTable(t.data(), t.size(), mt), "a cut MP table is refused");
        // a location of type 4 (MPEG-2 transport stream over IPv6, 36 bytes) in front of the packet id
        Bytes l4; l4.push_back(0x20); l4.push_back(1); putBe16(l4, 0); l4.push_back(0xFE); l4.push_back(0); putBe16(l4, 0); l4.push_back(1);
        l4.push_back(0); putBe32(l4, 0); putBe32(l4, 1); l4.push_back('a'); add(l4, Bytes{'h', 'v', 'c', '1'}); l4.push_back(0xFE);
        l4.push_back(2); l4.push_back(4); add(l4, Bytes(34, 0x11)); putBe16(l4, 0x0100); l4.push_back(0); putBe16(l4, 77); putBe16(l4, 0);
        dect2::endian::storeBe16(&l4[2], (uint32_t)l4.size() - 4);
        CHECK(parseMpTable(l4.data(), l4.size(), mt) && mt.assets.size() == 1 && mt.assets[0].packetIds == std::vector<uint16_t>{77}, "a location of type 4 is stepped over");
    }
    // ---- 3, 12. one asset in order: one init segment of the media track alone, the media samples byte for byte, a running timeline
    const int kMpus = 9;
    std::vector<Mpu> vm;
    for (int k = 0; k < kMpus; k++) vm.push_back(makeMpu(vinit, vseg[k % 3], 1000 + k, "video-1"));
    Sender sv{100};
    for (int k = 0; k < kMpus; k++) sv.mpu(vm[k], 1000 + k);
    std::vector<Out> ref = run(sv.out);
    MpuInit vin;
    readMpuMetadata(vinit.data(), vinit.size(), vin);
    // how many pictures of a video MPU are played when everything from its sample `cut` on (in decode order) is gone: those before it, less
    // the last ones that would be presented after a picture that is gone
    auto playedBefore = [&](const Mpu& m, size_t cut) {
        MpuFragment f;
        if (!readMpuFragment(m.moof.data(), m.moof.size(), vin, f)) return (size_t)0;
        std::vector<int64_t> pt;
        int64_t t = 0;
        for (size_t i = 0; i < f.sizes.size(); i++) { pt.push_back(t + f.compositionOffsets[i]); t += f.durations[i]; }
        int64_t first = INT64_MAX;
        for (size_t i = cut; i < pt.size(); i++) first = std::min(first, pt[i]);
        size_t n = cut;
        while (n > 1 && *std::max_element(pt.begin(), pt.begin() + n) >= first) { n--; first = std::min(first, pt[n]); }
        return n;
    };
    {
        CHECK(ref.size() == kMpus + 1 && ref[0].init, "one init segment, then one media segment per MPU");
        const Bytes& init = ref[0].data;
        MpuInit in;
        CHECK(readMpuMetadata(init.data(), init.size(), in) && in.handler == "vide" && in.trackId == vin.trackId && in.hintTrackId == 0, "the init segment holds the video track only");
        bool mmpu = false;
        for (const B& b : boxes(init, 0, init.size())) mmpu |= b.type == "mmpu";
        CHECK(!mmpu, "the mmpu box is not in the init segment");
        Bytes file = init;
        uint64_t expect = 0;
        bool same = true, layout = true, times = true;
        for (int k = 0; k + 1 < (int)ref.size(); k++) {
            Bytes media;
            uint64_t tfdt = 0;
            uint32_t mfhd = 0;
            layout &= readSegment(ref[k + 1].data, media, tfdt, mfhd) && mfhd == (uint32_t)k + 1;
            Bytes want;
            for (auto& s : vm[k].samples) add(want, s);
            same &= media == want;
            if (k > 0) times &= tfdt == expect;
            expect = tfdt + 25 * 512;
            add(file, ref[k + 1].data);
        }
        CHECK(same, "media samples byte for byte");
        CHECK(layout, "one traf per moof, the data offset at the mdat, fragment numbers counting up");
        CHECK(times, "each MPU starts where the one before ended");
        bool rising = false;
        double t0 = 0, t1 = 0;
        const long n = ffmpegPackets(file, rising, t0, t1);
        printf("  FFmpeg reads %ld video packets from %.2f to %.2f s\n", n, t0, t1);
        CHECK(n == kMpus * 25 && rising && t1 - t0 > kMpus - 1.5, "FFmpeg plays the rebuilt fragments as one stream");
    }
    // ---- 4, 5. the same from shuffled and duplicated packets, and with the metadata and movie fragment in fragments
    {
        Sender s{100};
        s.metaPieces = 3; s.moofPieces = 2; s.mfuPiece = 333;
        for (int k = 0; k < kMpus; k++) {
            Sender one{100};
            one.psn = s.psn; one.metaPieces = 3; one.moofPieces = 2; one.mfuPiece = 333;
            one.mpu(vm[k], 1000 + k, k % 2 == 1);
            std::vector<UdpDatagram> pk = one.out;
            for (size_t i = 0; i < pk.size(); i += 5) pk.push_back(pk[i]);
            std::shuffle(pk.begin(), pk.end(), rng);
            s.out.insert(s.out.end(), pk.begin(), pk.end());
            s.psn = one.psn;
        }
        std::vector<Out> o = run(s.out);
        bool same = o.size() == ref.size();
        for (size_t i = 0; same && i < o.size(); i++) same = o[i].data == ref[i].data && o[i].init == ref[i].init;
        CHECK(same, "shuffled, duplicated and fragmented packets give the same output");
    }
    // ---- 6. the init segment once, though each MPU's metadata differs (mmpu); again when the moov really changes
    {
        Sender s{100};
        for (int k = 0; k < 6; k++) s.mpu(makeMpu(vinit, vseg[k % 3], 2000 + k, "video-1", k >= 3), 2000 + k);
        std::vector<Out> o = run(s.out);
        int inits = 0;
        for (auto& x : o) inits += x.init;
        CHECK(o.size() == 8 && inits == 2 && o[0].init && o[4].init, "init segment sent once, and again after a moov change");
    }
    // ---- 7. lost packets. An MPU missing an MFU fragment goes out with the samples that can be played, in place on the timeline: in decode
    // order up to the first lost sample other pictures may need (a lost picture that no other one uses costs itself only); when MPUs after
    // it are lost too, less the last ones before it that would be presented after a picture that is gone; sound loses the lost samples alone, and a lost sound moof is rebuilt
    // from the MPU before. A video MPU without its moof, or without the first picture, is left out and time goes on across the gap. The
    // picture decodes without an error.
    {
        // which test pictures no other picture uses, read here from the NAL unit headers (one temporal sub-layer in the test file)
        auto disposable = [](const Bytes& smp) {
            for (size_t q = 0; q + 6 <= smp.size();) {
                const uint32_t n = loadBe32(&smp[q]);
                const int type = (smp[q + 4] >> 1) & 0x3F;
                if (type < 32) return type <= 14 && type % 2 == 0 && (smp[q + 5] & 7) == 1;
                q += 4 + n;
            }
            return false;
        };
        const Mpu& m = vm[3];   // MPU 1003 (vseg[0])
        const size_t n = m.samples.size();
        int ref = -1, disp = -1;
        for (size_t i = 3; i < n; i++) {
            if (ref < 0 && !disposable(m.samples[i]) && m.samples[i].size() > 12) ref = (int)i;
            if (disp < 0 && disposable(m.samples[i]) && m.samples[i].size() > 12) disp = (int)i;
        }
        CHECK(ref > 1 && disp > 0, "the test video has pictures other pictures use and pictures none uses");
        Sender s{100};
        s.mfuPiece = kHint + 10;   // the first fragment of a sample holds its first NAL unit header, the pictures are small
        for (int k = 0; k < kMpus; k++) {
            std::function<bool(size_t, size_t)> lose;
            if (k == 3) lose = [&](size_t i, size_t f) { return (int)i == ref && f == 1; };   // a picture others use: cut there
            if (k == 4) lose = [&](size_t i, size_t f) { return (int)i == disp && f == 1; };  // a picture none uses: that picture only
            if (k == 5) lose = [&](size_t i, size_t f) { return i == 0 && f == 1; };          // the first picture: nothing to play
            s.mpu(vm[k], 1000 + k, false, lose, k == 6);                                       // MPU 6 without its moof
        }
        std::vector<Out> o = run(s.out);
        // what each MPU should give: sample indexes
        std::vector<std::vector<size_t>> want(kMpus);
        const size_t played3 = (size_t)ref;   // MPU 4 follows on time
        CHECK(playedBefore(vm[3], (size_t)ref) < (size_t)ref, "the test video has pictures before the cut presented after it");
        for (int k = 0; k < kMpus; k++)
            for (size_t i = 0; i < vm[k].samples.size(); i++) {
                if (k == 3 && i >= played3) continue;
                if (k == 4 && (int)i == disp) continue;
                if (k == 5 || k == 6) continue;
                want[k].push_back(i);
            }
        bool ok = o.size() == (size_t)kMpus + 1 - 2 && o[0].init;
        Bytes file = o.empty() ? Bytes() : o[0].data;
        size_t at = 1;
        long frames = 0;
        uint64_t t0 = 0;
        for (int k = 0; ok && k < kMpus; k++) {
            if (want[k].empty()) continue;
            Bytes media, expect;
            uint64_t tfdt = 0;
            uint32_t mfhd = 0, count = 0;
            uint64_t dur = 0;
            ok = at < o.size() && readSegment(o[at].data, media, tfdt, mfhd) && trunTimes(o[at].data, count, dur);
            if (!ok) break;
            if (k == 0) t0 = tfdt;
            for (size_t i : want[k]) add(expect, vm[k].samples[i]);
            ok = media == expect && count == want[k].size() && tfdt == t0 + (uint64_t)k * 25 * 512 && dur == 25 * 512;
            if (!ok) printf("  MPU %d: %u samples (want %zu), media %s, tfdt %llu (want %llu), duration %llu\n", k, count, want[k].size(), media == expect ? "same" : "differs",
                            (unsigned long long)tfdt, (unsigned long long)(t0 + (uint64_t)k * 25 * 512), (unsigned long long)dur);
            frames += (long)want[k].size();
            add(file, o[at].data);
            at++;
        }
        CHECK(ok, "an MPU with a lost fragment keeps the pictures that can be played, in place; one without its moof or first picture is left out");
        long decoded = 0, errors = 0;
        decodeVideo(file, decoded, errors);
        printf("  repaired video: %ld pictures decoded of %ld sent, %ld decoder errors\n", decoded, frames, errors);
        CHECK(decoded == frames && errors == 0, "the repaired video decodes without an error");
        // the same cut in MPU 3, now with MPUs 4 and 5 lost whole: the pictures before the cut presented after it are left out as well
        {
            Sender g{100};
            g.mfuPiece = kHint + 10;
            for (int k = 0; k < kMpus; k++) {
                if (k == 4 || k == 5) continue;
                std::function<bool(size_t, size_t)> lose;
                if (k == 3) lose = [&](size_t i, size_t f) { return (int)i == ref && f == 1; };
                g.mpu(vm[k], 1000 + k, false, lose);
            }
            MmtpStats gs;
            std::vector<Out> go = run(g.out, &gs);
            const size_t trimmed = playedBefore(vm[3], (size_t)ref);
            bool gok = go.size() == (size_t)kMpus + 1 - 2 && go[0].init;
            Bytes gfile = gok ? go[0].data : Bytes();
            uint64_t g0 = 0;
            long gframes = 0;
            for (int k = 0, at = 1; gok && k < kMpus; k++) {
                if (k == 4 || k == 5) continue;
                Bytes media, expect;
                uint64_t tfdt = 0, dur = 0;
                uint32_t mfhd = 0, count = 0;
                gok = readSegment(go[at].data, media, tfdt, mfhd) && trunTimes(go[at].data, count, dur);
                if (!gok) break;
                if (k == 0) g0 = tfdt;
                const size_t keep = k == 3 ? trimmed : vm[k].samples.size();
                for (size_t i = 0; i < keep; i++) add(expect, vm[k].samples[i]);
                gok = media == expect && count == keep && tfdt == g0 + (uint64_t)k * 25 * 512 && dur == 25 * 512;
                gframes += (long)keep;
                add(gfile, go[at].data);
                at++;
            }
            CHECK(gok && gs.samplesCut == (long)(vm[3].samples.size() - 1 - trimmed),
                  "before MPUs lost whole, the pictures kept are those presented before every picture cut");
            long gdec = 0, gerr = 0;
            decodeVideo(gfile, gdec, gerr);
            CHECK(gdec == gframes && gerr == 0, "and that video decodes without an error");
        }
        // sound: one sample lost in MPU 2, the moof of MPU 3 lost (rebuilt from MPU 2's), the first sample of MPU 4 lost
        Sender a{200};
        const int kA = 6;
        Mpu am0 = makeMpu(ainit, aseg[0], 1000, "audio-1");
        const size_t an = am0.samples.size();
        for (int k = 0; k < kA; k++) {
            std::function<bool(size_t, size_t)> lose;
            if (k == 2) lose = [](size_t i, size_t) { return i == 7; };
            if (k == 4) lose = [](size_t i, size_t) { return i == 0; };
            a.mpu(makeMpu(ainit, aseg[0], 1000 + k, "audio-1"), 1000 + k, false, lose, k == 3);
        }
        o = run(a.out);
        MpuInit ain;
        readMpuMetadata(ainit.data(), ainit.size(), ain);
        MpuFragment af;
        readMpuFragment(am0.moof.data(), am0.moof.size(), ain, af);
        uint64_t adur = 0;
        for (uint32_t d : af.durations) adur += d;
        ok = o.size() == (size_t)kA + 1;
        for (int k = 0; ok && k < kA; k++) {
            Bytes media, expect;
            uint64_t tfdt = 0, dur = 0;
            uint32_t mfhd = 0, count = 0;
            ok = readSegment(o[k + 1].data, media, tfdt, mfhd) && trunTimes(o[k + 1].data, count, dur);
            if (k == 0) t0 = tfdt;
            for (size_t i = 0; i < an; i++) if (!(k == 2 && i == 7) && !(k == 4 && i == 0)) add(expect, am0.samples[i]);
            const uint64_t lead = k == 4 ? af.durations[0] : 0;
            ok = ok && media == expect && count == an - (k == 2 || k == 4) && tfdt == t0 + (uint64_t)k * adur + lead && dur + lead == adur;
        }
        CHECK(ok, "sound loses the lost samples only, keeps its time, and a lost moof is rebuilt from the MPU before");
        // the first MPU of an asset is not repaired when the receiver came in part of the way through it
        Sender j{200};
        for (int k = 0; k < 3; k++) j.mpu(makeMpu(ainit, aseg[0], 1000 + k, "audio-1"), 1000 + k);
        std::vector<UdpDatagram> late(j.out.begin() + 2 + an / 2, j.out.end());
        o = run(late);
        ok = o.size() == 3;
        if (ok) {
            Bytes media; uint64_t tfdt = 0; uint32_t mfhd = 0;
            Bytes expect;
            for (auto& x : am0.samples) add(expect, x);
            ok = readSegment(o[1].data, media, tfdt, mfhd) && media == expect;
        }
        CHECK(ok, "the end of an MPU the receiver came in on is not played as a repaired MPU");
    }
    // ---- 8. one packet with a corrupted MPU sequence number does not stop the asset
    {
        Sender s{100};
        for (int k = 0; k < kMpus; k++) {
            s.mpu(vm[k], 1000 + k);
            if (k == 2) s.packet(MmtpMpu, Sender::payload(MpuMfu, 0, 0, (1000 + k) ^ 0x40000000u, Bytes(30, 0)));
        }
        MmtpStats st;
        std::vector<Out> o = run(s.out, &st);
        CHECK(o.size() == ref.size() && st.seqRejected == 1, "a corrupted sequence number is ignored");
    }
    // ---- 9. sample tables beyond the limits are refused, not clamped
    {
        MpuFragment f;
        Bytes trun; putBe32(trun, 0x000201); putBe32(trun, 5000); putBe32(trun, 0);
        for (int i = 0; i < 5000; i++) putBe32(trun, 10);
        Bytes tfhd; putBe32(tfhd, 0x020000); putBe32(tfhd, vin.trackId);
        Bytes traf = box("tfhd", tfhd); add(traf, box("trun", trun));
        Bytes moof = box("moof", box("traf", traf));
        CHECK(!readMpuFragment(moof.data(), moof.size(), vin, f), "a trun of 5000 samples is refused");
        Bytes trun2; putBe32(trun2, 0x000201); putBe32(trun2, 2); putBe32(trun2, 0); putBe32(trun2, 100); putBe32(trun2, 9u << 20);
        Bytes traf2 = box("tfhd", tfhd); add(traf2, box("trun", trun2));
        Bytes moof2 = box("moof", box("traf", traf2));
        CHECK(!readMpuFragment(moof2.data(), moof2.size(), vin, f), "a sample of 9 MB is refused");
        Bytes trun3; putBe32(trun3, 0x000201); putBe32(trun3, 1000); putBe32(trun3, 0); putBe32(trun3, 100);
        Bytes traf3 = box("tfhd", tfhd); add(traf3, box("trun", trun3));
        Bytes moof3 = box("moof", box("traf", traf3));
        CHECK(!readMpuFragment(moof3.data(), moof3.size(), vin, f), "a sample count larger than the box is refused");
    }
    // ---- 10. MPUs that never complete are given up, at most 8 stay open per asset
    {
        Sender s{100};
        for (int k = 0; k < 20; k++) s.pieces(MpuMetadata, 3000 + k, vm[0].meta, 1);
        MmtpStats st;
        run(s.out, &st);
        CHECK(st.mpusDropped >= 12 && st.mpusDelivered == 0, "unfinished MPUs are dropped");
    }
    // ---- 13. MPUs of two movie fragments (mfhd 1 and 2): MFU sample numbers from 1 in each fragment or on through the MPU, the moofs first or
    // after their samples; a lost sample costs its fragment the samples from it on (and those before it presented after it), and the next
    // fragment keeps its place in time
    {
        auto setMfhd = [](Bytes& moof, uint32_t n) { for (size_t q = 8; q + 16 < moof.size(); q++) if (!memcmp(&moof[q], "mfhd", 4)) { dect2::endian::storeBe32(&moof[q + 8], n); return; } };
        for (int through = 0; through < 2; through++)
            for (int moofLast = 0; moofLast < 2; moofLast++) {
                Sender s{100};
                const int kTwo = 4;
                for (int k = 0; k < kTwo; k++) {
                    const uint32_t seq = 4000 + k;
                    Mpu f1 = makeMpu(vinit, vseg[0], seq, "video-1"), f2 = makeMpu(vinit, vseg[1], seq, "video-1");
                    setMfhd(f2.moof, 2);
                    s.pieces(MpuMetadata, seq, f1.meta, 1);
                    uint32_t sn = 1;
                    for (int f = 0; f < 2; f++) {
                        const Mpu& m = f ? f2 : f1;
                        if (!f) sn = 1;
                        if (!through) sn = 1;
                        if (!moofLast) s.pieces(MpuMovieFragment, seq, m.moof, 2);
                        for (size_t i = 0; i < m.samples.size(); i++, sn++) {
                            if (k == 1 && f == 0 && i == 4) continue;   // lost
                            Bytes du; putBe32(du, (uint32_t)f + 1); putBe32(du, sn); putBe32(du, 0); du.push_back(0); du.push_back(0);
                            Bytes all(kHint, 0xA0); add(all, m.samples[i]); add(du, all);
                            s.packet(MmtpMpu, Sender::payload(MpuMfu, 0, 0, seq, du));
                        }
                        if (moofLast) s.pieces(MpuMovieFragment, seq, m.moof, 2);
                    }
                }
                MmtpStats st;
                std::vector<Out> o = run(s.out, &st);
                const size_t played = 4;   // the fragment after it follows on time: the pictures before the lost one all play
                bool ok = !o.empty() && o[0].init && (int)o.size() == 1 + 2 * kTwo;
                uint64_t t0 = 0;
                Bytes file;
                for (size_t i = 0; ok && i < o.size(); i++) {
                    add(file, o[i].data);
                    if (!i) continue;
                    Bytes media; uint64_t tfdt = 0; uint32_t mfhd = 0;
                    ok = readSegment(o[i].data, media, tfdt, mfhd);
                    const int slot = (int)i - 1;   // the third fragment of the stream (MPU 1, fragment 1) lost its fifth sample
                    if (i == 1) t0 = tfdt;
                    Bytes want;
                    const auto& smp = vm[slot % 2 ? 1 : 0].samples;
                    for (size_t x = 0; x < smp.size() && (slot != 2 || x < played); x++) add(want, smp[x]);
                    ok = ok && tfdt == t0 + (uint64_t)slot * 25 * 512 && media == want;
                }
                bool rising = false;
                double a0 = 0, a1 = 0;
                CHECK(ok && st.mpusDelivered == kTwo && st.fragmentsDelivered == 2 * kTwo && st.fragmentsLost == 0 && ffmpegPackets(file, rising, a0, a1) == (2 * kTwo - 1) * 25 + (long)played && rising,
                      through ? "two movie fragments per MPU, samples numbered through the MPU" : "two movie fragments per MPU, samples numbered in each fragment");
            }
    }
    // ---- 14. the sender restarts with numbers a little lower; numbers that wrap past 2^32 while the next MPU starts before the last one ends
    {
        Sender s{100};
        for (int k = 0; k < 5; k++) s.mpu(vm[k % kMpus], 500 + k);
        for (int k = 0; k < 20; k++) s.mpu(vm[k % kMpus], 455 + k);
        MmtpStats st;
        std::vector<Out> o = run(s.out, &st);
        CHECK((int)o.size() >= 1 + 5 + 19 && st.seqRestarts == 1, "a sender restart with lower numbers carries on");
        std::vector<std::vector<UdpDatagram>> per;
        for (int k = 0; k < 8; k++) { Sender t{100}; t.mpu(vm[k % kMpus], 0xFFFFFFFCu + (uint32_t)k, true); per.push_back(t.out); }
        std::vector<UdpDatagram> in;
        for (size_t k = 0; k < per.size(); k++) {
            for (size_t i = 0; i + 1 < per[k].size(); i++) in.push_back(per[k][i]);
            if (k + 1 < per.size()) { in.push_back(per[k + 1][1]); per[k + 1].erase(per[k + 1].begin() + 1); }
            in.push_back(per[k].back());
        }
        o = run(in, &st);
        CHECK(o.size() == 9 && st.mpusDropped == 0, "MPU numbers wrapping past 2^32 keep their order");
    }
    // ---- 15. MFU fragments without data are not kept; tiny ones count what they cost against the limit; an MPU of an asset that stops is
    // given up once the service has moved on
    {
        MmtpStats st;
        for (int bytes = 0; bytes < 2; bytes++) {
            Sender s{100};
            uint32_t off = 0;
            for (int pk = 0; pk < 4000; pk++) {
                Bytes pl; putBe16(pl, 0); pl.push_back((uint8_t)(MpuMfu << 4 | 8 | 1)); pl.push_back(0); putBe32(pl, 42);
                for (int u = 0; u < 90; u++) { putBe16(pl, (uint32_t)(14 + bytes)); putBe32(pl, 1); putBe32(pl, 1); putBe32(pl, off++); pl.push_back(0); pl.push_back(0); if (bytes) pl.push_back(9); }
                dect2::endian::storeBe16(&pl[0], (uint32_t)pl.size() - 2);
                s.packet(MmtpMpu, pl);
            }
            run(s.out, &st);
            CHECK(bytes ? st.mpusDropped == 1 : st.mpusDropped == 0, bytes ? "an MPU of 360000 one-byte fragments goes past the limit" : "fragments without data are not kept");
        }
        Sender a{100}, b{200};
        a.mpu(vm[0], 7);
        a.out.resize(a.out.size() / 2);
        for (int k = 0; k < 80; k++) b.mpu(vm[k % kMpus], 100 + k);
        std::vector<UdpDatagram> in = a.out;
        in.insert(in.end(), b.out.begin(), b.out.end());
        run(in, &st);
        // it has its start, so what came of it goes out as a repaired MPU
        CHECK(st.mpusDropped == 0 && st.mpusDelivered == 81, "the unfinished MPU of an asset that stopped is given up");
    }
    // ---- 16. the parts of the repair: which pictures no other one uses, which samples can be played, the cut-down sample table
    {
        MpuInit h;
        h.nalLengthSize = 4;
        h.maxTemporalId = 2;
        auto nal = [](Bytes& v, int type, int tid, size_t body) { putBe32(v, (uint32_t)(2 + body)); v.push_back((uint8_t)(type << 1)); v.push_back((uint8_t)(tid + 1)); v.insert(v.end(), body, 0x11); };
        Bytes aud; nal(aud, 35, 0, 1);
        Bytes x = aud; nal(x, 0, 2, 50);   // TRAIL_N in the highest sub-layer
        CHECK(sampleDisposable(x.data(), x.size(), h) == 1, "a TRAIL_N picture of the highest sub-layer is used by no other");
        Bytes y = aud; nal(y, 0, 1, 50);
        CHECK(sampleDisposable(y.data(), y.size(), h) == 0, "a TRAIL_N picture of a lower sub-layer may be used");
        Bytes z = aud; nal(z, 1, 2, 50);
        CHECK(sampleDisposable(z.data(), z.size(), h) == 0, "a TRAIL_R picture is used");
        Bytes w; putBe32(w, 500); w.push_back(39 << 1); w.push_back(1); w.insert(w.end(), 20, 0);
        CHECK(sampleDisposable(w.data(), w.size(), h) == -1, "not known when the first picture NAL unit is not in the part that came");
        MpuInit none;
        CHECK(sampleDisposable(x.data(), x.size(), none) == -1, "not known without an HEVC configuration");
        CHECK(vin.nalLengthSize == 4 && vin.maxTemporalId == 0, "NAL unit length size and sub-layers from the test video's hvcC");

        MpuFragment f;
        f.sizes = {10, 20, 30, 40, 50};
        f.durations = {100, 100, 100, 100, 100};
        f.compositionOffsets = {0, 200, 0, 0, 0};
        f.flags = {0, 0x10000, 0x10000, 0, 0x10000};   // sync samples at 0 and 3
        f.trunFlags = 0x000B01;
        auto pl = [&](std::vector<char> whole, std::vector<char> disp, const char* hdlr) { return playableSamples(f, hdlr, whole, disp, true); };
        CHECK((pl({1, 0, 1, 1, 1}, {-1, -1, -1, -1, -1}, "vide") == std::vector<char>{1, 0, 0, 1, 1}), "after a lost picture others may use, play again from the next sync sample");
        CHECK((pl({1, 0, 1, 1, 1}, {-1, 1, -1, -1, -1}, "vide") == std::vector<char>{1, 0, 1, 1, 1}), "a lost picture no other uses costs itself only");
        CHECK((pl({0, 1, 1, 1, 1}, {0, -1, -1, -1, -1}, "vide") == std::vector<char>{0, 0, 0, 1, 1}), "without the first picture nothing plays up to the next sync sample");
        MpuFragment g = f;
        g.flags = {0, 0, 0, 0, 0};
        g.compositionOffsets = {0, 0, 0, 0, 0};
        CHECK((playableSamples(g, "vide", {1, 1, 0, 1, 1}, {-1, -1, -1, -1, -1}, true) == std::vector<char>{1, 1, 0, 0, 0}), "video with no sync information: only the first sample is a sync sample");
        CHECK((playableSamples(g, "soun", {1, 1, 0, 1, 1}, {-1, -1, -1, -1, -1}, true) == std::vector<char>{1, 1, 0, 1, 1}), "sound with no sync information: every sample is one");
        {
            // reordered pictures, presented at 200, 300, ... 1000: decode order I(200) P(600) B(400) b(300) b(500) P(1000) B(800) b(700) b(900)
            MpuFragment r;
            r.sizes.assign(9, 10);
            r.durations.assign(9, 100);
            r.compositionOffsets = {200, 500, 200, 0, 100, 500, 200, 0, 100};
            r.flags = {0, 0x10000, 0x10000, 0x10000, 0x10000, 0x10000, 0x10000, 0x10000, 0x10000};
            r.trunFlags = 0x000F01;
            auto rp = [&](const MpuFragment& m, int lost, int disp, bool gap = true) {
                std::vector<char> whole(9, 1), d(9, -1);
                whole[lost] = 0;
                d[lost] = (char)disp;
                return playableSamples(m, "vide", whole, d, gap);
            };
            CHECK((rp(r, 6, 0) == std::vector<char>{1, 1, 1, 1, 1, 0, 0, 0, 0}), "a picture kept that would be presented after one cut (P at 1000 after 700) is left out too");
            CHECK((rp(r, 7, -1) == std::vector<char>{1, 1, 1, 1, 1, 0, 0, 0, 0}), "left out from the last one kept backwards, up to where all kept come before all cut");
            CHECK((rp(r, 7, 1) == std::vector<char>{1, 1, 1, 1, 1, 1, 1, 0, 1}), "a lost picture no other uses still costs itself only");
            CHECK((rp(r, 3, 0) == std::vector<char>{1, 0, 0, 0, 0, 0, 0, 0, 0}), "back to the sync sample when needed");
            MpuFragment late = r;
            late.compositionOffsets[0] = 400;
            CHECK((rp(late, 3, 0) == std::vector<char>{0, 0, 0, 0, 0, 0, 0, 0, 0}), "a sync sample presented after a picture cut goes too");
            CHECK((rp(r, 1, 0) == std::vector<char>{1, 0, 0, 0, 0, 0, 0, 0, 0}), "nothing to cut back when only a sync sample presented first is kept");
            CHECK((rp(r, 6, 0, false) == std::vector<char>{1, 1, 1, 1, 1, 1, 0, 0, 0}), "when the next fragment follows on time nothing more is left out");
            MpuFragment two = r;   // a second sync sample: P(1000) as a CRA, B(800) b(700) b(900) its leading pictures
            two.flags[5] = 0;
            CHECK((rp(two, 3, 0) == std::vector<char>{1, 1, 1, 0, 0, 1, 1, 1, 1}), "a cut that ends at a sync sample kept costs nothing more");
            CHECK((rp(two, 7, -1) == std::vector<char>{1, 1, 1, 1, 1, 0, 0, 0, 0}), "a sync sample of an open GOP presented after its lost leading pictures goes too");
            {
                std::vector<char> whole(9, 1), d(9, -1);
                whole[3] = whole[6] = 0;
                CHECK((playableSamples(two, "vide", whole, d, true) == std::vector<char>{1, 0, 0, 0, 0, 0, 0, 0, 0}),
                      "cut back past an earlier run of samples left out, which is then at the end too");
            }
            MpuFragment flat = r;
            flat.trunFlags = 0x000301;
            CHECK((rp(flat, 6, 0) == std::vector<char>{1, 1, 1, 1, 1, 1, 0, 0, 0}), "without composition offsets nothing more is left out");
        }

        // sdtp (one byte per sample) follows; sbgp is left out
        Bytes sdtp{0, 0, 0, 0, 1, 2, 3, 4, 5};
        Bytes sbgp(12, 0);
        f.trafOther = box("sdtp", sdtp);
        add(f.trafOther, box("sbgp", sbgp));
        MpuFragment out;
        uint64_t lead = 0;
        CHECK(keepSamples(f, {0, 1, 0, 1, 0}, out, lead) && lead == 100 && out.sizes == std::vector<uint32_t>({20, 40}) && out.durations == std::vector<uint32_t>({200, 200}) &&
                  out.compositionOffsets == std::vector<int64_t>({200, 0}) && out.mediaBytes == 60 && out.trunFlags == 0xF01 && out.trunEntries.size() == 2 * 16 &&
                  out.trafOther == box("sdtp", Bytes{0, 0, 0, 0, 2, 4}),
              "samples left out give their time to the sample before them; the time before the first one kept is the lead");
        MpuFragment empty;
        CHECK(!keepSamples(f, {0, 0, 0, 0, 0}, empty, lead), "nothing to keep");
        // the segment it makes: the data offset at the mdat, the durations kept
        Bytes seg = buildMediaSegment(out, 7, 1000, Bytes(60, 0x5A));
        Bytes media; uint64_t tfdt = 0; uint32_t mfhd = 0, count = 0; uint64_t dur = 0;
        CHECK(readSegment(seg, media, tfdt, mfhd) && trunTimes(seg, count, dur) && media.size() == 60 && tfdt == 1000 && mfhd == 7 && count == 2 && dur == 400, "the cut-down segment");

        // the counters of a repaired service: MPU 3 of the video with its 7th picture's second fragment lost
        Sender s{100};
        s.mfuPiece = 100;
        for (int k = 0; k < 5; k++) s.mpu(vm[k], 1000 + k, false, [&](size_t i, size_t fr) { return k == 3 && i == 6 && fr == 1; });
        MmtpStats st;
        run(s.out, &st);
        CHECK(st.mpusDelivered == 5 && st.mpusDropped == 0 && st.mpusRepaired == 1 && st.fragmentsRepaired == 1 && st.samplesLost == 1 &&
                  (st.samplesCut == 0 || st.samplesCut == (long)vm[3].samples.size() - 7),
              "repair counters");
    }
    // ---- two assets on one timeline: by the MP table's NTP times, or by sequence number
    Mpu am = makeMpu(ainit, aseg[0], 1000, "audio-1");
    const uint64_t ntp0 = 0xEE00000000000000ull;
    auto ntp = [&](double s) { return ntp0 + (uint64_t)(s * 4294967296.0); };
    {
        MpuInit ain;
        readMpuMetadata(ainit.data(), ainit.size(), ain);
        for (int bySeq = 0; bySeq < 2; bySeq++) {
            std::vector<UdpDatagram> in;
            in.push_back(signalling(0x20, {{100, "hvc1", {}}, {200, "mp4a", {}}}, 0));
            in.push_back(signalling(0x12, {{100, "hvc1", {{1000, ntp(0)}, {1001, ntp(1)}}}}, 1, true));
            in.push_back(signalling(0x13, {{200, "mp4a", {{1000, ntp(0.25)}}}}, 2));
            Sender v{100}, a{200};
            v.mpu(vm[0], 1000); v.mpu(vm[1], 1001);
            a.mpu(am, 1000);
            in.insert(in.end(), v.out.begin(), v.out.end());
            in.insert(in.end(), a.out.begin(), a.out.end());
            std::vector<Out> o = run(in, nullptr, bySeq);
            uint64_t vt = 0, at = 0;
            bool gv = false, ga = false;
            for (auto& x : o) {
                Bytes media; uint64_t t = 0; uint32_t f = 0;
                if (x.init || !readSegment(x.data, media, t, f)) continue;
                if (x.pid == 100 && !gv) { vt = t; gv = true; }
                if (x.pid == 200 && !ga) { at = t; ga = true; }
            }
            // the first video sample is shown at the MPU's time (its composition offset is taken off the decode time)
            int64_t cto0 = 0;
            {
                MpuFragment f;
                const auto top = boxes(vm[0].moof, 0, vm[0].moof.size());
                readMpuFragment(vm[0].moof.data(), vm[0].moof.size(), vin, f);
                int64_t d = 0;
                cto0 = INT64_MAX;
                for (size_t i = 0; i < f.durations.size(); i++) { cto0 = std::min(cto0, d + f.compositionOffsets[i]); d += f.durations[i]; }
                (void)top;
            }
            const double vs = (double)((int64_t)vt + cto0) / vin.timescale, as = (double)at / ain.timescale;
            printf("  %s: video starts at %.3f s, audio at %.3f s\n", bySeq ? "by sequence number" : "by MPU time", vs, as);
            CHECK(gv && ga && std::fabs(as - vs - (bySeq ? 0.0 : 0.25)) < 0.002, bySeq ? "assets lined up by sequence number" : "assets lined up by the MPU timestamps");
        }
    }
    // ---- the whole receiver: an MMTP service in the service list, MMTP datagrams, a transport stream with picture and sound
    {
        std::vector<Bytes> ip;
        int id = 1;
        auto send = [&](uint32_t dst, int port, const Bytes& pl) { for (auto& p : makeUdpPackets(sim::kSrc, dst, 4000, port, pl, id++ & 0xFFFF)) ip.push_back(p); };
        const std::string slt = "<SLT bsid=\"4660\"><Service serviceId=\"1002\" majorChannelNo=\"7\" minorChannelNo=\"2\" serviceCategory=\"1\" shortServiceName=\"MMT\" sltSvcSeqNum=\"0\">"
                                "<BroadcastSvcSignaling slsProtocol=\"2\" slsDestinationIpAddress=\"239.255.5.1\" slsDestinationUdpPort=\"3000\" slsSourceIpAddress=\"10.1.1.5\"/></Service></SLT>";
        send(kLlsAddress, kLlsPort, makeLls(1, 0, 1, slt));
        std::vector<UdpDatagram> in;
        in.push_back(signalling(0x20, {{100, "hvc1", {}}, {200, "mp4a", {}}}, 0));
        Sender v{100}, a{200};
        for (int k = 0; k < 3; k++) v.mpu(vm[k], 1000 + k);
        a.mpu(am, 1000);
        in.insert(in.end(), a.out.begin(), a.out.end());
        in.insert(in.end(), v.out.begin(), v.out.end());
        for (auto& d : in) send(d.dstIp, d.dstPort, d.payload);
        Atsc3Receiver rx;
        rx.setAutoSelect(true);
        std::vector<uint8_t> ts;
        std::thread reader([&] { uint8_t buf[8192]; for (;;) { int k = rx.readTs(buf, sizeof buf); if (k <= 0) break; ts.insert(ts.end(), buf, buf + k); } });
        for (auto& p : ip) rx.pushIp(p.data(), p.size());
        for (int i = 0; i < 60; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); size_t s0 = ts.size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (s0 == ts.size() && s0 > 0) break; }
        rx.stop();
        reader.join();
        const auto st = rx.stats();
        CHECK(rx.selectedService() == 1002 && rx.serviceReady(), "the MMTP service is chosen and ready");
        const auto comps = rx.components();
        CHECK(comps.size() == 2 && comps[0].tsi == 100 && comps[0].contentType == "video" && comps[1].contentType == "audio", "components from the MP table");
        CHECK(st.mpusDelivered == 4 && st.mpusDropped == 0, "all MPUs delivered");
        long vc = 0, ac = 0;
        bool hevc = false, aac = false;
        CHECK(sim::countTs(ts, vc, ac, hevc, aac) && hevc && aac, "HEVC video and AAC audio in the transport stream");
        printf("  receiver: %zu bytes of transport stream, %ld video and %ld audio frames\n", ts.size(), vc, ac);
        CHECK(vc >= 70 && ac >= 130, "nearly all frames came through");
    }
    printf(fails ? "atsc3 mmtp: FAILED\n" : "atsc3 mmtp: ok\n");
    return fails ? 1 : 0;
}
