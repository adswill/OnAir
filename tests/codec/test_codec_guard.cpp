// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The codec guard (core/include/dect2/codec/codec_guard.h): a voice codec that crashes, hangs, answers nonsense or speaks another plug-in
// interface version is disabled and the program goes on (Discord report 2026-10-23: a plug-in built against mbelib-neo took OnAir down).
// argv[1]: the folder with the fake plug-ins (tests/codec/fake_codec_plugin.c).
#include "dect2/codec/codec_guard.h"
#include "dect2/codec/speech_codec.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace dect2::codec;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static double secsSince(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }

int main(int argc, char** argv) {
    // ---- the guard alone
    int v = 0;
    CHECK(runGuarded("t-ok", [&v] { v = 7; }) == GuardResult::Ok && v == 7, "a normal call returns Ok");
    CHECK(runGuarded("t-crash", [] { volatile int* p = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(16)); *p = 1; }) == GuardResult::Crashed, "a crash is caught");
    CHECK(guardDisabled("t-crash") && !guardMessage("t-crash").empty(), "the crashed codec is disabled with a message");
    CHECK(guardMessage("t-crash").find("disabled") != std::string::npos && guardMessage("t-crash").find("Voice codecs") != std::string::npos, "the message says what to do");
    CHECK(runGuarded("t-crash", [] {}) == GuardResult::Disabled, "a disabled codec is not called again");
    CHECK(!guardTakeNotice().empty(), "one notice for the popup");
    CHECK(guardTakeNotice().empty(), "only once");
    auto t0 = std::chrono::steady_clock::now();
    CHECK(runGuarded("t-hang", [] { std::this_thread::sleep_for(std::chrono::seconds(3)); }, 200) == GuardResult::TimedOut, "a hang is given up");
    CHECK(secsSince(t0) < 1.5, "after the time limit, not when the call returns (%.2f s)", secsSince(t0));
    CHECK(guardDisabled("t-hang"), "the hanging codec is disabled");
    v = 0;
    CHECK(runGuarded("t-ok", [&v] { v = 9; }) == GuardResult::Ok && v == 9, "other codecs still work after a hang (a new worker)");
    guardReset("t-crash");
    CHECK(!guardDisabled("t-crash") && runGuarded("t-crash", [] {}) == GuardResult::Ok, "Look again clears it");
    // a crash and a hang again after the first ones: still caught (the handler is not used up)
    CHECK(runGuarded("t-crash2", [] { volatile int* p = nullptr; *p = 2; }) == GuardResult::Crashed, "a second crash is caught");

    // ---- fake plug-ins through the real loader
    if (argc < 2) { std::printf("no plug-in folder given: plug-in checks skipped\n"); return fails ? 1 : 0; }
    const std::string dir = argv[1];
    std::unique_ptr<SpeechCodec> c;
    CodecLookup r = loadSpeechCodec("fakeok", dir, true, c);
    CHECK(r.status == CodecStatus::Ready && c, "the good plug-in loads (%s)", r.message.c_str());
    if (c) CHECK(c->decode({1, 2, 3}).size() == 160, "and decodes");

    std::unique_ptr<SpeechCodec> cr;
    r = loadSpeechCodec("fakecrash", dir, true, cr);
    CHECK(r.status == CodecStatus::Ready && cr, "the crashing plug-in loads (%s)", r.message.c_str());
    if (cr) {
        CHECK(cr->decode({1, 2, 3}).empty(), "its crash gives silence, not the end of the program");
        CHECK(guardDisabled("fakecrash"), "and disables it");
        CHECK(cr->decode({1, 2, 3}).empty(), "a later frame is silence too");
        cr.reset();   // the destructor leaves a crashed plug-in alone
    }

    std::unique_ptr<SpeechCodec> h;
    r = loadSpeechCodec("fakehang", dir, true, h);
    CHECK(r.status == CodecStatus::Ready && h, "the hanging plug-in loads (%s)", r.message.c_str());
    if (h) {
        t0 = std::chrono::steady_clock::now();
        CHECK(h->decode({1}).empty(), "a hanging decode gives silence");
        CHECK(secsSince(t0) < 3.0, "within the time limit (%.2f s)", secsSince(t0));
        CHECK(guardDisabled("fakehang"), "and disables it");
        h.reset();
    }
    if (c) CHECK(c->decode({4}).size() == 160, "the good plug-in still decodes after the others failed");

    std::unique_ptr<SpeechCodec> g;
    r = loadSpeechCodec("fakegarbage", dir, true, g);
    CHECK(r.status == CodecStatus::Ready && g, "the plug-in with nonsense answers loads (%s)", r.message.c_str());
    if (g) {
        CHECK(g->sampleRate() == 8000, "a sample rate no codec has is not believed (%d)", g->sampleRate());
        CHECK(g->decode({1}).size() <= 16000, "a sample count larger than the buffer is not believed");
    }

    std::unique_ptr<SpeechCodec> a;
    r = loadSpeechCodec("fakeabi", dir, true, a);
    CHECK(r.status == CodecStatus::LoadFailed && !a, "another interface version is refused");
    CHECK(r.message.find("version") != std::string::npos, "with a message that says why (%s)", r.message.c_str());

    std::printf(fails ? "FAILED\n" : "codec_guard ok\n");
    return fails ? 1 : 0;
}
