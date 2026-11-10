// Teletext, wide screen signalling and captions in the blanking lines: the generator's lines through the slicers directly and through the whole receiver. Simulated (atv_gen).
#include "dect2/atv/atv_vbi.h"
#include "dect2/atv/atv_testkit.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    {   // bit layouts through the slicer on a simple level function
        AtvVbi v; const auto b = atvWssElements(0b1110);
        v.line625([&](double u) { const int i = (int)std::floor((u - 11.0) / 0.2); return i >= 0 && i < (int)b.size() ? (b[(size_t)i] ? 0.7f : 0.f) : 0.f; });
        CHECK(v.status().wssValid && v.status().wssAspect == 0b1110, "WSS bits %d", v.status().wssAspect);
        AtvVbi x; const auto bad = atvWssElements(0b1111);       // even parity
        x.line625([&](double u) { const int i = (int)std::floor((u - 11.0) / 0.2); return i >= 0 && i < (int)bad.size() ? (bad[(size_t)i] ? 0.7f : 0.f) : 0.f; });
        CHECK(!x.status().wssValid, "parity");
    }
    for (double cnr : {44.0, 30.0}) {
        AtvGenConfig c; c.cnrDb = cnr; c.vbiTeletext = true; c.vbiWss = 0b1110;
        const Run r = run(c, 3.0);
        CHECK(r.tel.ttx != nullptr, "decoder");
        printf("CNR %.0f: teletext packets %llu, pages %zu, WSS '%s'\n", cnr, (unsigned long long)r.tel.ttxPackets, r.tel.ttx ? r.tel.ttx->pages().size() : 0, r.tel.wssText.c_str());
        CHECK(r.tel.ttxPackets > 300, "packets");
        CHECK(r.tel.wssText == "full format 16:9 (anamorphic)", "wss");
        TtxPage p; CHECK(r.tel.ttx && r.tel.ttx->page(100, p), "page 100");
        TtxGrid g; ttxRender(p, g);
        printf("  header: %s\n  row 1: %s\n", ttxRowText(g, 0).c_str(), ttxRowText(g, 1).c_str());
        CHECK(ttxRowText(g, 1).find("ONAIR TELETEXT TEST ROW 1") != std::string::npos, "row text");
    }
    {   AtvGenConfig c; c.cnrDb = 40; const Run r = run(c, 2.0); CHECK(r.tel.ttxPackets == 0 && r.tel.wssText.empty(), "no data: %llu", (unsigned long long)r.tel.ttxPackets); }
    {   AtvGenConfig c; c.sys = kAtvM; c.cnrDb = 40; c.vbiCaptions = "HELLO FROM LINE 21 "; const Run r = run(c, 3.0); printf("captions: '%s'\n", r.tel.captions.c_str()); CHECK(r.tel.captions.find("HELLO") != std::string::npos, "captions"); }
    printf(fails ? "atv_vbi: %d FAILED\n" : "atv_vbi: all passed\n", fails);
    return fails ? 1 : 0;
}
