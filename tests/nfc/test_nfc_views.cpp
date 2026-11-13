// The protocol filter of the NFC / RFID receiver (NfcReceiver::setProtoMask, the Protocol choice of the NFC screen): ISO 14443 Type A, Type B or ISO 15693.
// The three scripted sessions of nfc_gen (Type A with an NDEF read, ISO 15693 inventory, Type B REQB / ATQB), with a carrier error and noise, go into one receiver without a view and one per view, from the same samples:
//   - a view lists exactly the frames, tags and NDEF records of the receiver without a view that belong to its protocol, and nothing of another one;
//   - a view is empty exactly when the receiver without a view has no frame of its protocol; noise alone: a view lists what the receiver without a view lists of its protocol (and nothing else).
// The generator's Type B session is rendered as 100 % ASK Miller, so the receiver labels it Type A: the Type B entry is only checked for the filter, not for decoding a Type B signal.
// Simulated signals only (the sessions are fabricated examples, not captured tags).
#include "dect2/nfc/nfc_engine.h"
#include "dect2/nfc/nfc_gen.h"
#include "dect2/engine/mode_synth.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <algorithm>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<NfcTelemetry> runAll(const SynthConfig& sc, double rate, double secs, const std::vector<uint32_t>& masks) {
    auto syn = nfc::makeNfcSynth(sc, rate);
    std::vector<std::unique_ptr<NfcReceiver>> rx;
    for (uint32_t m : masks) { rx.emplace_back(new NfcReceiver); rx.back()->configure(rate); rx.back()->setProtoMask(m); }
    std::vector<cf32> buf(16384);
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { syn->generate(buf.data(), buf.size()); for (auto& r : rx) r->feed(buf.data(), buf.size()); }
    std::vector<NfcTelemetry> out(rx.size());
    for (size_t i = 0; i < rx.size(); i++) rx[i]->telemetry(out[i], 0);
    return out;
}
static size_t logOf(const NfcTelemetry& t, int proto) { size_t n = 0; for (auto& e : t.log) if (e.proto == proto) n++; return n; }

int main() {
    const int session[3] = {0, 2, 1};   // the generator's session for proto 0 (Type A), 1 (Type B), 2 (ISO 15693)
    for (int p = 0; p < 3; p++) for (double cfo : {0.0, 600.0}) {
        SynthConfig sc; sc.modeOpt[0] = session[p]; sc.snrDb = 30; sc.cfoHz = cfo;
        const auto r = runAll(sc, 4e6, 6, {0u, 1u << 0, 1u << 1, 1u << 2});
        const NfcTelemetry& all = r[0];
        printf("  session %d (proto %d), cfo %.0f: receiver without a view: %zu frames, %zu tags\n", session[p], p, cfo, all.log.size(), all.tags.size());
        CHECK(!all.log.empty(), "session %d: nothing decoded without a view", session[p]);
        for (int v = 0; v < 3; v++) {
            const NfcTelemetry& t = r[(size_t)v + 1];
            for (auto& e : t.log) CHECK(e.proto == v, "view %d: a frame of protocol %d", v, e.proto);
            CHECK(t.log.size() == logOf(all, v), "session %d view %d: %zu frames, the receiver without a view has %zu of that protocol", session[p], v, t.log.size(), logOf(all, v));
            size_t tagsOfV = 0; for (auto& tg : all.tags) { const int pr = tg.type.find("14443-B") != std::string::npos ? 1 : tg.type.find("15693") != std::string::npos ? 2 : 0; if (pr == v) tagsOfV++; }
            CHECK(t.tags.size() == tagsOfV, "session %d view %d: %zu tags (want %zu)", session[p], v, t.tags.size(), tagsOfV);
            CHECK(t.blocksOk == t.log.size() && t.dataValid == !t.log.empty(), "view %d counts", v);
            // the protocol the receiver gives a frame is what its demodulator saw: the Type B session of the generator is rendered as 100 % ASK Miller (see nfc_gen.cpp), so the receiver labels its frames Type A.
            // A view must list exactly what the receiver without a view labels with its protocol, so every protocol the receiver gave a frame is not empty in its view and the others are empty.
            CHECK(t.log.empty() == (logOf(all, v) == 0), "session %d: view %d lists %zu frames, the receiver without a view %zu of that protocol", session[p], v, t.log.size(), logOf(all, v));
        }
    }
    {   // noise alone
        std::mt19937 g(2); std::normal_distribution<float> nd(0.f, 0.02f); std::vector<cf32> n((size_t)(4e6 * 5)); for (auto& v : n) v = cf32(nd(g), nd(g));
        NfcReceiver a, b; a.configure(4e6); b.configure(4e6); b.setProtoMask(1u);
        for (size_t o = 0; o < n.size(); o += 16384) { a.feed(n.data() + o, std::min<size_t>(16384, n.size() - o)); b.feed(n.data() + o, std::min<size_t>(16384, n.size() - o)); }
        NfcTelemetry ta, tb; a.telemetry(ta, 0); b.telemetry(tb, 0);
        size_t a0 = 0; for (auto& e : ta.log) if (e.proto == 0) a0++;
        CHECK(tb.log.size() == a0 && tb.tags.size() <= ta.tags.size(), "noise: %zu frames in the Type A view, %zu of that protocol without a view", tb.log.size(), a0);
        for (auto& e : tb.log) CHECK(e.proto == 0, "noise: a frame of protocol %d in the Type A view", e.proto);
    }
    return testSummary("nfc views");
}
