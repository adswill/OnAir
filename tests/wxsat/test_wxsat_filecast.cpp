// File broadcasts and MSG raw: bzip2, Fazzt reassembly with hostile input, the transport stream layer, the whole chain on the test signal with impairments, SEVIRI pixels.
// test_wxsat_filecast real <directory with JMA .bz2 segments> [<himawari-rx samples dir>]: real files, untrusted, not in git.
#include "dect2/wxsat/filecast.h"
#include "dect2/wxsat/wxsat_gen.h"
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/goes/goes_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::wxsat;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> unhex(const char* h) { std::vector<uint8_t> o; for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); o.push_back((uint8_t)v); } return o; }
static std::vector<uint8_t> readAll(const std::string& p) { std::ifstream in(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }

// made by Python's bz2 module, so the decoder is checked against another implementation
static const char kPyBz2[] = "425a6839314159265359c741a3cd00008fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb00118164980026000260000000000000000000000000000000000000000000000000000000024c0013000130000000000000000000000000000000000000000000000000000000004a5249b44d0189a6990340c8699321a01a0d0c988d34018201a69820f5343462190d340c8d34340d0d030800d00646400c2006469a686801a01a340cca2a5aaa2dab5c5daaaf55aaf58acd6ab6bec0c35c62aeb1d799196bf60b0d8ac6fb6451110160084200859acf686768b4daad6d36c6a6bb6b636dbadf706eb8dcae774baddaeedf78bcdeaf6e1fbff38838ac0105497f737475c0605db04ef83c26170cda8881f4c462714f0f2f4f6f98bc66371d8fc8645f724fd00020fee602100b4e8020b0042096d9401083ca6555008348082802196cba8020b1799804330d909995c01085010cdb100830c28084300828f08082bceae808432b8788cf67f411315a1d168e2f4919a5d369f51a98d8ed56ae3f5921add76be4761b1d96cf6925b5db6de7e82868a8e92969a9ea2a6aaaeb2b6babec2c6caced2d6dadee2e6eaeef2f6faff03070b0f13171b1f23272b2f33373b3f43474b4f53575b5f63676b6f73777b7f83878b8f93979b9fa3a7abafb3b7bbbe1fc287c7cbcfd3d7dbdfe027e6ebebeff3f45dc914e142431d068f34";

static void testBzip2() {
    std::string want; for (int i = 0; i < 40; i++) want += "OnAir filecast fixture ";
    for (int i = 0; i < 256; i++) want.push_back((char)i);
    want.append(500, 'A');
    std::vector<uint8_t> z = unhex(kPyBz2), out;
    CHECK(bunzip2(z.data(), z.size(), out), "python stream");
    CHECK(std::string(out.begin(), out.end()) == want, "python stream content (%zu bytes)", out.size());
    // our encoder through the decoder: runs at every length that matters to the two run-length steps
    for (int kind = 0; kind < 4; kind++) {
        std::vector<uint8_t> in;
        for (int i = 0; i < 3000; i++) in.push_back(kind == 0 ? (uint8_t)(i * 31) : kind == 1 ? 0x55 : kind == 2 ? (uint8_t)(i / 300) : (uint8_t)(i % 7 == 0 ? i : 9));
        const std::vector<uint8_t> e = bzip2Simple(in.data(), in.size());
        CHECK(bunzip2(e.data(), e.size(), out) && out == in, "round trip kind %d", kind);
    }
    // two streams with rubbish between, as HimawariCast files can be
    std::vector<uint8_t> two = unhex(kPyBz2); two.push_back(0); two.push_back(7);
    const std::vector<uint8_t> z2 = unhex(kPyBz2); two.insert(two.end(), z2.begin(), z2.end());
    CHECK(bunzip2(two.data(), two.size(), out) && out.size() == want.size() * 2, "concatenated streams: %zu", out.size());
    // damage must be seen, never crash
    int caught = 0;
    for (size_t i = 12; i < z.size(); i += 7) { std::vector<uint8_t> b = z; b[i] ^= 0x10; if (!bunzip2(b.data(), b.size(), out) || std::string(out.begin(), out.end()) != want) caught++; }
    CHECK(caught > 40, "damaged streams caught: %d", caught);
    for (size_t n = 0; n < z.size(); n += 11) bunzip2(z.data(), n, out);
    CHECK(!bunzip2(z.data(), z.size(), out, 100), "output limit");
}

static std::vector<uint8_t> head(uint32_t id, const std::string& name, int parts, uint32_t size) {
    std::vector<uint8_t> f(242, 0); f[1] = 3; f[2] = 242;
    for (int i = 0; i < 4; i++) f[(size_t)4 + i] = (uint8_t)(id >> (8 * i));
    f[72] = (uint8_t)parts; f[73] = (uint8_t)(parts >> 8);
    std::memcpy(&f[84], name.data(), name.size());
    const size_t s = 84 + name.size() + 56;
    for (int i = 0; i < 4; i++) f[s + i] = (uint8_t)(size >> (8 * i));
    return f;
}
static std::vector<uint8_t> body(uint32_t id, int part, int payload, uint8_t v) {
    std::vector<uint8_t> f(16 + (size_t)payload, v); f[0] = 0; f[1] = 1; f[2] = (uint8_t)f.size(); f[3] = (uint8_t)(f.size() >> 8);
    for (int i = 0; i < 4; i++) f[(size_t)4 + i] = (uint8_t)(id >> (8 * i));
    f[8] = (uint8_t)part; f[9] = (uint8_t)(part >> 8);
    return f;
}
static std::vector<uint8_t> tail(uint32_t id) { std::vector<uint8_t> f(16, 0); f[1] = 0xFF; f[2] = 16; for (int i = 0; i < 4; i++) f[(size_t)4 + i] = (uint8_t)(id >> (8 * i)); return f; }

static void testFazzt() {
    FazztAssembler a(100);
    std::vector<FazztFile> out;
    const auto push = [&](const std::vector<uint8_t>& f, double t = 0) { a.push(f.data(), f.size(), t, out); };
    // in order, out of order, repeated head, repeated part
    push(head(1, "good.bin", 3, 250));
    push(head(1, "good.bin", 3, 250));
    push(body(1, 2, 100, 3)); push(body(1, 0, 100, 1)); push(body(1, 0, 100, 1)); push(body(1, 1, 100, 2));
    CHECK(out.size() == 1 && out[0].complete && out[0].size == 250 && out[0].data.size() == 250, "complete file");
    if (!out.empty()) CHECK(out[0].data[99] == 1 && out[0].data[100] == 2 && out[0].data[200] == 3, "parts in place");
    out.clear();
    // the carousel repeats a finished file: nothing new
    push(head(1, "good.bin", 3, 250)); push(body(1, 1, 100, 2)); push(tail(1));
    CHECK(out.empty() && a.filesOpen() == 0, "repeat of a finished file");
    // tail with parts missing: goes out marked incomplete
    push(head(2, "half.bin", 4, 350)); push(body(2, 0, 100, 1)); push(tail(2));
    CHECK(out.size() == 1 && !out[0].complete && out[0].have == 1, "incomplete at tail");
    out.clear();
    // silence: the same
    push(head(3, "quiet.bin", 2, 150), 10); push(body(3, 1, 100, 1), 10);
    a.expire(200, out);
    CHECK(out.size() == 1 && !out[0].complete && a.filesOpen() == 0, "incomplete by timeout");
    out.clear();
    // hostile: no crash, nothing opened
    const uint64_t badBefore = a.stats.bad;
    push(head(4, "x.bin", 0, 10));                          // no parts
    push(head(5, "toolarge", 2, 100000));                   // bigger than its parts
    push(head(6, "huge.bin", 65535, 1000));                 // parts * payload is fine here, so it may open: cap below
    push(body(7, 0, 100, 1));                               // no head
    { std::vector<uint8_t> f = head(8, "nul.bin", 1, 10); std::memset(&f[84], 'a', f.size() - 84); push(f); }   // no end of name
    push(std::vector<uint8_t>(5, 0)); push(std::vector<uint8_t>{0, 3, 200, 0, 1, 2, 3, 4, 5}); // short, length beyond the packet
    { auto f = head(9, "idx.bin", 1, 10); push(f); push(body(9, 7, 100, 1)); }                 // part out of range
    CHECK(a.stats.bad > badBefore + 3, "hostile frames counted: %llu", (unsigned long long)(a.stats.bad - badBefore));
    FazztAssembler small(1000); small.maxFileBytes = 4096;
    { auto f = head(1, "big.bin", 100, 90000); small.push(f.data(), f.size(), 0, out); CHECK(small.filesOpen() == 0, "per-file limit"); }
    FazztAssembler tight(1000); tight.maxOpenBytes = 2500; out.clear();
    for (uint32_t id = 1; id <= 3; id++) { auto f = head(id, "file.bin", 1, 900); tight.push(f.data(), f.size(), (double)id, out); }
    CHECK(tight.filesOpen() == 2 && out.size() == 1 && tight.stats.evicted == 1, "memory limit evicts the oldest (%zu open)", tight.filesOpen());
    // names off the air
    CHECK(cleanFileName("../../etc/passwd") == "_.._etc_passwd", "traversal: %s", cleanFileName("../../etc/passwd").c_str());
    CHECK(cleanFileName("..") == "" && cleanFileName("C:\\x\ty") == "C__x_y", "names");
    CHECK(parseHritName("IMG_DK01IR1_201703080230_001").ok && parseHritName("IMG_DK01B13_201703080230_010").channel == "B13", "segment names");
    CHECK(!parseHritName("IMG_XX01IR1_201703080230_001").ok && !parseHritName("SOMETHING.txt").ok, "other names");
}

static std::vector<uint8_t> testSegment(const char* stamp, int n) {
    char nm[64]; snprintf(nm, sizeof nm, "IMG_DK01IR1_%s_%03d", stamp, n + 1);
    goes::Headers h;
    h.fileType = 0; h.hasImage = true; h.bpp = 16; h.columns = kFcTestColumns; h.lines = kFcTestSegLines; h.compression = 0;
    h.annotation = nm; h.hasSeg = true; h.segSeq = n + 1; h.maxSeg = kFcTestSegments; h.startLine = n * kFcTestSegLines + 1;
    std::vector<uint8_t> px((size_t)kFcTestColumns * kFcTestSegLines * 2);
    for (int y = 0; y < kFcTestSegLines; y++) for (int x = 0; x < kFcTestColumns; x++) { const int v = filecastTestPixel(x, y + n * kFcTestSegLines); const size_t i = (size_t)(y * kFcTestColumns + x) * 2; px[i] = (uint8_t)(v >> 8); px[i + 1] = (uint8_t)v; }
    return goes::makeFile(h, px, 1);
}

static void testMosaic() {
    HritMosaic m;
    struct Got { std::string stamp; int w, h, have, total, bad; };
    std::vector<Got> got;
    m.onImage = [&](const std::string&, const std::string& s, int w, int h, const uint8_t* px, int have, int total) {
        int bad = 0;
        if (have == total) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (px[(size_t)y * w + x] != (filecastTestPixel(x, y) >> 2)) bad++;
        got.push_back({s, w, h, have, total, bad});
    };
    char nm[64];
    // out of order, one segment twice: one whole picture, every pixel right
    for (int n : {3, 0, 9, 1, 2, 2, 4, 5, 6, 7, 8}) { snprintf(nm, sizeof nm, "IMG_DK01IR1_202611090230_%03d", n + 1); CHECK(m.push(nm, testSegment("202611090230", n)), "segment"); }
    CHECK(got.size() == 1 && got[0].have == kFcTestSegments && got[0].h == kFcTestSegLines * kFcTestSegments && got[0].bad == 0, "whole picture (%zu, bad %d)", got.size(), got.empty() ? -1 : got[0].bad);
    // a segment lost: the next scan pushes out the old one, marked partial
    for (int n = 0; n < 5; n++) { snprintf(nm, sizeof nm, "IMG_DK01IR1_202611090240_%03d", n + 1); m.push(nm, testSegment("202611090240", n)); }
    snprintf(nm, sizeof nm, "IMG_DK01IR1_202611090250_%03d", 1); m.push(nm, testSegment("202611090250", 0));
    CHECK(got.size() == 2 && got[1].stamp == "202611090240" && got[1].have == 5, "partial picture pushed out");
    // damage: too short for its own headers, or not HRIT at all
    std::vector<uint8_t> cut = testSegment("202611090300", 0); cut.resize(cut.size() / 2);
    const uint64_t before = m.bad;
    CHECK(m.push("IMG_DK01IR1_202611090300_001", cut) && m.bad == before + 1, "cut segment");
    CHECK(m.push("IMG_DK01IR1_202611090300_002", std::vector<uint8_t>(40, 0)) && m.bad == before + 2, "not HRIT");
    CHECK(!m.push("SOMETHING_ELSE.txt", std::vector<uint8_t>(40, 0)), "other file");
}

static void waitTs(FilecastLayer& l, int profile, int nPackets) {
    auto src = filecastTestTs(profile);
    std::vector<uint8_t> b(188 * 256);
    for (int done = 0; done < nPackets; done += 256) { for (int i = 0; i < 256; i++) src(&b[(size_t)i * 188]); l.pushTs(b.data(), 256, 0.01); }
    l.flush();
}

static void checkPicture(const FilecastTelemetry& t) {
    CHECK(t.pictures.size() == 1, "%zu pictures", t.pictures.size());
    if (t.pictures.empty() || !t.pictures[0].px) return;
    const FilecastPicture& p = t.pictures[0];
    // the carousel never stops, so the newest picture may be part way through: its segments come in order, and only those that have landed are compared here (testMosaic checks whole ones)
    CHECK(p.channel == "IR1" && p.have >= 1 && p.total == kFcTestSegments, "picture %s %d/%d", p.channel.c_str(), p.have, p.total);
    CHECK(p.w == kFcTestColumns && p.h == kFcTestSegLines * kFcTestSegments, "size %d x %d", p.w, p.h);
    int bad = 0;
    for (int y = 0; y < p.have * kFcTestSegLines; y += 3) for (int x = 0; x < p.w; x += 5) if ((*p.px)[(size_t)y * p.w + x] != (filecastTestPixel(x, y) >> 2)) bad++;
    if (bad) {   // which segments, and how: a whole segment of zeros has not landed, anything else is damage
        printf("    picture %s %s: %d/%d segments\n", p.channel.c_str(), p.stamp.c_str(), p.have, p.total);
        for (int s = 0; s < p.total; s++) {
            int wrong = 0, zero = 0, n = 0;
            for (int y = s * kFcTestSegLines; y < (s + 1) * kFcTestSegLines; y += 3) for (int x = 0; x < p.w; x += 5) { const int v = (*p.px)[(size_t)y * p.w + x]; n++; zero += v == 0; wrong += v != (filecastTestPixel(x, y) >> 2); }
            printf("      segment %2d: %d of %d wrong, %d zero\n", s + 1, wrong, n, zero);
        }
    }
    CHECK(bad == 0, "%d wrong pixels", bad);
}

static void testLayer() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "onair_filecast_test";
    std::filesystem::remove_all(dir);
    {
        FilecastLayer l(kWxHimawariCast);
        l.setSaveDir((dir / "h").string());
        waitTs(l, kWxHimawariCast, 40000);
        const FilecastTelemetry t = l.telemetry();
        printf("  HimawariCast TS: %llu datagrams, %llu files, %llu partial, %llu failed, %llu pictures\n", (unsigned long long)t.datagrams, (unsigned long long)t.filesComplete, (unsigned long long)t.filesPartial, (unsigned long long)t.filesFailed, (unsigned long long)t.picturesDone);
        CHECK(t.filesComplete >= 11 && t.filesFailed == 0, "files %llu, failed %llu", (unsigned long long)t.filesComplete, (unsigned long long)t.filesFailed);
        CHECK(t.picturesDone >= 1, "no picture finished");
        checkPicture(t);
        CHECK(std::filesystem::exists(dir / "h" / "IMAGES" / "IR1_202611090230.png"), "PNG not saved");
        CHECK(std::filesystem::exists(dir / "h" / "HRIT" / "IMG_DK01IR1_202611090230_001.hrit"), "HRIT not saved");
        CHECK(std::filesystem::exists(dir / "h" / "ADD" / "NOTICE_202611090230.txt"), "other file not saved");
    }
    {
        FilecastLayer l(kWxGeoNetCast);
        l.setSaveDir((dir / "g").string());
        waitTs(l, kWxGeoNetCast, 8000);
        const FilecastTelemetry t = l.telemetry();
        CHECK(t.filesComplete >= 5 && t.pictures.empty(), "GeoNetCast files %llu", (unsigned long long)t.filesComplete);
        CHECK(std::filesystem::exists(dir / "g" / "COMPLETE" / "OR_TEST-00_G16_s2026313.nc"), "GeoNetCast name tag not removed");
        const std::vector<uint8_t> f = readAll((dir / "g" / "COMPLETE" / "OR_TEST-00_G16_s2026313.nc").string());
        bool same = f.size() == 5000;
        for (size_t i = 0; same && i < f.size(); i++) same = f[i] == (uint8_t)(i * 7);
        CHECK(same, "GeoNetCast content (%zu bytes)", f.size());
    }
    {   // a wrong PID finds nothing
        FilecastLayer l(kWxHimawariCast);
        l.setPid(33);
        waitTs(l, kWxHimawariCast, 4000);
        CHECK(l.telemetry().datagrams == 0, "other PID");
    }
    std::filesystem::remove_all(dir);
}

