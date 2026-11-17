// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MMTP: packet and MPU payload headers, the MP table, and whole services built here from the fragmented test files: each media segment
// becomes an MPU the way ATSC 3.0 senders make them (ftyp + mmpu + moov with an MMT hint track in front of the media track, a moof with a
// hint traf, MFUs that carry a 34-byte hint sample in front of the media and a 14-byte header on every fragment), split into MMTP packets.
// Checked: the rebuilt fragmented MP4 (media samples byte for byte, one init segment, a running timeline that FFmpeg reads), shuffled,
// duplicated and lost packets, a corrupted MPU sequence number, the limits, the alignment of two assets, and the whole receiver from an
// MMTP service in the service list to a transport stream. Needs the test files: test_atsc3_mmtp <video.mp4> <audio.mp4>
#include "atsc3_sim.h"
#include "dect2/atsc3/atsc3_mmtp.h"
#include "dect2/common/endian.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    void mpu(const Mpu& m, uint32_t seq, bool moofLast = false) {
        pieces(MpuMetadata, seq, m.meta, metaPieces);
        if (!moofLast) pieces(MpuMovieFragment, seq, m.moof, moofPieces);
        for (size_t s = 0; s < m.samples.size(); s++) {
            Bytes all(kHint, (uint8_t)(0xA0 + s));   // the hint sample, then the media
            add(all, m.samples[s]);
            std::vector<std::pair<uint32_t, Bytes>> frags;
            for (size_t o = 0; o < all.size(); o += mfuPiece) frags.push_back({(uint32_t)(o == 0 ? 0 : o - kHint), Bytes(all.begin() + o, all.begin() + std::min(all.size(), o + mfuPiece))});
            for (size_t k = 0; k < frags.size(); k++) {
                Bytes du; putBe32(du, 1); putBe32(du, (uint32_t)s + 1); putBe32(du, frags[k].first); du.push_back(0); du.push_back(0);
                add(du, frags[k].second);
                const int n = (int)frags.size();
                packet(MmtpMpu, payload(MpuMfu, n == 1 ? 0 : k == 0 ? 1 : k + 1 == frags.size() ? 3 : 2, n - 1 - (int)k, seq, du));
            }
        }
        if (moofLast) pieces(MpuMovieFragment, seq, m.moof, moofPieces);
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
    // ---- 7. a lost fragment: that MPU is left out, the ones after it still come, in order, with the gap in their time
    {
        for (int trial = 0; trial < 4; trial++) {
            Sender s{100};
            std::vector<int> lostMpu;
            for (int k = 0; k < kMpus; k++) {
                Sender one{100};
                one.psn = s.psn;
                one.mpu(vm[k], 1000 + k);
                const bool lose = (int)(rng() % 3) == 0 && k > 0 && k + 1 < kMpus;
                if (lose) { one.out.erase(one.out.begin() + 3 + rng() % (one.out.size() - 4)); lostMpu.push_back(k); }
                s.out.insert(s.out.end(), one.out.begin(), one.out.end());
                s.psn = one.psn;
            }
            MmtpStats st;
            std::vector<Out> o = run(s.out, &st);
            bool ok = (int)o.size() == kMpus + 1 - (int)lostMpu.size() && o[0].init;
            uint64_t t0 = 0;
            for (size_t i = 1; ok && i < o.size(); i++) {
                Bytes media; uint64_t tfdt = 0; uint32_t mfhd = 0;
                ok = readSegment(o[i].data, media, tfdt, mfhd);
                if (i == 1) t0 = tfdt;
                // which MPU it is: count the lost ones before it
                int k = (int)i - 1;
                for (int l : lostMpu) if (l <= k) k++;
                ok = ok && tfdt == t0 + (uint64_t)k * 25 * 512;
            }
            CHECK(ok && st.mpusDropped >= (long)lostMpu.size(), "a lost fragment costs its MPU only, and time goes on across the gap");
        }
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
