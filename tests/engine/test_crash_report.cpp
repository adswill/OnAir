// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Crash report tests: the log rotation (everywhere) and, where there is fork(), a child that crashes and leaves a report.
#include "dect2/engine/crash_report.h"
#include "dect2/engine/debug_dump.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace dect2;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void spit(const fs::path& p, const std::string& t) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << t; }

int main() {
    const fs::path dir = fs::temp_directory_path() / ("onair_crash_test_" + std::to_string((long long)std::rand()) + "_" + std::to_string((long long)fs::file_time_type::clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    // log rotation: the old log is kept once, a newer one replaces it
    crash::rotateLog(dir.string());   // no log yet: nothing happens
    CHECK(!fs::exists(dir / "onair-prev.log"), "no log, no previous log");
    spit(dir / "onair.log", "first run");
    crash::rotateLog(dir.string());
    CHECK(!fs::exists(dir / "onair.log"), "the log moved away");
    CHECK(slurp(dir / "onair-prev.log") == "first run", "the previous log has the old text");
    spit(dir / "onair.log", "second run");
    crash::rotateLog(dir.string());
    CHECK(slurp(dir / "onair-prev.log") == "second run", "only one old log is kept");

#ifndef _WIN32
    // a fresh start reports no crash
    crash::install("crash", dir.string().c_str());
    std::string path;
    CHECK(!crash::lastRunCrashed(path), "no crash before anything ran");

    fflush(stdout); fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        crash::install("crash", dir.string().c_str());
        volatile int* p = nullptr;
        *p = 1;   // the crash
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the child died of SIGSEGV (status %d)", status);
    CHECK(fs::exists(dir / "crash.txt"), "crash.txt exists");
    const std::string rep = slurp(dir / "crash.txt");
    CHECK(rep.find("crashed") != std::string::npos, "the report says crashed: %s", rep.c_str());
    CHECK(rep.find("SIGSEGV") != std::string::npos, "the report names the signal: %s", rep.c_str());
    CHECK(rep.find("stack:") != std::string::npos, "the report has a stack");
    if (dump::supported()) {   // Linux x86-64: the dump of all threads next to the report
        const std::string dmp = slurp(dir / "crash.dmp");
        CHECK(dmp.size() > 32 && dmp.compare(0, 4, "MDMP") == 0, "crash.dmp is a minidump (%zu bytes)", dmp.size());
    }

    // the next start finds the report, the one after it does not
    crash::install("crash", dir.string().c_str());
    CHECK(crash::lastRunCrashed(path) && path == (dir / "crash.txt").string(), "the next start sees the crash (%s)", path.c_str());
    if (dump::supported())
        CHECK(crash::lastCrashDump() == (dir / "crash.dmp").string(), "the next start finds the dump (%s)", crash::lastCrashDump().c_str());
    crash::install("crash", dir.string().c_str());
    CHECK(!crash::lastRunCrashed(path), "the start after that sees none");
#endif

    {   // startup guard: starts that never reached startupDone() are counted, a finished start clears the count
        const fs::path g = dir / "guard";
        fs::create_directories(g);
        crash::install("boot", g.string().c_str());
        CHECK(crash::failedStarts() == 0, "a first start has no failed starts (%d)", crash::failedStarts());
        crash::install("boot", g.string().c_str());   // the run before did not finish starting
        CHECK(crash::failedStarts() == 1, "one unfinished start (%d)", crash::failedStarts());
        crash::install("boot", g.string().c_str());
        CHECK(crash::failedStarts() == 2, "two unfinished starts in a row (%d)", crash::failedStarts());
        crash::startupDone();
        CHECK(crash::failedStarts() == 0 && !fs::exists(g / "boot.booting"), "a finished start clears the count and the mark");
        crash::install("boot", g.string().c_str());
        CHECK(crash::failedStarts() == 0, "the start after a finished one is clean (%d)", crash::failedStarts());
        crash::startupDone();
        spit(g / "boot.booting", "garbage");
        crash::install("boot", g.string().c_str());
        CHECK(crash::failedStarts() == 1, "an unreadable mark still counts as one unfinished start (%d)", crash::failedStarts());
        crash::startupDone();
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("crash report: ok\n");
    return 0;
}
