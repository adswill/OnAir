// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS physical layer through the Rust crate: a burst of every profile through the Watterson channel, 12 kHz audio and a WAV file, back to the same bits;
// +-100 Hz offset and +-2 s clock error; chat, beacon and ack frames over the air; voice superframes over P6 / P7
#include "dect2/kaws/kaws_arq.h"
#include "dect2/kaws/kaws_phy.h"
#include "dect2/kaws/kaws_sched.h"
#include "dect2/kaws/kaws_voice.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
using namespace dect2;
using namespace dect2::kaws;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

struct Rng {
    kris::XorShift64 s;
    explicit Rng(uint64_t seed) : s{seed ? seed : 1} {}
    uint64_t next() { return s.next(); }
    double uni() { return (double)(s.next() >> 11) * (1.0 / 9007199254740992.0); }
    Bits bits(size_t n) { Bits b(n); for (auto& x : b) x = (uint8_t)(next() >> 40) & 1u; return b; }
};

// SNR (dB in 2.4 kHz) with a comfortable margin over the measured thresholds, per profile, for the clean-ish end to end checks
static double easySnr(int p) { return kProfiles[p].estAwgnDb + 9.0; }

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("kaws_phy_test_" + std::to_string((unsigned long)std::rand() % 100000));
    fs::create_directories(dir);
    Rng rng(20261019);

    // every profile: build, add lead-in and tail, channel (AWGN, +-100 Hz, clock error), audio at 12 kHz, WAV file, read back, receive
    for (int p = 0; p < kNumProfiles; p++) {
        const int K = kProfiles[p].K;
        int nb = p == 5 ? 3 : (p == 2 ? 2 : 1);
        std::vector<Bits> blocks;
        for (int b = 0; b < nb; b++) blocks.push_back(rng.bits((size_t)K));
        std::vector<cf> tx = buildBurst(p, blocks);
        CHECK((int)tx.size() == burstSamples(p, nb) && !tx.empty(), "P%d burst %zu samples", p, tx.size());
        double pw = 0;
        for (cf v : tx) pw += std::norm(v);
        pw /= (double)tx.size();
        CHECK(pw > 0.85 && pw < 1.15, "P%d mean power %.3f", p, pw);

        double startSec = 1.5 + (p % 2 ? 1.7 : -1.2);   // within +-2 s of the nominal start; the stream begins at slot start - 1.5 s
        double delay = (kWindowBeforeSec + startSec) * kBaseRate + 0.37;
        double fo = (p % 2 ? 83.0 : -77.0);
        std::vector<cf> rxc = applyChannel(tx, Channel::Awgn, easySnr(p), fo, 0.0, delay, 2400, 1000 + (uint64_t)p);
        CHECK(!rxc.empty(), "channel");
        // 12 kHz audio and a WAV file
        std::vector<float> audio = toAudio12k(rxc);
        CHECK(audio.size() == rxc.size() * 5, "audio length");
        std::string path = (dir / ("p" + std::to_string(p) + ".wav")).string();
        CHECK(writeWav(path, audio), "write wav");
        std::vector<float> back;
        int rate = 0;
        CHECK(readWav(path, back, rate) && rate == kAudioRate && back.size() == audio.size(), "read wav");
        std::vector<cf> iq = fromAudio12k(back);
        RxBurst r = receiveBurst(p, nb, iq, kSearchLo, kSearchHi);
        CHECK(r.sync, "P%d sync (stat %.1f)", p, r.stat);
        if (r.sync) {
            CHECK(std::fabs(clockErrorSec(r.tau) - (startSec - 1.5)) < 0.002, "P%d clock error %.4f vs %.4f", p, clockErrorSec(r.tau), startSec - 1.5);
            CHECK(std::fabs(r.deltaHz - fo) < 1.0, "P%d offset %.2f vs %.2f", p, r.deltaHz, fo);
            CHECK(std::fabs(r.snrDb - easySnr(p)) < 3.0 || easySnr(p) < -26, "P%d snr estimate %.1f vs %.1f", p, r.snrDb, easySnr(p));
        }
        for (int b = 0; b < nb; b++) CHECK(r.sync && r.iters[(size_t)b] >= 0 && r.blocks[(size_t)b] == blocks[(size_t)b], "P%d block %d through audio + WAV (iters %d)", p, b, r.sync ? r.iters[(size_t)b] : -9);
        printf("P%d %-10s burst %.2f s on air, stat %.1f, snr est %.1f dB, offset %.2f Hz\n", p, kProfiles[p].name, onAirSeconds(p, nb), r.stat, r.snrDb, r.deltaHz);
    }

    // no signal: noise only must not be detected, and nothing may crash on a short stream
    {
        std::vector<cf> noise(30000);
        for (auto& v : noise) v = cf((float)(rng.uni() - 0.5), (float)(rng.uni() - 0.5));
        RxBurst r = receiveBurst(3, 1, noise, 0, 14400);
        CHECK(!r.sync, "noise detected as a burst (stat %.1f)", r.stat);
        RxBurst s = receiveBurst(3, 1, std::vector<cf>(100), 0, 14400);
        CHECK(!s.sync, "short stream");
    }

    // chat over P3 on the moderate channel, then the ack back over P3, then a beacon over P0 on the poor channel (SNRs well above the measured thresholds)
    {
        Chat c;
        c.from = "LY1ABC"; c.to = "DL2XYZ"; c.firstMessage = true; c.seq = 1;
        c.text = "HELLO FROM VILNIUS - 73 DE LY1ABC";
        Bits msg = encodeChat(c);
        std::vector<Bits> blocks = splitMessage(kProfiles[3].K, 6, msg);
        CHECK(blocks.size() == 1, "chat fits one P3 block");
        ArqReceiver rx("DL2XYZ");
        std::vector<cf> iq = applyChannel(buildBurst(3, blocks), Channel::Moderate, 3.0, 31.0, 0.0, 3.0 * kBaseRate + 11.3, 2400, 77);
        RxBurst r = receiveBurst(3, 1, iq, kSearchLo, kSearchHi);
        CHECK(r.sync && r.iters[0] >= 0 && rx.onBlock(kProfiles[3].K, r.blocks[0]) && rx.complete(6), "chat block received");
        Chat c2;
        CHECK(decodeChat(rx.message(6), c2) && c2.text == c.text && c2.from == "LY1ABC" && c2.to == "DL2XYZ", "chat text intact");
        Ack a = rx.makeAck(6, r.snrDb);
        std::vector<cf> iq2 = applyChannel(buildBurst(3, {encodeAck(a, kProfiles[3].K)}), Channel::Moderate, 3.0, -20.0, 0.0, 3.0 * kBaseRate + 5.1, 2400, 78);
        RxBurst r2 = receiveBurst(3, 1, iq2, kSearchLo, kSearchHi);
        Ack a2;
        CHECK(r2.sync && decodeAck(r2.blocks[0], a2) && a2.msgId == 6 && (a2.bitmap & 1) && a2.from == "DL2XYZ", "ack received over the air");

        BeaconShort bs;
        bs.call = "LY1ABC"; bs.grid = "KO24RM"; bs.powerStep = 44; bs.fineLat = 12; bs.fineLon = 40; bs.linkQuality = 9;
        Bits blk;
        CHECK(makeBlock(kProfiles[0].K, 0, 0, encodeBeaconShort(bs), blk), "beacon block");
        std::vector<cf> iq3 = applyChannel(buildBurst(0, {blk}), Channel::Poor, -9.0, 12.0, 0.0, 3.0 * kBaseRate + 0.5, 2400, 79);
        RxBurst r3 = receiveBurst(0, 1, iq3, kSearchLo, kSearchHi);
        uint8_t id, ix;
        Bits pl;
        BeaconShort b2;
        CHECK(r3.sync && parseBlock(kProfiles[0].K, r3.blocks[0], id, ix, pl) && decodeBeaconShort(pl, b2) && b2.call == "LY1ABC" && b2.grid == "KO24RM" && b2.fineLon == 40, "short beacon over P0 on KW-POOR at -9 dB");
    }

    // voice: 4 superframes of the test codec over P6 and P7 (AWGN), side channel and codec bits intact
    for (int p : {6, 7}) {
        auto codec = makeTestCodec();
        VoiceTx tx(*codec, p, "LY1ABC", 3);
        std::vector<int16_t> pcm(kVoiceFrameSamples * kVoiceFrames * 2);
        for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(8000 * std::sin(2 * M_PI * (300 + 0.05 * (double)(i % 2000)) * (double)i / 8000.0));
        tx.setSlowData(rng.bits(108));
        std::vector<Bits> blocks = tx.push(pcm.data(), pcm.size());
        CHECK(blocks.size() == 2 && (int)blocks[0].size() == kProfiles[p].K, "voice blocks P%d: %zu", p, blocks.size());
        std::vector<cf> iq = applyChannel(buildBurst(p, blocks), Channel::Awgn, kProfiles[p].estAwgnDb + 8.0, 40.0, 0.0, 3.0 * kBaseRate + 0.7, 2400, 90 + (uint64_t)p);
        RxBurst r = receiveBurst(p, 2, iq, kSearchLo, kSearchHi);
        CHECK(r.sync && r.iters[0] >= 0 && r.iters[1] >= 0 && r.blocks[0] == blocks[0] && r.blocks[1] == blocks[1], "voice blocks P%d over the air", p);
        VoiceRxResult v = voiceReceive(*codec, p, r.blocks[0]);
        CHECK(v.sideOk && v.side.call == "LY1ABC" && v.side.callNumber == 3 && v.pcm.size() == (size_t)kVoiceFrames * kVoiceFrameSamples, "voice side channel P%d", p);
        VoiceRxResult v0 = voiceReceive(*codec, p, blocks[0]);
        CHECK(v.pcm == v0.pcm, "decoded audio identical to the transmitted frames P%d", p);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    printf(fails ? "kaws_phy: %d FAILED\n" : "kaws_phy: all passed\n", fails);
    return fails ? 1 : 0;
}
