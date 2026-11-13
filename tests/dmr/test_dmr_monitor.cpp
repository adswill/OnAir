// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR voice through the stub codec plug-in (tests/codec/stub_codec_plugin.c, no codec) into the voice monitor (voice_monitor.h): the loop
// generator -> DmrReceiver -> monitor -> plug-in -> recording. Busy two-slot traffic on three talkgroups, the monitor clock follows the air.
//  1. defaults: one call is played at a time, the calls are listed with their talkgroup, source and colour code, each played call is
//     recorded to a WAV (8 kHz, the stub's samples) with a JSON file naming its talkgroup;
//  2. every talkgroup heard on the mute list: nothing reaches the plug-in;
//  3. hold on one talkgroup: only its calls are played;
//  4. a priority talkgroup: its calls are played whole once their talkgroup is known;
//  5. encrypted traffic: listed as encrypted, never played or recorded.
// NOT covered: a real codec, a real recording, the output device.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/voice/voice_monitor.h"
#include "../codec/plugin_test_util.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace dect2;
#include "../common/test_util.h"

namespace fs = std::filesystem;

static double gAir = 0;   // seconds of signal fed

struct Run { uint64_t decoded = 0; int frames = 0; std::vector<voice::CallEntry> hist; std::vector<fs::path> wavs; };

