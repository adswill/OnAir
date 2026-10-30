// MHEG-5 interpreter on the synthetic application of tests/tv/mheg_gen.h: start-up, scene change, keys, variables, timers, Quit, and bad input (missing files,
// event loops, damaged BER). Synthetic only: no real UK application was available.
#include "dect2/common/png.h"
#include "dect2/tv/mheg_engine.h"
#include "mheg_gen.h"
#include <map>
using namespace dect2;
using namespace mhegtest;
#include "../common/test_util.h"

static std::vector<uint8_t> tinyPng() {   // never decoded here, only carried
    return {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 1, 2, 3};
}
static MhegEngine::Fetch fetchFrom(const std::map<std::string, std::vector<uint8_t>>& m) {
    return [&m](const std::string& p, std::vector<uint8_t>& out) { auto it = m.find(p); if (it == m.end()) return false; out = it->second; return true; };
}
static std::string texts(const MhegRender& r) {
    std::string s;
    for (auto& o : r.ops) if (o.kind == MhegDrawOp::Text) s += "[" + o.text + "]";
    return s;
}

int main() {
    auto app = makeTestApp(tinyPng());
    std::map<std::string, std::vector<uint8_t>> files;
    for (auto& f : app.files) files[f.path] = f.data;

    {   // start-up, keys, timers, scene changes, Quit
        MhegEngine e(fetchFrom(files));
        CHECK(e.start("a", 0), "start");
        CHECK(e.running() && e.scene() == "s1", "scene s1 after start-up, got '%s'", e.scene().c_str());
        MhegRender r = e.render();
        CHECK(r.ops.size() == 3 && r.ops[0].kind == MhegDrawOp::Rectangle && texts(r) == "[Press red]" && r.ops[2].kind == MhegDrawOp::Bitmap && !r.ops[2].image.empty(), "first picture: %zu ops %s", r.ops.size(), texts(r).c_str());
        CHECK(r.ops[0].fill == 0x0000A0FFu && r.ops[0].x == 60 && r.ops[0].w == 300, "rectangle colour and place");
        CHECK(r.links == 5, "5 links, got %d", r.links);
        e.key(MhegKeys::Red); e.step(10);
        r = e.render();
        CHECK(texts(r) == "[Red pressed]", "red key: %s", texts(r).c_str());
        CHECK(e.variableText("s1", 4) == "1", "presses variable is 1");
        e.key(MhegKeys::Green); e.step(100);
        CHECK(texts(e.render()) == "[Red pressed]", "timer not yet fired");
        e.step(599);
        CHECK(texts(e.render()) == "[Red pressed]", "timer 1 ms early");
        e.step(600);
        CHECK(texts(e.render()) == "[Red pressed][Timer fired]", "timer fired: %s", texts(e.render()).c_str());
        e.key(MhegKeys::Yellow); e.step(700);
        CHECK(e.scene() == "s2" && texts(e.render()) == "[Second scene]", "scene s2");
        e.key(MhegKeys::Red); e.step(710);
        CHECK(e.scene() == "s2" && e.running(), "red does nothing in s2");
        e.key(MhegKeys::Blue); e.step(800);
        CHECK(e.scene() == "s1" && texts(e.render()) == "[Press red]", "back in a fresh s1");
        e.key(MhegKeys::Blue); e.step(900);
        CHECK(!e.running(), "blue in s1 quits");
        e.key(MhegKeys::Red); e.step(1000);
        CHECK(!e.running(), "stays stopped");
    }
    {   // a missing scene file: the application runs, the scene is empty, the failure is counted
        auto m = files; m.erase("s1");
        MhegEngine e(fetchFrom(m));
        CHECK(e.start("a", 0) && e.running() && e.scene().empty() && e.render().ops.empty() && e.stats().fetchFailures >= 1, "missing scene handled");
        CHECK(!MhegEngine(fetchFrom(m)).start("nothing", 0), "missing application");
        auto m2 = files; m2.erase("img/p.png");
        MhegEngine e2(fetchFrom(m2));
        e2.start("a", 0);
        auto r = e2.render();
        CHECK(r.ops.size() == 3 && r.ops[2].missing && r.ops[2].image.empty(), "missing bitmap marked");
    }
    {   // an event loop: a Link on is-running of the scene that sends is-running again, forever; and an action storm
        const std::string g = "s1";
        MhegNode loop = link(2, g, integer(0), kIsRunning, 0, false, {cons(2, 184, {integer(0), integer(0), enumv(kIsRunning)})});
        auto m = files;
        m["s1"] = mhegEncode(cons(2, 1, {ext(g, 0), cons(2, 8, {loop}), ctxInt(51, 4)}));
        MhegEngine e(fetchFrom(m));
        e.start("a", 0);
        CHECK(!e.running(), "runaway event loop stops the application");
        bool said = false;
        for (auto& l : e.log()) if (l.find("runaway") != std::string::npos) said = true;
        CHECK(said, "runaway logged");
        // scene A and B transition to each other at start-up: bounded per step
        MhegNode tA = cons(2, 1, {ext("s1", 0), cons(2, 5, {transitionTo("s2")}), ctxInt(51, 4)});
        MhegNode tB = cons(2, 1, {ext("s2", 0), cons(2, 5, {transitionTo("s1")}), ctxInt(51, 4)});
        auto m3 = files; m3["s1"] = mhegEncode(tA); m3["s2"] = mhegEncode(tB);
        MhegEngine e3(fetchFrom(m3));
        e3.start("a", 0);
        CHECK(!e3.running(), "endless scene ping-pong stops");
    }
    {   // damaged files: flipped bytes must never crash or hang, whatever the engine does with them
        unsigned seed = 7;
        int started = 0;
        for (int iter = 0; iter < 300; iter++) {
            auto m = files;
            for (const char* name : {"a", "s1", "s2"}) {
                auto& d = m[name];
                int flips = 1 + (int)(seed % 5);
                for (int k = 0; k < flips; k++) { seed = seed * 1103515245u + 12345u; d[(seed >> 8) % d.size()] ^= (uint8_t)(1 + (seed >> 20) % 255); }
                if (iter % 7 == 0) d.resize(d.size() * 2 / 3);
            }
            MhegEngine e(fetchFrom(m));
            if (e.start("a", 0)) started++;
            for (int k : {100, 101, 102, 103, 100}) { e.key(k); e.step(1000 + k); }
            e.render();
        }
        CHECK(started > 0, "some damaged variants still start (%d)", started);
    }
    {   // oversized content and a flood of keys do not grow memory without bound
        MhegLimits lim; lim.stringBytes = 12;
        MhegEngine e(fetchFrom(files), lim);
        e.start("a", 0);
        for (int i = 0; i < 1000; i++) e.key(MhegKeys::Digit0);
        e.step(10);
        CHECK(e.running(), "key flood handled");
        CHECK(e.render().ops[1].text == "Press red", "short text kept under the limit");
    }
    return testSummary("mheg_engine");
}
