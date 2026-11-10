// CIS HF modes: every decoder / identifier against its generator, with carrier offsets, noise, a sample clock error and the
// other CIS signals (each identifier must stay quiet on the others). Simulated only: no real recording went through this test.
#include "dect2/cishf/cishf_dsp.h"
#include "dect2/cishf/cishf_rx.h"
#include "dect2/cishf/cishf_tel.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace dect2::cishf;
using dect2::cf32;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run { Status st; std::string text; std::vector<std::string> events; };
static Run run(int kind, const std::vector<cf32>& x, double baud = 0, double shift = 0) {
    auto d = makeDecoder(kind);
    Run r;
    d->setTextSink([&](const std::string& s) { r.text += s; });
    d->setEventSink([&](const std::string& s) { r.events.push_back(s); });
    if (baud > 0 || shift > 0) d->setForced(baud, shift);
    for (size_t i = 0; i < x.size(); i += 480) d->push(x.data() + i, std::min<size_t>(480, x.size() - i));
    r.st = d->status();
    return r;
}
static std::string param(const Status& s, const std::string& k) { for (const auto& p : s.params) if (p.key == k) return p.value; return ""; }
static std::vector<int> hexChars(const std::string& t) {   // the characters of the messages, in order ("[xx]" counted as failed: -1)
    std::vector<int> v;
    std::istringstream is(t);
    std::string w;
    bool inKey = false;
    while (is >> w) {
        if (w == "[start,") { inKey = true; continue; }
        if (inKey) { if (w.back() == ']') inKey = false; continue; }
        if (w == "<EOM>" || w == "[next]") { v.push_back(-2); continue; }
        if (w.size() == 2) v.push_back((int)strtol(w.c_str(), nullptr, 16));
        else if (w.size() == 4 && w[0] == '[') v.push_back(-1);
    }
    return v;
}
static std::vector<int> randomChars(std::mt19937& g, int n) {
    std::vector<int> valid;
    for (int c = 0; c < 128; c++) if (beeValidChar(c) && c != kBeeEom) valid.push_back(c);
    std::vector<int> v;
    for (int i = 0; i < n; i++) v.push_back(valid[g() % valid.size()]);
    return v;
}