struct Rf { int profile; double rate, secs, snr, cfo, ppm, sym; };
static WxsatTelemetry runRf(const Rf& k) {
    WxsatGenConfig g; g.profile = k.profile; g.rate = k.rate; g.snrDb = k.snr; g.cfoHz = k.cfo; g.clockPpm = k.ppm; g.seed = 5; g.fileSymbolRate = k.sym;
    WxsatGenerator gen(g);
    WxsatReceiver rx(k.profile);
    rx.setBlocking(true);   // as for a file: this test is about the decoding, not about how busy the machine is
    rx.configure(k.rate);
    if (k.sym > 0) rx.setFilecastSymbolRate(k.sym);
    std::vector<cf32> buf(1 << 16);
    for (double done = 0; done < k.secs * k.rate; done += (double)buf.size()) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    rx.flush();
    WxsatTelemetry t; rx.telemetry(t, 0);
    printf("  %s snr %.0f cfo %.0f ppm %.0f: %s; state %d, %llu ok %llu bad, snr %.1f\n", wxProfile(k.profile).name, k.snr, k.cfo, k.ppm, wxsatSummary(t).c_str(), t.state, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.snrDb);
    return t;
}

static void testMsg() {
    Rf k{kWxMsgRaw, 6e6, 2.5, 8, 0, 0, 0};
    for (const auto& c : {Rf{kWxMsgRaw, 6e6, 2.5, 30, 0, 0, 0}, Rf{kWxMsgRaw, 6e6, 2.5, 6, 40000, 20, 0}}) {
        k = c;
        const WxsatTelemetry t = runRf(k);
        CHECK(t.scid == 323, "spacecraft id %d", t.scid);
        CHECK(t.satName == "Meteosat-10 (MSG-3)", "name '%s'", t.satName.c_str());
        CHECK(t.packets >= 32 && t.lines >= 2, "packets %llu, lines %llu", (unsigned long long)t.packets, (unsigned long long)t.lines);
        CHECK(t.blocksBad == 0 || t.blocksOk > t.blocksBad * 50, "bad %llu", (unsigned long long)t.blocksBad);
        if (!t.image) { CHECK(false, "no picture"); continue; }
        const WxImage& im = *t.image;
        CHECK(im.width == kSeviriNrmOutW && im.channels == kSeviriNrmChannels, "shape");
        int good = 0, seen = 0, channelLines = 0;
        for (const auto& l : im.line) {
            if (!l) continue;
            const int row = (kSeviriNrmOutH - 1 - l->idx) * 2;
            for (int ch = 0; ch < kSeviriNrmChannels; ch++) {
              // a channel-line not received yet (the first and last steps of the window) is empty: not an error
              bool any = false;
              for (int x = 0; x < kSeviriNrmOutW && !any; x++) any = l->px[(size_t)ch * im.width + (size_t)x] != 0;
              if (!any) continue;
              channelLines++;
              for (int x = 0; x < kSeviriNrmOutW; x += 13) {
                seen++;
                if (l->px[(size_t)ch * im.width + (size_t)(kSeviriNrmOutW - 1 - x)] == (uint8_t)((wxTestPixel(ch, x * 2, row % 400) & 1023) >> 2)) good++;
              }
            }
        }
        printf("    SEVIRI pixels: %d of %d right, %d channel-lines\n", good, seen, channelLines);
        CHECK(channelLines >= 22, "only %d channel-lines", channelLines);
        CHECK(seen > 0 && good == seen, "pixels %d of %d", good, seen);
        CHECK(t.image2 && t.image2->lines > 0, "HRV picture");
    }
}

