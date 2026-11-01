// iridium-toolkit .bits output: every line must match iridium-toolkit bitsparser.py's RAW regex (ported to std::regex) and carry the
// fields as gr-iridium iridium-extractor prints them. The test signal live (fed straight in) and from a recording file.
#include "dect2/iridium/iridium_bits.h"
#include "../../core/src/net/netcompat.h"
#include "dect2/common/stable_random.h"
#include "dect2/iridium/iridium_gen.h"
#include "dect2/iridium/iridium_phy.h"
#include "dect2/iridium/iridium_rx.h"
#include "dect2/iridium/iridium_testkit.h"
#include <random>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Seen { size_t nbits; bool downlink; double freqHz; std::vector<uint8_t> bits; };

// Checks one file against the bursts the callback saw (same order: the writer runs just before the callback).
static void checkFile(const char* name, const std::string& path, const std::vector<Seen>& seen, bool needUl) {
    const std::regex re(kIridiumRawRegex);
    std::ifstream in(std::filesystem::u8path(path));
    std::string line;
    size_t n = 0, dl = 0, ul = 0, simplex = 0, badOrder = 0;
    double first = -1, last = -1, maxMs = -1;
    std::string fileInfo;
    while (std::getline(in, line)) {
        std::smatch m;
        const bool ok = std::regex_match(line, m, re);
        CHECK(ok, "%s line %zu does not match the parser's regex: %.120s", name, n, line.c_str());
        if (!ok) { n++; continue; }
        CHECK(m[1] == "RAW", "%s: type %s", name, m[1].str().c_str());
        if (fileInfo.empty()) fileInfo = m[2];
        CHECK(m[2] == fileInfo && std::regex_match(fileInfo, std::regex(R"(i-\d+-t1)")), "%s: file info %s", name, m[2].str().c_str());
        const double ms = std::stod(m[3]);
        const long hz = std::stol(m[4]);
        CHECK(m[3].length() >= 12 && m[4].length() == 10, "%s: field widths", name);
        CHECK(m[5].matched && m[6].matched, "%s: no N: field", name);
        CHECK(m[8] == std::to_string(n) || std::stoull(m[8]) == n, "%s: id %s at %zu", name, m[8].str().c_str(), n);
        const int conf = std::stoi(m[9]);
        CHECK(conf >= 0 && conf <= 100, "%s: confidence %d", name, conf);
        CHECK(std::stod(m[10]) > 0, "%s: level", name);
        const int syms = std::stoi(m[11]);
        const std::string bits = m[12];
        CHECK(m[13].length() == 0, "%s: trailing data", name);
        CHECK(hz >= 1616000000 && hz <= 1626500000, "%s: frequency %ld", name, hz);
        if (hz > 1626000000) simplex++;
        // bit count = unique word (24) + 2 per data symbol (bitsparser: symbols = len(bits) / 2; gr-iridium prints n_symbols - UW_LENGTH)
        CHECK((int)bits.size() == 2 * iridium::kUwLen + 2 * syms, "%s: %zu bits for %d symbols", name, bits.size(), syms);
        const bool isDl = bits.compare(0, 24, iridium::kUwDlBits) == 0, isUl = bits.compare(0, 24, iridium::kUwUlBits) == 0;
        CHECK(isDl || isUl, "%s: no unique word", name);
        dl += isDl; ul += isUl;
        CHECK(syms <= (hz > 1626000000 ? iridium::kMaxSymbolsSimplex : iridium::kMaxSymbolsNormal), "%s: %d symbols", name, syms);
        if (n < seen.size()) {
            const Seen& s = seen[n];
            CHECK(s.nbits == bits.size() - 24 && s.downlink == isDl, "%s line %zu: %zu bits, burst had %zu", name, n, bits.size() - 24, s.nbits);
            CHECK(std::labs(hz - std::lround(s.freqHz)) <= 1, "%s line %zu: %ld Hz, burst %.0f", name, n, hz, s.freqHz);
            bool same = true;
            for (size_t i = 0; i < s.bits.size() && same; i++) same = bits[24 + i] == (s.bits[i] ? '1' : '0');
            CHECK(same, "%s line %zu: bits differ from the demodulated burst", name, n);
        }
        // the worker takes bursts in order of their end: a long burst may start before the short one written ahead of it
        if (ms + 20 < maxMs) badOrder++;
        maxMs = std::max(maxMs, ms);
        if (first < 0) first = ms;
        last = ms;
        n++;
    }
    printf("%-10s %zu lines (%zu downlink, %zu uplink, %zu simplex), %.1f .. %.1f ms, %s\n", name, n, dl, ul, simplex, first, last, fileInfo.c_str());
    CHECK(n == seen.size(), "%s: %zu lines, %zu bursts", name, n, seen.size());
    CHECK(n > 100 && dl > 0 && simplex > 0 && (!needUl || ul > 20), "%s: %zu lines, %zu simplex", name, n, simplex);
    CHECK(badOrder == 0 && last > first, "%s: timestamps out of order %zu times", name, badOrder);
}

