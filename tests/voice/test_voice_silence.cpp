// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "No audio: why" (voice_silence.h): every reason a voice mode can be silent although calls decode, as pure logic, and the same fed by a
// Monitor with a test clock (a held talkgroup, a squelch, a zero-only codec, a failed output).
#include "dect2/voice/voice_monitor.h"
#include <cstdio>
#include <string>

using namespace dect2::voice;
#include "../common/test_util.h"

static double gT = 0;
static CallInfo tg(uint32_t dst, uint32_t src = 1001, int slot = 1) { CallInfo c; c.dst = dst; c.src = src; c.slot = slot; c.group = true; return c; }

static SilenceIn heard() {   // a receiver that is running and has heard a call that was played a second ago
    SilenceIn in; in.live = true; in.sincePlay = 1;
    return in;
}

static void testPure() {
    CHECK(whySilent(SilenceIn()).why == SilenceWhy::None, "not live: nothing to say");
    SilenceIn in; in.live = true;
    CHECK(whySilent(in).why == SilenceWhy::None, "live but no call heard: nothing to say");
    in = heard();
    CHECK(whySilent(in).why == SilenceWhy::None, "playing normally: no message");
    in.sincePlay = 1e9; in.sinceNotHeld = 2; in.hold = "11";
    Silence s = whySilent(in);
    CHECK(s.why == SilenceWhy::Hold && s.canReleaseHold && s.text.find("hold is set to 11") != std::string::npos && s.text.rfind("No audio:", 0) == 0, "hold: %s", s.text.c_str());
    in.sinceMuted = 1;
    CHECK(whySilent(in).why == SilenceWhy::MuteList, "the most recent blocking verdict wins");
    in.sinceNotHeld = 0.5;
    CHECK(whySilent(in).why == SilenceWhy::Hold, "hold again when it is the newest");
    in = SilenceIn(); in.live = true; in.sinceFiltered = 1;
    CHECK(whySilent(in).why == SilenceWhy::AllowList, "allow list");
    in = SilenceIn(); in.live = true; in.sinceSquelched = 1; in.squelchPct = 5;
    s = whySilent(in);
    CHECK(s.why == SilenceWhy::Squelch && s.text.find("5 %") != std::string::npos, "squelch: %s", s.text.c_str());
    in = SilenceIn(); in.live = true; in.sinceEncrypted = 1;
    CHECK(whySilent(in).why == SilenceWhy::Encrypted, "encrypted");
    in = SilenceIn(); in.live = true; in.sinceNoCodec = 1; in.codecReady = false;
    CHECK(whySilent(in).why == SilenceWhy::NoCodec, "no codec");
    in = SilenceIn(); in.live = true; in.sinceEncrypted = 100;
    CHECK(whySilent(in).why == SilenceWhy::None, "an old verdict is not reported");
    // the sound path while calls are played
    in = heard(); in.outputFailed = true; in.outputError = "\"USB DAC\" could not be opened";
    s = whySilent(in);
    CHECK(s.why == SilenceWhy::OutputError && s.canUseDefaultOutput && s.text.find("USB DAC") != std::string::npos, "output error: %s", s.text.c_str());
    in = heard(); in.codecUnavailable = true;
    CHECK(whySilent(in).why == SilenceWhy::CodecUnavailable, "codec found, no decoder");
    in = heard(); in.silentRun = 9;
    CHECK(whySilent(in).why == SilenceWhy::None, "a few zero bursts are speech pauses");
    in.silentRun = 40;
    s = whySilent(in);
    CHECK(s.why == SilenceWhy::CodecSilent && s.text.find("40") != std::string::npos, "codec returns only zeros: %s", s.text.c_str());
    // volume and mute come first, whatever else is going on
    in = heard(); in.monVolume = 0;
    CHECK(whySilent(in).why == SilenceWhy::ModeVolume && whySilent(in).text.find("volume 0") != std::string::npos, "mode volume 0");
    in = heard(); in.appVolume = 0;
    CHECK(whySilent(in).why == SilenceWhy::AppVolume, "program volume 0");
    in = heard(); in.appMuted = true; in.outputFailed = true;
    CHECK(whySilent(in).why == SilenceWhy::AppMuted, "muted");
    in = heard(); in.live = false; in.appMuted = true;
    CHECK(whySilent(in).why == SilenceWhy::None, "stopped receiver: nothing to say");
}

static Monitor& fresh(const char* name) {
    Monitor& m = monitor(name);
    m.setClock([] { return gT; }, [] { return (int64_t)1790000000; });
    m.setSettings(Settings());
    m.clearHistory();
    return m;
}

static void testMonitor() {
    Monitor& m = fresh("t-silence");
    gT = 1000;
    Settings s; s.hold = "11";
    m.setSettings(s);
    CHECK(m.offer(tg(2620)) == Verdict::NotHeld, "TG 2620 is not played while TG 11 is held");
    gT += 0.5;
    Silence sl = whySilent(m.silenceInput(true, true, 1.f, false));
    CHECK(sl.why == SilenceWhy::Hold && sl.canReleaseHold, "the hold is named: %s", sl.text.c_str());
    gT += 30;
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::None, "30 s later nothing is heard: no message");
    m.releaseHold();
    CHECK(m.offer(tg(2620)) == Verdict::Play, "released: plays");
    gT += 0.5;
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::None, "playing: no message");
    // the sound path
    m.noteOutput(false, "USB DAC");
    sl = whySilent(m.silenceInput(true, true, 1.f, false));
    CHECK(sl.why == SilenceWhy::OutputError && sl.text.find("USB DAC") != std::string::npos, "output failed: %s", sl.text.c_str());
    m.noteOutput(true, "USB DAC");
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::None, "output back");
    for (int i = 0; i < 12; i++) m.noteSound(Monitor::Sound::DecodedSilent);
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::CodecSilent, "twelve zero bursts");
    m.noteSound(Monitor::Sound::Decoded);
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::None, "sound again");
    m.noteSound(Monitor::Sound::CodecUnavailable);
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::CodecUnavailable, "no decoder");
    gT += 10;
    m.offer(tg(2620));
    CHECK(whySilent(m.silenceInput(true, true, 0.f, false)).why == SilenceWhy::AppVolume, "program volume 0");
    s = m.settings(); s.volume = 0; m.setSettings(s);
    CHECK(whySilent(m.silenceInput(true, true, 1.f, false)).why == SilenceWhy::ModeVolume, "mode volume 0 from the settings");
    // squelch
    Monitor& q = fresh("t-silence2");
    gT += 100;
    s = Settings(); s.squelchPct = 3; q.setSettings(s);
    CallInfo c = tg(5); c.errorRate = 0.2f;
    CHECK(q.offer(c) == Verdict::Squelched, "squelched");
    CHECK(whySilent(q.silenceInput(true, true, 1.f, false)).why == SilenceWhy::Squelch, "squelch is named");
}

int main() {
    testPure();
    testMonitor();
    return testSummary("voice_silence");
}