static void testRf() {
    const WxsatTelemetry a = runRf({kWxHimawariCast, 6e6, 4, 10, 60000, 15, 0});
    CHECK(a.files.filesComplete >= 11, "HimawariCast files %llu", (unsigned long long)a.files.filesComplete);
    checkPicture(a.files);
    // the 30 Msym/s link's chain (8PSK 5/6, PID 4201) at a rate a test can afford
    const WxsatTelemetry g = runRf({kWxGeoNetCast, 8e6, 3, 16, 30000, 0, 3e6});
    CHECK(g.files.filesComplete >= 5, "GeoNetCast files %llu", (unsigned long long)g.files.filesComplete);
}

// The receiver destroyed while the DVB-S2 workers are still decoding (no flush): they hand packets to the file layer, which must still be
// there. Destruction freed the file layer first and a worker wrote into freed memory (custom_bandwidth_all segfaulted at GeoNetCast).
static void testTeardown() {
    WxsatGenConfig g; g.profile = kWxGeoNetCast; g.rate = 8e6; g.snrDb = 16; g.seed = 6; g.fileSymbolRate = 3e6;
    WxsatGenerator gen(g);
    std::vector<cf32> buf(1 << 16);
    for (int run = 0; run < 6; run++) {
        WxsatReceiver rx(kWxGeoNetCast);
        rx.configure(g.rate);
        rx.setFilecastSymbolRate(3e6);
        for (double done = 0; done < (0.6 + 0.1 * run) * g.rate; done += (double)buf.size()) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    }   // destroyed with frames in flight
    printf("  GeoNetCast receiver destroyed mid-stream 6 times: ok\n");
}