static void run(const char* name, const std::string& path, double rate, const std::vector<cf32>* direct, const std::string& cs8, bool needUl = false) {
    IridiumReceiver r;
    r.configure(rate);
    r.setCenterMhz(1622);
    r.setOffline(true);
    std::vector<Seen> seen;
    std::mutex mu;
    r.setBurstCallback([&](const IridiumBurstBits& b, const IridiumFrame&) { std::lock_guard<std::mutex> lk(mu); seen.push_back({b.bits.size(), b.downlink, b.freqHz, b.bits}); });
    std::string err;
    CHECK(r.startBitsFile(path, &err), "%s", err.c_str());
    if (direct) r.feed(direct->data(), direct->size());
    else {
        FILE* f = fopen(cs8.c_str(), "rb");
        std::vector<int8_t> b8(2 * 65536);
        std::vector<cf32> x(65536);
        size_t k;
        while (f && (k = fread(b8.data(), 2, x.size(), f)) > 0) {
            for (size_t i = 0; i < k; i++) x[i] = cf32(b8[2 * i] / 128.f, b8[2 * i + 1] / 128.f);
            r.feed(x.data(), k);
        }
        if (f) fclose(f);
    }
    r.flush();
    r.stopBitsFile();
    CHECK(r.bitsLines() == seen.size(), "%s: %llu lines written, %zu bursts", name, (unsigned long long)r.bitsLines(), seen.size());
    checkFile(name, path, seen, needUl);
}

int main() {
    // the line itself, field by field as gr-iridium's frame printer formats it
    {
        IridiumBurstBits b;
        b.bits = {1, 0, 0, 1};
        b.downlink = true; b.freqHz = 1626270833.4; b.levelDb = -20; b.snrDb = 18; b.confidence = 97.4;
        const std::string s = iridiumRawLine(b, 1791374400, 1791374400.0123456, 7);
        const std::string want = "RAW: i-1791374400-t1 0000012.3456 1626270833 N:-20.00-38.00 I:00000000007  97% 0.10000   2 0011000000110000111100111001";
        CHECK(s == want, "line\n  %s\n  %s", s.c_str(), want.c_str());
        CHECK(std::regex_match(s, std::regex(kIridiumRawRegex)), "regex");
    }
    // live copy: a synthetic burst written with only the UDP stream open arrives as the same RAW line, newline-ended
    {
        dect2::netInit();   // Winsock must be started before the first socket()
        const dect2::sock_t rs = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        CHECK(rs != dect2::kBadSock && bind(rs, (sockaddr*)&a, sizeof a) == 0, "udp bind");
        #ifdef _WIN32
        int al = (int)sizeof a;
#else
        socklen_t al = sizeof a;
#endif
        getsockname(rs, (sockaddr*)&a, &al);
#ifdef _WIN32
        DWORD tv = 2000;
#else
        timeval tv{2, 0};
#endif
        setsockopt(rs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        IridiumBitsWriter w;
        std::string err;
        CHECK(w.openStream(false, "127.0.0.1", ntohs(a.sin_port), &err), "stream %s", err.c_str());
        IridiumBurstBits b;
        b.bits = {1, 0, 0, 1};
        b.downlink = true; b.freqHz = 1626270833.4; b.levelDb = -20; b.snrDb = 18; b.confidence = 97.4;
        w.write(b, 1791374400.0123456);
        char buf[2048];
        const long n = (long)dect2::sockRecv(rs, buf, sizeof buf);
        const std::string want = iridiumRawLine(b, 1791374400, 1791374400.0123456, 0) + "\n";
        CHECK(n == (long)want.size() && std::string(buf, (size_t)std::max(0L, n)) == want, "udp line (%ld bytes)", n);
        CHECK(w.streamLines() == 1 && w.active() && !w.isOpen(), "stream counters");
        dect2::sockClose(rs);
    }
    const double rate = 10e6, secs = 3;
    SynthConfig c;
    c.mode = 21; c.snrDb = 25; c.modeOpt[1] = 3;
    auto syn = makeIridiumSynth(c, rate);
    std::vector<cf32> x((size_t)(secs * rate));
    syn->generate(x.data(), x.size());
    const auto dir = std::filesystem::temp_directory_path() / "dect2_iridium_bits";
    std::filesystem::create_directories(dir);
    run("live", (dir / "live.bits").string(), rate, &x, "");
    // a recording: 8-bit file, read back in chunks
    const std::string cs8 = (dir / "rec.cs8").string();
    {
        FILE* f = fopen(cs8.c_str(), "wb");
        std::vector<int8_t> b8(2 * x.size());
        for (size_t i = 0; i < x.size(); i++) {
            b8[2 * i] = (int8_t)std::lround(std::fmax(-128.f, std::fmin(127.f, x[i].real() * 128)));
            b8[2 * i + 1] = (int8_t)std::lround(std::fmax(-128.f, std::fmin(127.f, x[i].imag() * 128)));
        }
        fwrite(b8.data(), 1, b8.size(), f);
        fclose(f);
    }
    // uplink, long simplex bursts and overlaps (the test signal is downlink only)
    {
        iridiumtest::SceneOpts so; so.rate = rate; so.secs = 2; so.uplinkFrac = 0.4; so.longSimplex = true; so.overlap = true; so.seed = 5;
        const double sigma = 0.01;
        IridiumBurstPlayer pl(rate);
        for (const auto& b : iridiumtest::makeScene(so, sigma)) pl.add(b);
        std::vector<cf32> y((size_t)(so.secs * rate));
        pl.render(y.data(), y.size());
        std::mt19937 g(9);
        dect2::normal_distribution<float> nd(0.f, (float)(sigma / std::sqrt(2.0)));
        for (auto& v : y) v += cf32{nd(g), nd(g)};
        run("scene", (dir / "scene.bits").string(), rate, &y, "", true);
    }
    run("recording", (dir / "sub dir" / "output.bits").string(), rate, nullptr, cs8);
    std::filesystem::remove_all(dir);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
