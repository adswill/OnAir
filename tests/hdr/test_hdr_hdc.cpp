// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio sound through a user-installed HDC-patched FAAD2 (hdr_hdc.h), tested with fake libraries (tests/hdr/fake_faad_hdc.c): no codec, no sound
// device. The fake "decodes" a packet to 2048 stereo frames: L = first byte * 100, R = packet size * 10, and L + 7 for the first packet of a decoder state.
//  1  the loader: no file, an unknown version, a plain FAAD2, a wrong frame info layout (all refused with a reason), a good one
//  2  one decoder state: PCM for known packets, errors, restart
//  3  HdcAudio: packets to 48 kHz sound, program switching, lost and bad packets (silence and a restarted decoder), repeated errors, split packets,
//     no library, a library that disappears
//  4  the same through the Layer 2 decoder (PDU headers, sequence numbers, latency)
//  5  threads: controls, rescans, resets and the feed side at the same time
#include "dect2/hdr/hdr_fec.h"
#include "dect2/hdr/hdr_hdc.h"
#include "dect2/hdr/hdr_l2.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::hdr;
#include "../common/test_util.h"

static void setEnv(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

constexpr double kStep = kHdcFrameSamples / kHdcRate;   // one packet in seconds of signal
constexpr double kOutPerPacket = kHdcFrameSamples * 48000.0 / kHdcRate;   // output frames per packet (2229.3)

static std::vector<uint8_t> packet(int first, int size) { std::vector<uint8_t> p((size_t)size, 0x55); p[0] = (uint8_t)first; return p; }

// Collects what HdcAudio sends to the device
struct Sink {
    std::vector<float> lr;
    void attach(HdcAudio& a) { a.setTap([this](const float* x, size_t n) { lr.insert(lr.end(), x, x + 2 * n); }); }
    // mean of one channel over the middle half of packet n's stretch of the output (counted from `from` frames)
    double mid(size_t from, int n, int ch) const {
        const size_t a = from + (size_t)((n + 0.25) * kOutPerPacket), b = from + (size_t)((n + 0.75) * kOutPerPacket);
        if (b * 2 + 1 >= lr.size()) return NAN;
        double s = 0;
        for (size_t i = a; i < b; i++) s += lr[2 * i + (size_t)ch];
        return s / (double)(b - a) * 32768.0;
    }
};
static bool nearBy(double v, double want) { return std::fabs(v - want) < 3 + 0.01 * std::fabs(want); }

static void pushRun(HdcAudio& a, int prog, unsigned seq0, int count, int fill0, int size = 160, bool fillStep = true) {
    for (int i = 0; i < count; i++) {
        const std::vector<uint8_t> p = packet(fill0 + (fillStep ? i : 0), size);
        a.push(prog, 0, (seq0 + (unsigned)i) % 64, HdcAudio::Full, true, p.data(), p.size());
    }
}

int main() {
    // ---------------------------------------------------------------- 1 the loader
    std::string path, prob;
    setEnv("DECT2_FAAD_HDC_PATH", "/nonexistent-folder/libfaad_hdc.so");
    CHECK(!hdcAvailable(&path, &prob) && prob.empty(), "no file: available %d problem '%s'", (int)hdcAvailable(), prob.c_str());
    CHECK(!hdcFileNames().empty() && !hdcCandidatePaths().empty(), "names and paths listed");
    {   // no library: the audio path takes packets, plays nothing, does not fall over
        HdcAudio a; Sink s; s.attach(a); a.setSilent(true);
        a.advanceTo(0);
        pushRun(a, 0, 0, 8, 10);
        a.align(0, 0, 0);
        pushRun(a, 0, 0, 8, 10);
        a.advanceTo(8 * kStep + 0.01);
        CHECK(s.lr.empty(), "sound without a library");
        const HdcAudio::Stats st = a.stats();
        CHECK(!st.libFound && st.decoded == 0, "stats without a library");
        HdcDecoder d; CHECK(!d.ok(), "decoder without a library");
        std::vector<int16_t> pcm; const std::vector<uint8_t> p = packet(1, 100);
        CHECK(d.decode(p.data(), p.size(), pcm) == -1, "decode without a library");
    }
    struct { const char* env; const char* want; } bad[] = {{FAKE_HDC_BADVER, "not one OnAir knows"}, {FAKE_HDC_NOINIT, "NeAACDecInitHDC"}, {FAKE_HDC_BADLAYOUT, "layout"}};
    for (const auto& b : bad) {
        setEnv("DECT2_FAAD_HDC_PATH", b.env);
        CHECK(!hdcRescan(), "refused library accepted: %s", b.env);
        CHECK(!hdcAvailable(&path, &prob) && prob.find(b.want) != std::string::npos && path == b.env, "problem for %s: '%s'", b.env, prob.c_str());
    }
    // a good one, from a copy that is deleted again after loading (an uninstalled or moved library must not take the program down)
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("onair_hdc_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
    const fs::path copy = tmp / fs::path(FAKE_HDC_GOOD).filename();
    std::error_code ec;
    fs::copy_file(FAKE_HDC_GOOD, copy, ec);
    setEnv("DECT2_FAAD_HDC_PATH", ec ? std::string(FAKE_HDC_GOOD) : copy.string());
    CHECK(hdcRescan() && hdcAvailable(&path, &prob) && prob.empty() && !path.empty(), "good library: '%s'", prob.c_str());
    bool removed = false;
#ifndef _WIN32
    removed = !ec && fs::remove(copy, ec);   // a loaded DLL cannot be deleted on Windows
#endif
    CHECK(hdcAvailable() && hdcRescan(), "found library lost its answer after the file went (removed %d)", (int)removed);

    // ---------------------------------------------------------------- 2 one decoder state
    {
        HdcDecoder d;
        CHECK(d.ok(), "decoder with the library");
        std::vector<int16_t> pcm;
        const std::vector<uint8_t> p = packet(5, 160);
        CHECK(d.decode(p.data(), p.size(), pcm) == 2048 && pcm.size() == 4096 && pcm[0] == 507 && pcm[1] == 1600 && pcm[4095] == 1600, "first packet: L %d R %d", pcm.empty() ? 0 : pcm[0], pcm.empty() ? 0 : pcm[1]);
        CHECK(d.decode(p.data(), p.size(), pcm) == 2048 && pcm[0] == 500, "second packet L %d", pcm.empty() ? 0 : pcm[0]);
        d.reset();
        CHECK(d.decode(p.data(), p.size(), pcm) == 2048 && pcm[0] == 507, "after reset a new state (L %d)", pcm.empty() ? 0 : pcm[0]);
        const std::vector<uint8_t> e = packet(0xEE, 160);
        CHECK(d.decode(e.data(), e.size(), pcm) == -1 && pcm.empty(), "error packet");
        CHECK(d.decode(p.data(), 0, pcm) == -1, "empty packet");
        CHECK(d.decode(p.data(), p.size(), pcm) == 2048 && pcm[0] == 500, "decodes on after an error");
    }

    // ---------------------------------------------------------------- 3 packets to sound
    {
        HdcAudio a; Sink s; s.attach(a); a.setSilent(true);
        a.advanceTo(0);
        a.align(0, 0, 0); a.align(1, 0, 0);
        pushRun(a, 0, 0, 8, 10);       // HD1: L 1000 .. 1700
        pushRun(a, 1, 0, 8, 50, 80);   // HD2: L 5000 .. 5700, R 800
        a.setProgram(0);
        a.advanceTo(8 * kStep + 0.001);
        CHECK(std::fabs((double)s.lr.size() / 2 - 8 * kOutPerPacket) < 400, "8 packets gave %zu frames at 48 kHz (want %.0f)", s.lr.size() / 2, 8 * kOutPerPacket);
        for (int i = 0; i < 8; i++) {
            CHECK(nearBy(s.mid(0, i, 0), (10 + i) * 100 + (i == 0 ? 7 : 0)) && nearBy(s.mid(0, i, 1), 1600), "HD1 packet %d: L %.1f R %.1f", i, s.mid(0, i, 0), s.mid(0, i, 1));
        }
        HdcAudio::Stats st = a.stats();
        CHECK(st.libFound && st.program == 0 && st.decoded == 8 && st.errors == 0 && st.lost == 0, "stats: decoded %llu errors %llu lost %llu", (unsigned long long)st.decoded, (unsigned long long)st.errors, (unsigned long long)st.lost);

        // switching to HD2: its packets from where the clock is, a fresh decoder, nothing of HD1 left in the queue
        pushRun(a, 0, 8, 8, 18);
        pushRun(a, 1, 8, 8, 58, 80);
        a.setProgram(1);
        const size_t from = s.lr.size() / 2;
        a.advanceTo(16 * kStep + 0.001);
        for (int i = 0; i < 8; i++)
            CHECK(nearBy(s.mid(from, i, 0), (58 + i) * 100 + (i == 0 ? 7 : 0)) && nearBy(s.mid(from, i, 1), 800), "HD2 packet %d: L %.1f R %.1f", i, s.mid(from, i, 0), s.mid(from, i, 1));
        CHECK(a.stats().program == 1, "program reported");

        // a lost packet (seq 18 never comes) and a packet with a bad CRC (seq 20): silence for each, and the decoder starts over after it
        pushRun(a, 1, 16, 2, 70, 80);
        { const std::vector<uint8_t> p = packet(72, 80); a.push(1, 0, 19, HdcAudio::Full, true, p.data(), p.size()); }
        { const std::vector<uint8_t> p = packet(73, 80); a.push(1, 0, 20, HdcAudio::Full, false, p.data(), p.size()); }
        pushRun(a, 1, 21, 3, 74, 80);
        const size_t from2 = s.lr.size() / 2;
        a.advanceTo(24 * kStep + 0.001);
        const double want[8] = {7000 + 7 * 0, 7100, 0, 7200 + 7, 0, 7400 + 7, 7500, 7600};   // packets 16..23: after the silence a fresh state (+7)
        for (int i = 0; i < 8; i++) CHECK(nearBy(s.mid(from2, i, 0), want[i] + (i == 0 ? 0 : 0)), "gap test packet %d: L %.1f want %.1f", i, s.mid(from2, i, 0), want[i]);
        st = a.stats();
        CHECK(st.lost == 2, "lost slots %llu", (unsigned long long)st.lost);

        // three decode errors in a row: silence, then the decoder state is thrown away (one restart); a good packet after it is a fresh state
        for (int i = 0; i < 3; i++) { const std::vector<uint8_t> p = packet(0xEE, 80); a.push(1, 0, (unsigned)(24 + i), HdcAudio::Full, true, p.data(), p.size()); }
        pushRun(a, 1, 27, 2, 30, 80);
        const size_t from3 = s.lr.size() / 2;
        const uint64_t errs0 = a.stats().errors, rs0 = a.stats().resets;
        a.advanceTo(29 * kStep + 0.001);
        CHECK(a.stats().errors - errs0 == 3 && a.stats().resets - rs0 == 1, "errors %llu resets %llu", (unsigned long long)(a.stats().errors - errs0), (unsigned long long)(a.stats().resets - rs0));
        CHECK(nearBy(s.mid(from3, 0, 0), 0) && nearBy(s.mid(from3, 1, 0), 0) && nearBy(s.mid(from3, 2, 0), 0) && nearBy(s.mid(from3, 3, 0), 3000 + 7) && nearBy(s.mid(from3, 4, 0), 3100), "error run: %.1f %.1f %.1f %.1f %.1f",
              s.mid(from3, 0, 0), s.mid(from3, 1, 0), s.mid(from3, 2, 0), s.mid(from3, 3, 0), s.mid(from3, 4, 0));

        // a packet that crosses two PDUs: the halves are joined (100 + 60 bytes, the first byte is in the front half)
        { const std::vector<uint8_t> f = packet(44, 100), b(60, 0x66); a.push(1, 0, 29, HdcAudio::HalfFront, true, f.data(), f.size()); a.push(1, 0, 29, HdcAudio::HalfBack, true, b.data(), b.size()); }
        { const std::vector<uint8_t> f = packet(45, 100), b(60, 0x66); a.push(1, 0, 30, HdcAudio::HalfFront, true, f.data(), f.size()); a.push(1, 0, 30, HdcAudio::HalfBack, false, b.data(), b.size()); }   // a bad back half
        const size_t from4 = s.lr.size() / 2;
        a.advanceTo(31 * kStep + 0.001);
        CHECK(nearBy(s.mid(from4, 0, 0), 4400) && nearBy(s.mid(from4, 0, 1), 1600), "joined packet: L %.1f R %.1f", s.mid(from4, 0, 0), s.mid(from4, 0, 1));
        CHECK(nearBy(s.mid(from4, 1, 0), 0), "half packet with a bad half: L %.1f", s.mid(from4, 1, 0));

        // a program that stops: no endless silence queued for it
        const size_t before = s.lr.size();
        a.advanceTo(31 * kStep + 5.0);
        a.advanceTo(31 * kStep + 9.0);
        CHECK(s.lr.size() - before < (size_t)(2 * 4 * kOutPerPacket), "silence made for a program that stopped: %zu frames", (s.lr.size() - before) / 2);

        // reset: a new signal
        a.reset();
        CHECK(a.stats().decoded == 0 && a.stats().lost == 0, "reset clears the counters");
        s.lr.clear();
        a.advanceTo(100.0);
        a.align(1, 0, 0);
        pushRun(a, 1, 0, 2, 20, 80);
        a.advanceTo(100.0 + 2 * kStep + 0.001);
        CHECK(nearBy(s.mid(0, 0, 0), 2000 + 7) && nearBy(s.mid(0, 1, 0), 2100), "after reset: %.1f %.1f", s.mid(0, 0, 0), s.mid(0, 1, 0));
    }

    // ---------------------------------------------------------------- 4 through the Layer 2 decoder
    for (int latency : {0, 2}) {
        HdcAudio a; Sink s; s.attach(a); a.setSilent(true);
        L2Decoder dec;
        dec.setAudio(&a);
        a.setProgram(0);
        a.advanceTo(0);
        const int frames = 6;
        std::vector<uint8_t> psd0, psd1;
        for (int k = 0; k < frames; k++) {
            AudioPduSpec x;
            x.codecMode = 0; x.program = 0; x.progType = 5; x.pduSeq = k % 8; x.seq = (k * 32) % 64; x.latency = latency; x.fill = (uint8_t)(10 + k);
            x.packetBytes.assign(32, 100); x.psdRoom = 256 - audioPduHeaderBytes(x);
            AudioPduSpec y = x;
            y.program = 1; y.fill = (uint8_t)(100 + k); y.packetBytes.assign(32, 40); y.psdRoom = 256 - audioPduHeaderBytes(y);
            std::vector<uint8_t> pdu = audioPdu(x, psd0);
            const std::vector<uint8_t> p2 = audioPdu(y, psd1);
            pdu.insert(pdu.end(), p2.begin(), p2.end());
            pdu.resize((size_t)pduBytes(kP1LenFm), 0);
            std::vector<uint8_t> bits((size_t)kP1LenFm);
            packTransfer(pdu, kPciAudio, kP1LenFm, bits.data());
            dec.pushTransfer(bits.data(), kP1LenFm, 0);
            if (k == 3) a.setProgram(1);   // the selection on the screen changes to HD2 half way
            a.advanceTo((k + 1) * 32 * kStep + 0.001);
        }
        // The play position lags the packets by 2 * latency slots; slots with nothing in them before the first packet make no sound (nothing is
        // queued before there is something to play), so output packet n is stream packet n for both latencies. HD1 plays packets 0..95 (frames 0..2),
        // HD2 from the switch on (frame 3 on); look at packets well inside each part.
        CHECK(s.lr.size() / 2 > (size_t)(frames * 32 * kOutPerPacket * 0.95), "latency %d: %zu output frames", latency, s.lr.size() / 2);
        for (int n : {0, 10, 50, 80}) {
            const double v = s.mid(0, n, 0);
            CHECK(nearBy(v, (10 + n / 32) * 100 + (n == 0 ? 7 : 0)) && nearBy(s.mid(0, n, 1), 1000), "latency %d: HD1 packet %d level %.1f / %.1f", latency, n, v, s.mid(0, n, 1));
        }
        for (int n : {120, 150, 180}) {
            const double v = s.mid(0, n, 0);
            CHECK(nearBy(v, (100 + n / 32) * 100) && nearBy(s.mid(0, n, 1), 400), "latency %d: HD2 packet %d level %.1f / %.1f", latency, n, v, s.mid(0, n, 1));
        }
        const HdcAudio::Stats st = a.stats();
        CHECK(st.errors == 0 && st.lost == 0 && st.decoded >= 186, "latency %d: decoded %llu errors %llu lost %llu", latency, (unsigned long long)st.decoded, (unsigned long long)st.errors, (unsigned long long)st.lost);
        CHECK(dec.pciOk() == (uint64_t)frames, "latency %d: %llu transfer frames", latency, (unsigned long long)dec.pciOk());
    }

    // ---------------------------------------------------------------- 5 threads
    {
        HdcAudio a; a.setSilent(true);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> taps{0};
        a.setTap([&](const float*, size_t n) { taps += n; });
        std::thread feed([&] {
            double t = 0;
            unsigned seq = 0;
            while (!stop) {
                for (int p = 0; p < 4; p++) a.align(p, 0, seq % 64);
                for (int i = 0; i < 8; i++) for (int p = 0; p < 4; p++) pushRun(a, p, seq + (unsigned)i, 1, 20 + p, 100 + p, false);
                seq += 8;
                t += 8 * kStep;
                a.advanceTo(t);
            }
        });
        std::thread ctl([&] {
            int n = 0;
            while (!stop) {
                a.setProgram(n % 8); a.setMuted(n & 1); a.setVolume((float)(n % 5) / 4); a.setSilent(true);
                (void)a.stats();
                if (n % 7 == 0) a.reset();
                if (n % 11 == 0) a.setTap([&](const float*, size_t k) { taps += k; });
                n++;
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        std::thread scan([&] {
            while (!stop) { (void)hdcRescan(); (void)hdcAvailable(); HdcDecoder d; std::vector<int16_t> pcm; const std::vector<uint8_t> p = packet(1, 50); (void)d.decode(p.data(), p.size(), pcm); }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        stop = true;
        feed.join(); ctl.join(); scan.join();
        CHECK(taps.load() > 0, "threads: no sound made");
    }
    if (!tmp.empty()) fs::remove_all(tmp, ec);
    return testSummary("hdr_hdc");
}
