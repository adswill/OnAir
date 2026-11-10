// Several PLPs in one signal: a common PLP (type 0), a type 1 data PLP and sub-sliced type 2 PLPs. Every selection decodes identical frames, and the common PLP's packets
// merge into the data PLP's nulls (annex D). Simulated signals.
#include "dect2/t2/t2gen.h"
#include "dect2/t2/t2rx.h"
#include "dect2/t2/t2plp_merge.h"
#include "dect2/tv/bbunpack.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
struct Cfg { const char* name; int subSlices; int wantPlp; int expectCommon; double snr; };
static void run(const Cfg& c) {
    uint64_t nA = 0, nC = 0;
    auto srcData = [&nA](uint8_t* p) { memset(p, 0, 188); const bool slot = (nA % 4) == 1; const int pid = slot ? 0x1FFF : 0x200; p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = 0x10 | (nA & 15); for (int k = 4; k < 188; k++) p[k] = (uint8_t)(nA + k); nA++; };
    auto srcCommon = [&nC](uint8_t* p) { memset(p, 0, 188); const bool slot = (nC % 4) == 1; const int pid = slot ? 0x11 : 0x1FFF; p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = 0x10 | (nC & 15); for (int k = 4; k < 188; k++) p[k] = (uint8_t)(nC * 3 + k); nC++; };
    auto srcB = [](uint8_t* p) { static uint64_t n = 0; memset(p, 0x77, 188); p[0] = 0x47; p[1] = 0x03; p[2] = 0x00; p[3] = 0x10 | (n++ & 15); };
    auto srcD = [](uint8_t* p) { static uint64_t n = 0; memset(p, 0x33, 188); p[0] = 0x47; p[1] = 0x04; p[2] = 0x00; p[3] = 0x10 | (n++ & 15); };
    TxParams tp; tp.s1 = 0; tp.s2field1 = 1; tp.giIdx = 2; tp.pp = 0; tp.payload = true; tp.plpRot = false; tp.subSlices = c.subSlices;
    TxPlp cm; cm.id = 0; cm.type = 0; cm.group = 1; cm.mod = 1; cm.cod = 2; cm.blocks = 2; cm.tsSource = srcCommon;
    TxPlp t1; t1.id = 1; t1.type = 1; t1.group = 1; t1.mod = 2; t1.cod = 2; t1.blocks = 2; t1.tsSource = srcData;
    TxPlp t2a; t2a.id = 2; t2a.type = 2; t2a.group = 1; t2a.mod = 2; t2a.cod = 3; t2a.blocks = 3; t2a.tsSource = srcB;
    TxPlp t2b; t2b.id = 3; t2b.type = 2; t2b.group = 2; t2b.mod = 1; t2b.cod = 3; t2b.blocks = 2; t2b.tsSource = srcD;
    tp.plps = {cm, t1, t2a, t2b};
    T2Generator gen(tp);
    const double bw = 8, fn = nativeRateHz(bw);
    T2Receiver rx; rx.configure(fn, bw); rx.selectPlp(c.wantPlp); rx.setBlocking(true);   // every frame decoded: a loaded runner made the decoder drop some
    std::mutex mu; std::vector<PlpResult> res;
    rx.setPlpCallback([&](const PlpResult& r) { std::lock_guard<std::mutex> k(mu); res.push_back(r); });
    std::map<int, std::map<int, std::vector<std::vector<uint8_t>>>> sent;   // plp -> frame -> frames
    std::mt19937 rng(3); dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> f;
    for (int i = 0; i < 12; i++) {
        gen.nextFrame(f);
        for (auto& q : tp.plps) sent[q.id][i & 0xff] = gen.lastBbFrames(q.id);
        double pw = 0; for (auto& v : f) pw += std::norm(v); pw /= f.size();
        const float sg = (float)std::sqrt(pw / std::pow(10.0, c.snr / 10) / 2);
        for (auto& v : f) v += cf32{nd(rng), nd(rng)} * sg;
        auto wanted = [&] { std::lock_guard<std::mutex> k(mu); size_t n = 0; for (auto& r : res) n += r.plpId == c.wantPlp; return n; };   // the common PLP's frames come out too
        const size_t before = wanted();
        for (size_t o = 0; o < f.size(); o += 1 << 16) rx.feed(f.data() + o, std::min<size_t>(1 << 16, f.size() - o));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        // once frames come out, wait for the next one before feeding more: the PLP decoder drops a frame when its queue backs up, which a fixed
        // pace caused on a loaded CI runner (a live radio's backlog is not what this test is about)
        for (int w = 0; before > 0 && w < 300 && wanted() <= before; w++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    RxTelemetry t; uint64_t q = 0; size_t seen = 0;
    for (int i = 0, still = 0; i < 1500 && still < 300; i++) { rx.telemetry(t, q); std::this_thread::sleep_for(std::chrono::milliseconds(10)); size_t n; { std::lock_guard<std::mutex> k(mu); n = res.size(); } still = n == seen ? still + 1 : 0; seen = n; }
    std::map<int, int> same, bad, frames;
    for (auto& r : res) { auto& m = sent[r.plpId]; auto it = m.find(r.t2Frame); if (it == m.end()) continue; frames[r.plpId]++;
        for (size_t k = 0; k < r.frames.size() && k < it->second.size(); k++) { if (r.frames[k].bits.empty()) continue; (r.frames[k].bits == it->second[k] ? same : bad)[r.plpId]++; } }
    printf("%-22s want %d:", c.name, c.wantPlp); for (auto& kv : frames) printf("  PLP %d: %d frames %d same %d bad", kv.first, kv.second, same[kv.first], bad[kv.first]); printf("  unsupported %zu, decoder dropped %llu\n", t.unsupported.size(), (unsigned long long)t.plpFramesDropped);
    CHECK(frames[c.wantPlp] >= 4 && same[c.wantPlp] >= 8 && bad[c.wantPlp] == 0, "%s: selected PLP %d frames %d same %d bad %d", c.name, c.wantPlp, frames[c.wantPlp], same[c.wantPlp], bad[c.wantPlp]);
    if (c.expectCommon >= 0) CHECK(frames[c.expectCommon] >= 4 && same[c.expectCommon] >= 4 && bad[c.expectCommon] == 0, "%s: common PLP frames %d", c.name, frames[c.expectCommon]);
    else CHECK(frames.count(0) == 0, "%s: common PLP of another group decoded", c.name);
    for (auto& u : t.unsupported) CHECK(u.find("sub-sliced") == std::string::npos, "still flagged: %s", u.c_str());
}
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    run({"type 1 + common", 8, 1, 0, 30});
    run({"type 2, 8 sub-slices", 8, 2, 0, 28});
    run({"type 2, 1 sub-slice", 1, 2, 0, 28});
    run({"type 2, group 2", 16, 3, -1, 24});
    {   // merge
        T2PlpMerge m; std::vector<uint8_t> cf(188 * 3, 0xEE); for (int i = 0; i < 3; i++) { cf[i * 188] = 0x47; cf[i * 188 + 1] = i == 1 ? 0x00 : 0x1F; cf[i * 188 + 2] = i == 1 ? 0x11 : 0xFF; }
        m.commonFrame(cf);
        uint8_t a[188], n[188]; memset(a, 1, 188); a[0] = 0x47; a[1] = 0x02; a[2] = 0x00; memset(n, 0xFF, 188); n[0] = 0x47; n[1] = 0x1F; n[2] = 0xFF;
        uint8_t b[188]; memcpy(b, a, 188); CHECK(!m.step(b) && !memcmp(b, a, 188), "non-null kept");
        memcpy(b, n, 188); CHECK(m.step(b) && b[2] == 0x11, "null replaced");
        memcpy(b, n, 188); CHECK(!m.step(b) && b[2] == 0xFF, "null stays when common is null");
        CHECK(m.replaced() == 1, "count");
    }
    printf(fails ? "t2 plps FAILED\n" : "t2 plps passed\n");
    return fails ? 1 : 0;
}
