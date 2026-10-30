// Multi-VFO: one 2 Msps capture with two FM carriers (1000 and 1700 Hz tones), two pager carriers (POCSAG 512 and 1200, both talking at the same
// time), one APRS carrier and an empty channel with a squelch. Every channel runs at once: both FM stations are in the mixed sound (pan left /
// right, mute and squelch work), both pagers deliver every page with its exact text, the APRS channel decodes, and the channel list can change
// while running without losing the others.
#include "dect2/engine/vfo.h"
#include "dect2/engine/modes.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/pager/pager_rx.h"
#include <cmath>
#include <cstdio>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.0e6;

static double goertzel(const std::vector<float>& x, size_t stride, size_t off, double f, double rate) {
    const double w = 2 * M_PI * f / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0; size_t n = 0;
    for (size_t i = off; i < x.size(); i += stride, n++) { const double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return n ? std::sqrt(p) / (double)n : 0;
}

struct Fm { double hz, tone; double ph = 0, tp = 0; };

int main() {
    const double offP = -pagerTuning().tuneOffsetHz, offK = -packetTuning().tuneOffsetHz;
    Fm fm[2] = {{-300000, 1000}, {+150000, 1700}};
    const double pagHz[2] = {-700000, +500000};
    const int pagSpeed[2] = {kPocsag512, kPocsag1200};
    const double aprsHz = +820000;
    std::unique_ptr<ModeSynth> pg[2], ap;
    double secs = 0;
    for (int k = 0; k < 2; k++) {
        SynthConfig c; c.snrDb = 30; c.cfoHz = pagHz[k] - offP; c.modeOpt[2] = pagSpeed[k] + 1; c.modeOpt[3] = 21 + k;
        pg[k] = makePagerSynth(c, kFs);
        secs = std::max(secs, pagerCycleSeconds(0, pagSpeed[k] + 1) * 2.2);
    }
    { SynthConfig c; c.snrDb = 30; c.cfoHz = aprsHz - offK; ap = makePacketSynth(c, kFs); }

    VfoBank bank;
    std::vector<VfoConfig> ch;
    auto add = [&](int id, double off, VfoMode m, double sq = -150, float pan = 0) { VfoConfig c; c.id = id; c.offsetHz = off; c.mode = m; c.squelchDb = sq; c.pan = pan; ch.push_back(c); };
    add(1, fm[0].hz, VfoMode::Nfm, -50, -1);
    add(2, fm[1].hz, VfoMode::Nfm, -50, +1);
    add(3, -900000, VfoMode::Nfm, -22, 0);         // empty (only the other synths' noise, -31 dBFS): the squelch keeps it closed
    add(4, pagHz[0], VfoMode::Pager);
    add(5, pagHz[1], VfoMode::Pager);
    add(6, aprsHz, VfoMode::Packet);
    bank.configure(kFs);
    bank.setChannels(ch);
    std::vector<float> snd;   // interleaved stereo
    bank.setSink([&](const float* s, size_t n) { snd.insert(snd.end(), s, s + 2 * n); });

    const size_t blk = (size_t)(kFs * 0.05);
    std::vector<cf32> sum(blk), tmp(blk);
    uint32_t rng = 12345;
    const double total = std::max(secs, 12.0);
    bool muted = false;
    std::vector<float> sndMute;
    for (double s = 0; s < total; s += 0.05) {
        std::fill(sum.begin(), sum.end(), cf32(0, 0));
        for (int k = 0; k < 2; k++) { pg[k]->generate(tmp.data(), blk); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * 0.7f; }
        ap->generate(tmp.data(), blk); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * 0.7f;
        for (Fm& f : fm) for (size_t i = 0; i < blk; i++) {
            f.tp += 2 * M_PI * f.tone / kFs; f.ph += 2 * M_PI * (f.hz + 5000 * std::sin(f.tp)) / kFs;
            if (f.ph > M_PI) f.ph -= 2 * M_PI; if (f.tp > M_PI) f.tp -= 2 * M_PI;
            sum[i] += cf32((float)std::cos(f.ph), (float)std::sin(f.ph)) * 0.2f;
        }
        for (size_t i = 0; i < blk; i++) { rng = rng * 1664525u + 1013904223u; sum[i] += cf32(((rng >> 8) & 0xffff) / 65536.f - .5f, ((rng >> 12) & 0xffff) / 65536.f - .5f) * 0.0004f; }
        bank.feed(sum.data(), blk);
        if (!muted && s > 8) {   // mute channel 2 live: the channel list changes while running
            muted = true; sndMute.swap(snd); snd.clear();
            ch[1].mute = true; bank.setChannels(ch);
        }
    }
    printf("sound %zu frames, load %.2f\n", sndMute.size() / 2, bank.load());
    // before the mute: left carries 1000 Hz (channel 1), right 1700 Hz (channel 2), the other tone is far weaker in each
    const double l1 = goertzel(sndMute, 2, 0, 1000, 48000), l2 = goertzel(sndMute, 2, 0, 1700, 48000);
    const double r1 = goertzel(sndMute, 2, 1, 1000, 48000), r2 = goertzel(sndMute, 2, 1, 1700, 48000);
    printf("left 1000 %.4f 1700 %.4f | right 1000 %.4f 1700 %.4f\n", l1, l2, r1, r2);
    CHECK(l1 > 0.01 && r2 > 0.01, "both FM stations must be heard (%.4f, %.4f)", l1, r2);
    CHECK(l1 > 5 * l2 && r2 > 5 * r1, "pan: the stations are not on their sides");
    // after the mute of channel 2 only channel 1 stays
    const double a1 = goertzel(snd, 2, 0, 1000, 48000), b2 = goertzel(snd, 2, 1, 1700, 48000);
    printf("after mute: left 1000 %.4f right 1700 %.4f\n", a1, b2);
    CHECK(a1 > 0.01, "channel 1 vanished after muting channel 2");
    CHECK(b2 < 0.1 * a1, "muted channel 2 still plays");

    auto st = bank.status();
    for (auto& s : st) printf("  ch%d level %.1f open %d items %llu %s\n", s.id, s.levelDb, s.open, (unsigned long long)s.items, s.info.c_str());
    CHECK(st.size() == 6, "status rows %zu", st.size());
    for (auto& s : st) {
        if (s.id == 1 || s.id == 2) CHECK(s.open, "ch%d closed with a station on it", s.id);
        if (s.id == 3) CHECK(!s.open, "empty channel is open (level %.1f)", s.levelDb);
    }
    for (int k = 0; k < 2; k++) {
        PagerTelemetry pt;
        CHECK(bank.pagerTelemetry(4 + k, pt) && pt.messages, "pager channel %d has no results", 4 + k);
        if (!pt.messages) continue;
        const std::vector<PagerTestMessage> want = pagerTestMessages(0, pagSpeed[k] + 1);
        int found = 0;
        for (const PagerTestMessage& w : want) for (const PagerMessage& m : *pt.messages) if (m.address == w.address && m.text == w.text && m.type == w.type) { found++; break; }
        printf("  pager %d: %d of %zu pages (%s)\n", k, found, want.size(), pagerSpeedName(pagSpeed[k]));
        CHECK(found == (int)want.size(), "pager %d: %d of %zu pages", k, found, want.size());
    }
    PacketTelemetry kt;
    CHECK(bank.packetTelemetry(6, kt) && kt.blocksOk >= 3, "APRS channel: %llu frames", (unsigned long long)kt.blocksOk);

    {   // no channels: the bank does nothing and costs nothing; a bad channel (outside the capture) warns, never refuses
        VfoBank b2; b2.configure(kFs);
        VfoConfig c; c.id = 9; c.offsetHz = 5e6; b2.setChannels({c});
        CHECK(b2.channelCount() == 1 && !b2.warning(9).empty(), "an out-of-range channel must be kept with a warning");
        b2.setChannels({});
        b2.feed(sum.data(), blk);
        CHECK(b2.channelCount() == 0, "channels not removed");
    }
    return testSummary("vfo");
}
