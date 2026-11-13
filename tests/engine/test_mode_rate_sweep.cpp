// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Every engine mode started at every common SDR sample rate (0.25 .. 20 Msps) on its own synthetic signal: no crash, no hang,
// no division by zero (the UB sanitizer build catches that), and a rate the mode cannot use must be reported, not crash.
// Each mode runs in a child process (a crash names the mode and rate, an alarm turns a spin into a failure).
// The synthetic signal is generated at the rate asked for; a mode that has no generator at that rate fails to start, which is fine.
// No sound. POSIX only.
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace dect2;

#ifdef _WIN32
int main() { printf("mode rate sweep: skipped on Windows\n"); return 0; }
#else
static const double kRates[] = {0.25e6, 0.5e6, 1e6, 2e6, 2.4e6, 3e6, 4e6, 5e6, 6e6, 8e6, 10e6, 20e6};

static int sweepMode(const ModeTuning& mt) {
    for (double rate : kRates) {
        printf("%s @ %.2f Msps\n", mt.id, rate / 1e6);
        fflush(stdout);
        alarm(40);   // a hang at this rate: SIGALRM kills the child and the parent names the rate
        Engine e;
        DeviceInfo dev;
        TuneSettings t;
        t.centerHz = mt.defMhz * 1e6;
        t.bandwidthMhz = mt.bandwidthMhz;
        t.sampleRate = rate;
        t.synth.mode = mt.stdMode;
        t.synth.snrDb = 25;
        FileOptions fo;
        e.setStandard(mt.stdMode);
        if (e.start(dev, t, fo)) {
            RxTelemetry rx;
            uint64_t seq = 0;
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.35) {
                if (e.latestRx(rx, seq)) seq = rx.seq;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            e.stop();
        }
        alarm(0);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::vector<const ModeTuning*> modes;
    for (int s = 8; s < 200; s++) if (const ModeTuning* m = modeTuning(s)) modes.push_back(m);
    if (argc > 1) {   // one mode only: test_mode_rate_sweep <id>
        for (auto* m : modes) if (std::string(m->id) == argv[1]) return sweepMode(*m);
        return 2;
    }
    int bad = 0;
    for (const ModeTuning* m : modes) {
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) _exit(sweepMode(*m));
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st)) { printf("FAIL: mode %s died with signal %d (SIGALRM = hang), see the last rate above\n", m->id, WTERMSIG(st)); bad++; }
        else if (WEXITSTATUS(st) != 0) { printf("FAIL: mode %s exit %d\n", m->id, WEXITSTATUS(st)); bad++; }
    }
    printf("mode rate sweep: %zu modes, %d failed\n", modes.size(), bad);
    return bad ? 1 : 0;
}
#endif