int main() {
    std::mt19937 g(7);
    const bool L = std::getenv("ONAIR_TEST_LONG") != nullptr;   // the full sweeps; the default run keeps the hardest case of each
    using V = std::vector<double>;
    // ---- frame facts
    {
        int n = 0;
        for (int c = 0; c < 128; c++) n += beeValidChar(c);
        CHECK(n == 35, "3:4 characters: %d", n);
        CHECK(beeValidChar(kBeeEom) == false && ones7(kBeeEom) == 6, "EOM 0x77 is not a 3:4 character");
        const uint16_t f = cis14Frame(2, 0x15, 0x0A, 1);
        CHECK((f & 3) == 2 && ((f >> 12) & 3) == 1, "CIS-14 frame assembly %04X", f);
    }

    // ---- BEE: all traffic rates, both polarities, offsets, noise, clock error; two messages (the second without a start sequence)
    for (double traffic : {50.0, 100.0})
        for (bool inv : {false, true})
            for (double cfo : L ? V{-60.0, 0.0, 85.0} : V{inv ? -60.0 : 85.0})
                for (double snr : L ? V{30.0, 12.0} : V{12.0}) {
                    GenOpts o; o.trafficBaud = traffic; o.idleBaud = traffic == 100 ? 50 : 36; o.shiftHz = traffic == 100 ? 250 : 200; o.invert = inv;
                    o.centreHz = 1700;
                    BeeMsg a; a.key = randomChars(g, 10); a.chars = randomChars(g, 60);
                    BeeMsg b; b.withStart = false; b.chars = randomChars(g, 30);
                    auto x = impair(genBee(o, {a, b}), cfo, snr, 40, (uint32_t)g());
                    Run r = run(Bee, x);
                    std::vector<int> want = a.chars; want.push_back(-2); want.insert(want.end(), b.chars.begin(), b.chars.end()); want.push_back(-2);
                    auto got = hexChars(r.text);
                    int ok = 0;
                    for (size_t i = 0; i < want.size() && i < got.size(); i++) ok += got[i] == want[i];
                    const double frac = (double)ok / want.size();
                    CHECK(frac >= (snr > 20 ? 0.99 : 0.95), "BEE %.0f Bd inv %d cfo %.0f snr %.0f: %d of %zu characters (got %zu)", traffic, inv, cfo, snr, ok, want.size(), got.size());
                    CHECK(r.st.state >= 1 && std::fabs(std::stod(param(r.st, "shift").empty() ? "0" : param(r.st, "shift")) - o.shiftHz) < 12,
                          "BEE shift %s", param(r.st, "shift").c_str());
                }
    // BEE forced (Advanced): 150 Bd traffic with a 500 Hz shift
    {
        GenOpts o; o.trafficBaud = 150; o.idleBaud = 75; o.shiftHz = 500;
        BeeMsg a; a.key = randomChars(g, 10); a.chars = randomChars(g, 50);
        auto x = impair(genBee(o, {a}), 30, 20, 0, 3);
        Run r = run(Bee, x, 150, 500);
        auto got = hexChars(r.text);
        int ok = 0;
        for (size_t i = 0; i < a.chars.size() && i < got.size(); i++) ok += got[i] == a.chars[i];
        CHECK(ok >= 49, "BEE forced 150 Bd: %d of 50", ok);
    }
    // BEE on noise only: no message
    {
        auto x = impair(std::vector<cf32>(12000 * 20, cf32(0, 0)), 0, -100, 0, 9);
        std::mt19937 h(3); std::normal_distribution<float> n(0, 0.1f);
        for (auto& v : x) v = cf32(n(h), n(h));
        Run r = run(Bee, x);
        CHECK(r.text.find("[start") == std::string::npos, "BEE: a message start on noise");
    }

    // ---- CIS-14: frame period found and every field read, offsets, noise, clock error
    for (double cfo : L ? V{-40.0, 0.0, 120.0} : V{120.0})
        for (double snr : L ? V{30.0, 14.0} : V{14.0}) {
            GenOpts o; o.shiftHz = 200; o.centreHz = 1500;
            std::vector<uint16_t> fr;
            std::vector<std::string> want;
            for (int i = 0; i < 400; i++) {
                const int a = (int)(g() % 32), b = (int)(g() % 32), p = (int)(g() % 4);
                fr.push_back(cis14Frame(2, a, b, p));
                char t[32]; snprintf(t, sizeof t, "10 %02X %02X %d%d", a, b, p >> 1, p & 1); want.push_back(t);
            }
            auto x = impair(genCis14(o, fr), cfo, snr, 30, (uint32_t)g());
            Run r = run(Cis14, x);
            std::vector<std::string> got;
            std::istringstream is(r.text);
            std::string line;
            while (std::getline(is, line)) if (!line.empty()) got.push_back(line);
            // align the first decoded frame with the sent ones
            int best = 0;
            for (size_t off = 0; off + got.size() <= want.size() + 2 && off < want.size(); off++) {
                int ok = 0;
                for (size_t i = 0; i < got.size() && off + i < want.size(); i++) ok += got[i] == want[off + i];
                best = std::max(best, ok);
            }
            CHECK(r.st.state == 2 && best >= (snr > 20 ? 340 : 300), "CIS-14 cfo %.0f snr %.0f: %d of 400 frames (state %d, %zu lines)", cfo, snr, best, r.st.state, got.size());
        }

    // ---- CIS-12: pilot offset, PSK order and the phase steps of every carrier
    for (int variant : {0, 1})
        for (double cfo : L ? V{-25.0, 0.0, 40.0} : V{variant ? -25.0 : 40.0})
            for (double snr : L ? V{30.0, 15.0} : V{15.0}) {
                GenOpts o; o.variant = variant; o.symbols = 500;
                std::vector<std::vector<uint8_t>> steps;
                auto x = impair(genCis12(o, &steps), cfo, snr, 20, (uint32_t)g());
                Run r = run(Cis12, x);
                const int q = variant ? 2 : 4;
                CHECK(std::fabs(r.st.cfoHz - cfo) < 1.5, "CIS-12 pilot offset %.2f want %.0f", r.st.cfoHz, cfo);
                CHECK(param(r.st, "modulation") == (q == 2 ? "DBPSK" : "DQPSK"), "CIS-12 order %s want %d", param(r.st, "modulation").c_str(), q);
                std::vector<std::string> got;
                std::istringstream is(r.text);
                std::string w;
                while (is >> w) if (w.size() == 12) got.push_back(w);
                size_t bestOk = 0, total = 0;
                for (size_t off = 0; off < steps.size(); off++) {
                    size_t ok = 0, tot = 0;
                    for (size_t i = 0; i < got.size() && off + i < steps.size(); i++)
                        for (int k = 0; k < 12; k++) { tot++; ok += got[i][k] - '0' == steps[off + i][k]; }
                    if (ok > bestOk) { bestOk = ok; total = tot; }
                }
                const double ser = total ? 1.0 - (double)bestOk / total : 1;
                CHECK(got.size() >= 450 && ser < (snr > 20 ? 0.01 : 0.05), "CIS-12 q %d cfo %.0f snr %.0f: %zu symbols, step error %.3f", q, cfo, snr, got.size(), ser);
            }

    // ---- CIS-36 (MFSK) identification at its three rates
    for (double baud : {10.0, 20.0, 40.0})
        for (double cfo : L ? V{0.0, 70.0} : V{70.0}) {
            GenOpts o; o.trafficBaud = baud; o.symbols = (int)(baud * 40);
            auto x = impair(genCis36(o), cfo, 15, 20, (uint32_t)g());
            Run r = run(Cis36, x);
            CHECK(param(r.st, "identified") == "CIS-36", "CIS-36 %.0f Bd cfo %.0f: %s", baud, cfo, r.st.mode.c_str());
            CHECK(param(r.st, "symbol rate") == std::to_string((int)baud) + " Bd", "CIS-36 rate %s", param(r.st, "symbol rate").c_str());
        }

    // ---- OFDM HDR variants, CIS-128 and CIS-3000 identification
    static const char* hdrName[] = {"CIS-45", "CIS-45", "CIS-60", "CIS-93"};
    for (int v = 0; v < 4; v++)
        for (double cfo : L ? V{0.0, 30.0} : V{30.0}) {
            GenOpts o; o.variant = v; o.symbols = L ? 400 : 200;
            Run r = run(Hdr, impair(genOfdm(Hdr, o), cfo, 18, 10, (uint32_t)g()));
            CHECK(param(r.st, "identified").find(hdrName[v]) != std::string::npos, "HDR variant %d cfo %.0f: %s", v, cfo, r.st.mode.c_str());
        }
    for (int v = 0; v < 2; v++) {
        GenOpts o; o.variant = v; o.symbols = 300;
        Run r = run(C128, impair(genOfdm(C128, o), 20, 18, 10, (uint32_t)g()));
        CHECK(param(r.st, "identified").find("CIS-128") != std::string::npos, "CIS-128 variant %d: %s", v, r.st.mode.c_str());
    }
    for (double cfo : {0.0, -50.0}) {
        GenOpts o; o.symbols = 3000 * 8;
        Run r = run(C3000, impair(genCis3000(o), cfo, 18, 10, (uint32_t)g()));
        CHECK(param(r.st, "identified") == "CIS-3000", "CIS-3000 cfo %.0f: %s", cfo, r.st.mode.c_str());
        CHECK(param(r.st, "modulation") == "8-PSK", "CIS-3000 order %s", param(r.st, "modulation").c_str());
    }

    // ---- cross checks: each identifier stays quiet on the other signals
    {
        GenOpts o; o.symbols = 400;
        std::vector<std::vector<cf32>> sig;
        sig.push_back(impair(genCis12(o, nullptr), 0, 25, 0, 1));   // 0
        o.trafficBaud = 20; o.symbols = 800; sig.push_back(impair(genCis36(o), 0, 25, 0, 2));   // 1
        o.variant = 0; o.symbols = 400; sig.push_back(impair(genOfdm(Hdr, o), 0, 25, 0, 3));   // 2
        o.symbols = 300; sig.push_back(impair(genOfdm(C128, o), 0, 25, 0, 4));   // 3
        o.symbols = 24000; sig.push_back(impair(genCis3000(o), 0, 25, 0, 5));   // 4
        const int kinds[] = {Cis12, Cis36, Hdr, C128, C3000};
        for (int k = 0; k < 5; k++)
            for (int s = 0; s < 5; s++) {
                if (k == s) continue;
                Run r = run(kinds[k], sig[s]);
                const std::string id = param(r.st, "identified");
                const bool claimed = kinds[k] == Cis12 ? r.st.state == 2 : (!id.empty() && id != "no");
                CHECK(!claimed, "%s claims the %s signal (%s)", kindName(kinds[k]), kindName(kinds[s]), r.st.mode.c_str());
            }
    }
    // ---- the whole receiver: the engine's test signal at a radio rate, the dial 20 kHz below the centre, through the front end
    for (int kind : {(int)Bee, (int)Cis12, (int)C3000}) {
        const double rate = 250000;
        dect2::SynthConfig sc; sc.cfoHz = 15; sc.snrDb = 25;
        auto syn = dect2::makeCisHfSynth(kind, sc, rate);
        dect2::CisHfReceiver rx(kind);
        rx.configure(rate);
        rx.setSignalOffset(-dect2::cisHfTuning(kind).tuneOffsetHz);
        CHECK(rx.ready(), "receiver %d not ready at 250 kHz", kind);
        std::vector<cf32> buf(25000);
        for (int i = 0; i < 10 * (kind == Bee ? 16 : 8); i++) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }   // 16 s (BEE) / 8 s
        dect2::CisHfTelemetry t;
        CHECK(rx.telemetry(t, 0), "no telemetry");
        if (kind == Bee) CHECK(t.state >= 1 && t.text.find("[start") != std::string::npos && t.blocksOk >= 100, "receiver BEE: state %d, %llu characters, %s", t.state, (unsigned long long)t.blocksOk, t.mode.c_str());
        if (kind == Cis12) CHECK(t.state == 2 && std::fabs(t.cfoHz - 15) < 2, "receiver CIS-12: state %d, cfo %.1f, %s", t.state, t.cfoHz, t.mode.c_str());
        if (kind == C3000) {
            bool id = false;
            for (const auto& p : t.params) id |= p.first == "identified" && p.second == "CIS-3000";
            CHECK(id, "receiver CIS-3000: %s", t.mode.c_str());
        }
        CHECK(dect2::cisHfTuning(kind).stdMode == dect2::kCisHfFamily0 + 2 + kind, "tuning number");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("cishf: all checks passed\n");
    return 0;
}
