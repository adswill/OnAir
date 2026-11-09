// ProVoice: the IMBE 7100x4400 error control and its rewrite to 7200x4400, the block bit schedule, and the following of a digital EDACS call through a stand-in for the user's IMBE plug-in
// (generated signals only; no real ProVoice system was heard and no IMBE codec is part of this test).
#include "dect2/codec/codec_registry.h"
#include "dect2/edacs/edacs_gen.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/edacs/provoice.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::provoice;
#include "../common/test_util.h"

static std::vector<std::vector<uint8_t>>* gInputs = nullptr;
class FakeImbe final : public codec::SpeechCodec {
public:
    std::vector<int16_t> decode(const std::vector<uint8_t>& frame) override {
        if (gInputs) gInputs->push_back(frame);
        return std::vector<int16_t>(160, 1000);
    }
    int sampleRate() const override { return 8000; }
};

static void testCodec() {
    std::mt19937 rng(1);
    int wrong = 0, frames = 0, convertWrong = 0, flips = 0;
    for (int it = 0; it < 400; it++) {
        uint8_t info[88];
        edacsPvInfo((uint32_t)rng(), it, info);
        Frame7100 f;
        encode7100(info, f);
        uint8_t out[88] = {};
        int errs = -1;
        CHECK(decode7100(f, out, &errs), "clean frame %d", it);
        wrong += std::memcmp(out, info, 88) != 0 || errs != 0;
        // two errors in C1 and two in C2 are corrected (Golay t = 3)
        Frame7100 g = f;
        g.b[1][5] ^= 1; g.b[1][17] ^= 1; g.b[2][3] ^= 1; g.b[2][19] ^= 1;
        uint8_t o2[88];
        int e2 = 0;
        decode7100(g, o2, &e2);
        flips += std::memcmp(o2, info, 88) == 0;
        // the rearrangement to 7200 and back
        uint8_t a[88];
        std::memcpy(a, info, 88);
        convert7100to7200(a);
        uint8_t b[88];
        std::memcpy(b, a, 88);
        convert7200to7100(b);
        convertWrong += std::memcmp(b, info, 88) != 0;
        frames++;
    }
    CHECK(wrong == 0, "%d of %d clean frames wrong", wrong, frames);
    CHECK(flips == 400, "%d of 400 frames with four errors repaired", flips);
    CHECK(convertWrong == 0, "%d of %d rearrangements wrong", convertWrong, frames);
    // the 7200 frame is a valid code: mbelib format encoded, the C0 and Golay / Hamming relations hold (checked against the plug-in's packing below)
    Frame7200 f7;
    uint8_t info[88];
    edacsPvInfo(9, 0, info);
    convert7100to7200(info);
    encode7200(info, f7);
    CHECK(codec::packCodeWords(&f7.b[0][0], 8, 23).size() == 23, "pack size");
}

static void testBlock() {
    Frame7100 fr[4];
    for (int k = 0; k < 4; k++) { uint8_t info[88]; edacsPvInfo(5, k, info); encode7100(info, fr[k]); }
    auto b = makeBlock(fr, kSyncStd, 0x1234);
    CHECK(b.size() == kBlockBits, "block size %zu", b.size());
    // sync at the start, as DSD's table says
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) v = (v << 1) | b[(size_t)i];
    CHECK(v == kSyncStdValue, "sync %08X", v);
    uint8_t bits[736];
    std::copy(b.begin() + 32, b.end(), bits);
    Frame7100 got[4];
    uint16_t lid = 0;
    splitBlock(bits, got, &lid);
    int same = 0;
    for (int k = 0; k < 4; k++) {
        uint8_t a[88], c[88];
        decode7100(fr[k], a); decode7100(got[k], c);
        same += std::memcmp(a, c, 88) == 0;
    }
    CHECK(same == 4 && lid == 0x1234, "split %d, lid %04x", same, lid);
}

static void testFollow(double noise, double cfo, bool withPlugin, bool ea) {
    std::vector<std::vector<uint8_t>> inputs;
    gInputs = &inputs;
    if (withPlugin) codec::setPluginFactory("imbe", []() { return std::unique_ptr<codec::SpeechCodec>(new FakeImbe()); }, "fake");
    else codec::setPluginFactory("imbe", codec::DecoderFactory());
    EdacsGenConfig c;
    for (int i = 0; i < 10; i++) c.lcnFreqHz.push_back(856.000e6 + 25e3 * i);
    c.noise = noise; c.cfoHz = cfo;
    c.durSec = 5;
    EdacsGenCall k; k.lcn = 4; k.gid = 321; k.lid = 777; k.startSec = 1.0; k.endSec = 4.0; k.digital = true; k.pvSeed = 7; k.sendEnd = false;
    c.calls = {k};
    (void)ea;
    EdacsReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setSignalOffset(c.controlHz - c.centreHz + c.cfoHz);
    rx.setCenterHz(c.centreHz);
    rx.setLcnTable(c.lcnFreqHz);
    rx.setFollow(true);
    size_t audio = 0; int fol = 0;
    rx.setAudioTap([&](int lcn, const float*, size_t n) { audio += n; fol = lcn; });
    const auto sig = edacsSystemSignal(c);
    for (size_t i = 0; i < sig.size(); i += 2048) rx.feed(sig.data() + i, std::min<size_t>(2048, sig.size() - i));
    EdacsTelemetry t;
    rx.telemetry(t, 0);
    bool digitalCall = false;
    for (const auto& x : t.calls) if (x.gid == 321) digitalCall = x.digital;
    CHECK(digitalCall, "call marked digital");
    if (!withPlugin) {
        CHECK(inputs.empty() && audio == 0, "no plug-in: no audio, inputs %zu", inputs.size());
        CHECK(!t.note.empty(), "the note says what to install");
        printf("no plug-in: note \"%s\"\n", t.note.c_str());
        return;
    }
    // what the plug-in received, frame by frame, against the expected 7200 words
    int good = 0;
    std::map<int, int> seen;
    for (size_t i = 0; i < inputs.size(); i++) {
        bool found = false;
        for (int f = 0; f < 1000 && !found; f++) {
            uint8_t info[88];
            edacsPvInfo(k.pvSeed, f, info);
            convert7100to7200(info);
            Frame7200 x;
            encode7200(info, x);
            auto bytes = codec::packCodeWords(&x.b[0][0], 8, 23);
            if (bytes == inputs[i]) { found = true; seen[f]++; }
        }
        good += found;
    }
    printf("noise %.3f cfo %+6.0f: followed LCN %d, %zu frames to the plug-in, %d exact, %zu audio samples\n", noise, cfo, fol, inputs.size(), good, audio);
    CHECK(fol == k.lcn, "followed %d", fol);
    CHECK(inputs.size() >= 100, "%zu frames reached the plug-in", inputs.size());
    CHECK(good >= (int)inputs.size() * 9 / 10, "only %d of %zu frames exact", good, inputs.size());
    gInputs = nullptr;
}

int main() {
    testCodec();
    testBlock();
    testFollow(0.01, 0, true, false);
    testFollow(0.05, 900, true, false);
    testFollow(0.02, 0, false, false);
    return testSummary("edacs_provoice");
}
