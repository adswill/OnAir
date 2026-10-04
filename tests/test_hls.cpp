// HLS pipeline: a transport stream goes in, a playlist and segments come out.
//   test_hls                      checks that the demo programme (MPEG-2 picture) is refused with a clear message
//   test_hls <file.ts> [outdir]   streams a recorded transport stream and checks the playlist and the segments; with `outdir` they are
//                                 written there, so that they can be looked at with ffprobe or played with a browser
#include "dect2/demo_ts.h"
#include "dect2/hls.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void waitDone(HlsPipeline& p, double maxSec, bool untilReady) {
    for (int i = 0; i < (int)(maxSec * 10); i++) {
        if (!p.error().empty() || (untilReady && p.ready())) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        auto src = demoTsSource(4e6);
        long bytes = 0;
        HlsPipeline p([&](uint8_t* buf, int size) -> int {
            int n = 0;
            while (n + 188 <= size && n < 188 * 64) { src(buf + n); n += 188; }
            bytes += n;
            return bytes > 6000000 ? 0 : n;
        });
        p.start();
        waitDone(p, 20, false);
        p.stop();
        printf("demo programme: %s\n", p.error().c_str());
        CHECK(!p.error().empty(), "an MPEG-2 programme must be refused with an error message");
        printf(fails ? "hls tests FAILED\n" : "hls tests passed\n");
        return fails ? 1 : 0;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    std::vector<uint8_t> ts((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    HlsPipeline p([&](uint8_t* buf, int size) -> int {
        const size_t n = std::min<size_t>((size_t)size, ts.size() - pos);
        if (n == 0) return 0;
        memcpy(buf, ts.data() + pos, n);
        pos += n;
        std::this_thread::sleep_for(std::chrono::microseconds(100));   // about 150 Mbit/s: faster than the real thing
        return (int)n;
    });
    p.start();
    waitDone(p, 60, true);
    std::string list, type;
    CHECK(p.error().empty(), "the pipeline reported an error");
    if (!p.error().empty()) printf("error: %s\n", p.error().c_str());
    CHECK(p.get("index.m3u8", list, type), "no playlist");
    printf("playlist (%s):\n%s\n", type.c_str(), list.c_str());
    CHECK(list.find("#EXTM3U") == 0, "the playlist must start with #EXTM3U");
    CHECK(list.find("#EXT-X-VERSION") != std::string::npos, "the playlist needs a version");
    // the first listed segment
    std::string seg;
    size_t at = list.find("seg");
    if (at != std::string::npos) seg = list.substr(at, list.find('\n', at) - at);
    std::string body;
    CHECK(!seg.empty() && p.get(seg, body, type), "the first segment is missing");
    CHECK(body.size() > 20000 && (uint8_t)body[0] == 0x47, "a segment must be a transport stream");
    if (argc > 2) {
        for (int i = 0; i < 40; i++) {
            char name[32]; snprintf(name, sizeof name, "seg%05d.ts", i);
            if (p.get(name, body, type)) { std::ofstream o(std::string(argv[2]) + "/" + name, std::ios::binary); o.write(body.data(), (std::streamsize)body.size()); }
        }
        std::ofstream o(std::string(argv[2]) + "/index.m3u8"); o << list;
    }
    p.stop();
    printf(fails ? "hls tests FAILED\n" : "hls tests passed\n");
    return fails ? 1 : 0;
}