static Run run(const std::string& name, voice::Settings s, bool encrypted, int blocks = 300) {
    voice::Monitor& m = voice::monitor(name);
    m.setClock([] { return gAir; });
    const fs::path dir = fs::temp_directory_path() / ("onair-test-dmr-monitor-" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    s.recordDir = dir.string();
    m.setSettings(s);
    m.clearHistory();
    plugtest::gFrames = 0;
    DmrReceiver rx;
    rx.setSilent(true);
    rx.setVoiceMonitor(&m);
    rx.configure(2.4e6);
    DmrGenConfig c;
    c.snrDb = 30; c.traffic = 2; c.encrypted = encrypted; c.seed = 7;
    DmrSignal sig(c);
    std::vector<cf32> g(65536);
    for (int b = 0; b < blocks; b++) { sig.generate(g.data(), g.size()); rx.feed(g.data(), g.size()); gAir += g.size() / 2.4e6; }
    gAir += 100;   // every call is over
    m.housekeep();
    Run o;
    DmrTelemetry t;
    rx.telemetry(t, 0);
    o.decoded = t.voiceDecoded;
    o.frames = plugtest::gFrames;
    o.hist = m.history();
    for (auto& p : fs::recursive_directory_iterator(dir, ec)) if (p.path().extension() == ".wav") o.wavs.push_back(p.path());
    return o;
}

static std::string readAll(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    CHECK(plugtest::install("ambe2", STUB_PLUGIN_PATH), "stub plug-in not installed");

    // 1. defaults, recording on
    voice::Settings rec; rec.record = true;
    const Run a = run("dmr-loop-a", rec, false);
    std::set<uint32_t> tgs, targets;   // talkgroups; every target (private calls too)
    int played = 0, withFile = 0;
    for (const auto& e : a.hist) {
        if (e.info.dst && e.info.group) tgs.insert(e.info.dst);
        if (e.info.dst) targets.insert(e.info.dst);
        if (e.played) played++;
        if (!e.file.empty()) withFile++;
    }
    printf("defaults: %zu calls (%d played, %d recorded), %zu talkgroups, %llu bursts decoded, %d plug-in frames, %zu WAV files\n", a.hist.size(), played, withFile,
           tgs.size(), (unsigned long long)a.decoded, a.frames, a.wavs.size());
    CHECK(a.decoded > 20 && a.frames >= (int)a.decoded * 3, "voice decoded %llu, frames %d", (unsigned long long)a.decoded, a.frames);
    CHECK(tgs.size() >= 2, "talkgroups listed: %zu", tgs.size());
    CHECK(played >= 1 && withFile == played && (int)a.wavs.size() == withFile, "every played call recorded once: %d %d %zu", played, withFile, a.wavs.size());
    bool cc = false;
    for (const auto& e : a.hist) cc = cc || e.info.access == "CC 1";
    CHECK(cc, "colour code shown");
    size_t samples = 0;
    for (const auto& w : a.wavs) {
        int rate = 0, ch = 0; std::vector<int16_t> pcm;
        CHECK(voice::readWav(w.string(), rate, ch, pcm) && rate == 8000 && ch == 1, "WAV %s", w.string().c_str());
        samples += pcm.size();
        // the stub's frames: sample i of a frame is i * 37 + 1 (the first carries the code byte)
        bool pattern = false;
        for (size_t i = 0; i + 2 < pcm.size(); i++) if (pcm[i] >= 1000 && std::abs(pcm[i + 2] - (int)std::lrint((2 * 37 + 1) / 32768.0 * 32767)) <= 1) { pattern = true; break; }
        CHECK(pattern, "the stub's samples are in %s", w.filename().string().c_str());
        fs::path j = w; j.replace_extension(".json");
        const std::string js = readAll(j);
        CHECK(js.find("\"mode\": \"dmr-loop-a\"") != std::string::npos && js.find("\"target\": \"-\"") == std::string::npos && js.find("\"target_id\": 0,") == std::string::npos && js.find("\"access\": \"CC 1\"") != std::string::npos,
              "JSON of %s:\n%s", w.filename().string().c_str(), js.c_str());
    }
    CHECK(samples == (size_t)a.frames * 160, "the recordings hold every decoded frame: %zu samples for %d frames", samples, a.frames);

    // 2. every target muted
    voice::Settings mute;
    for (uint32_t t : targets) mute.mute += std::to_string(t) + ",";
    const Run b = run("dmr-loop-b", mute, false);
    int mutedSeen = 0;
    for (const auto& e : b.hist) if (e.verdict == voice::Verdict::Muted) mutedSeen++;
    printf("muted: %llu decoded, %d plug-in frames, %d muted calls\n", (unsigned long long)b.decoded, b.frames, mutedSeen);
    // a burst before the talkgroup is known (late entry) may play; no call with a known muted talkgroup does
    for (const auto& e : b.hist) if (e.info.dst && targets.count(e.info.dst)) CHECK(e.verdict == voice::Verdict::Muted, "TG %u not muted", e.info.dst);
    CHECK(mutedSeen >= 1 && b.decoded * 10 < a.decoded, "muting stopped the voice: %llu of %llu", (unsigned long long)b.decoded, (unsigned long long)a.decoded);

    // 3. hold on the first talkgroup
    const uint32_t held = *tgs.begin();
    voice::Settings hold; hold.hold = std::to_string(held);
    const Run h = run("dmr-loop-h", hold, false);
    int heldPlayed = 0;
    for (const auto& e : h.hist) {
        if (e.info.dst && e.info.dst != held) CHECK(e.played == 0 || e.played <= 3, "TG %u played %llu frames while held on %u", e.info.dst, (unsigned long long)e.played, held);
        if (e.info.dst == held && e.played) heldPlayed++;
    }
    printf("hold on TG %u: %d of its calls played, %llu decoded\n", held, heldPlayed, (unsigned long long)h.decoded);
    CHECK(heldPlayed >= 1, "the held talkgroup was played");

    // 4. priority on the last talkgroup
    const uint32_t pri = *tgs.rbegin();
    voice::Settings pr; pr.priority = std::to_string(pri);
    const Run p = run("dmr-loop-p", pr, false);
    for (const auto& e : p.hist) if (e.info.dst == pri && e.frames > 10) {
        CHECK(e.priority && e.played * 10 >= e.frames * 8, "priority TG %u played %llu of %llu frames", pri, (unsigned long long)e.played, (unsigned long long)e.frames);
    }

    // 5. encrypted
    const Run e = run("dmr-loop-e", rec, true);
    int enc = 0;
    for (const auto& c : e.hist) { if (c.verdict == voice::Verdict::Encrypted) enc++; CHECK(c.played == 0 && c.file.empty(), "an encrypted call was played or recorded"); }
    printf("encrypted: %d calls listed as encrypted, %llu decoded, %zu WAV files\n", enc, (unsigned long long)e.decoded, e.wavs.size());
    CHECK(enc >= 1 && e.decoded == 0 && e.frames == 0 && e.wavs.empty(), "encrypted voice reached the codec or a file");

    return testSummary("dmr_monitor");
}