// ---- real files
static int real(const char* dir, const char* hrx) {
    FilecastLayer l(kWxHimawariCast);
    const std::string out = (std::filesystem::temp_directory_path() / "onair_filecast_real").string();
    l.setSaveDir(out);
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".bz2") continue;
        FazztFile f;
        f.name = e.path().filename().string();
        f.data = readAll(e.path().string());
        f.size = (uint32_t)f.data.size(); f.parts = f.have = 1; f.complete = true;
        l.pushFile(std::move(f)); n++;
    }
    l.flush();
    FilecastTelemetry t = l.telemetry();
    printf("real JMA segments: %d files in, %llu complete, %llu failed, %llu pictures, %zu channels\n", n, (unsigned long long)t.filesComplete, (unsigned long long)t.filesFailed, (unsigned long long)t.picturesDone, t.pictures.size());
    for (const auto& p : t.pictures) {
        double sum = 0; int zero = 0;
        for (uint8_t v : *p.px) { sum += v; zero += v == 0; }
        printf("  %s %s %dx%d %d/%d mean %.1f, space %.0f%%, %s\n", p.channel.c_str(), p.stamp.c_str(), p.w, p.h, p.have, p.total, sum / (double)p.px->size(), 100.0 * zero / (double)p.px->size(), p.path.c_str());
    }
    CHECK(t.filesFailed == 0 && t.picturesDone >= 1, "real segments");
    if (hrx) {
        const std::vector<uint8_t> info = readAll(std::string(hrx) + "/file_info_packets.bin"), body2 = readAll(std::string(hrx) + "/file_content_packets.bin");
        FazztAssembler a(1411);
        std::vector<FazztFile> out2;
        const auto walk = [&](const std::vector<uint8_t>& d) { size_t i = 0; uint64_t c = 0; while (i + 8 <= d.size()) { const size_t len = d[i + 2] | (size_t)d[i + 3] << 8; if (len < 8 || i + len > d.size()) break; a.push(&d[i], len, 0, out2); i += len; c++; } return c; };
        const uint64_t ni = walk(info), nb = walk(body2);
        printf("real Fazzt packets: %llu info, %llu content; heads %llu, bodies %llu, orphan %llu, bad %llu, repeats %llu, %zu open\n", (unsigned long long)ni, (unsigned long long)nb, (unsigned long long)a.stats.heads, (unsigned long long)a.stats.bodies, (unsigned long long)a.stats.orphan, (unsigned long long)a.stats.bad, (unsigned long long)a.stats.repeats, a.filesOpen());
        CHECK(a.stats.bad == 0 && a.stats.heads == ni && a.filesOpen() > 0, "real Fazzt headers");
    }
    printf(fails ? "real: %d FAILED\n" : "real: all passed\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "real")) return real(argv[2], argc > 3 ? argv[3] : nullptr);
    testBzip2();
    testFazzt();
    testMosaic();
    testLayer();
    testMsg();
    testRf();
    testTeardown();
    printf(fails ? "filecast: %d FAILED\n" : "filecast: all passed\n", fails);
    return fails ? 1 : 0;
}
