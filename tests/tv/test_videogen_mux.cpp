// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Part of test_videogen (ctest -R "^videogen$"): the muxing of the Digital video generator program (#458).
//  1. TsRemux with set numbering (the Advanced Muxing tab): the programme number, PMT PID and stream PIDs given per source
//  2. TsRemux with kept numbering (several generators' whole multiplexes in the transmitter): multi-programme inputs keep their
//     programme numbers, PIDs and names; what collides with an earlier input is moved
//  3. the session's threads (C.6 of #458): sources start and stop in random order while the mux runs and sends over the link into
//     OnAir's hub; a stalled source does not stop the mux (the others keep their rate, its slots are padded); the preview on and off
//     and switched while streaming; the threads carry their names; everything is joined at the end (no thread left)
//  4. the session writing a file: two sources as two programmes (read back by FFmpeg), nothing dropped, the mux ends with the sources
//  5. the mux rate: the channel with padding, else the sources plus a margin
#include "dect2/tv/ts_proto.h"
#include "dect2/tv/ts_remux.h"
#include "dect2/tv/vglink.h"
#include "vg_test_pipe.h"
#include "dect2/tv/videogen_session.h"
#include "dect2/tx/tx.h"
#include "dect2/dvbt/dvbt_rx.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <random>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
}
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fstream>
#endif
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int gFail = 0;
void fail(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::printf("FAIL: %s\n", b);
    gFail++;
}
#define MCHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

// A multiplex of several programmes, each a PMT and some streams (the first carries the PCR), with a PAT and an SDT naming them
struct FakeProg { int sid, pmt; std::vector<int> es; std::string name; };
struct FakeMux {
    std::vector<FakeProg> progs;
    uint64_t n = 0;
    std::map<int, int> cc;
    void section(uint8_t* pk, int pid, std::vector<uint8_t> sec) {
        sec[1] = (uint8_t)(0xB0 | ((sec.size() + 1) >> 8));   // section length = what follows it + the CRC
        sec[2] = (uint8_t)((sec.size() + 1) & 0xFF);
        const uint32_t c = dect2::tsp::crc32(sec.data(), (int)sec.size());
        sec.push_back((uint8_t)(c >> 24)); sec.push_back((uint8_t)(c >> 16)); sec.push_back((uint8_t)(c >> 8)); sec.push_back((uint8_t)c);
        std::fill(pk, pk + 188, uint8_t(0xFF));
        pk[0] = 0x47; pk[1] = (uint8_t)(0x40 | (pid >> 8)); pk[2] = (uint8_t)pid; pk[3] = (uint8_t)(0x10 | (cc[pid]++ & 15)); pk[4] = 0;
        std::copy(sec.begin(), sec.end(), pk + 5);
    }
    void next(uint8_t* pk) {
        const uint64_t i = n++;
        const size_t slots = 2 + progs.size() + 20;
        const size_t slot = (size_t)(i % slots);
        if (slot == 0) {
            std::vector<uint8_t> s = {0x00, 0, 0, 0, 1, 0xC1, 0, 0};
            for (const auto& p : progs) { s.push_back((uint8_t)(p.sid >> 8)); s.push_back((uint8_t)p.sid); s.push_back((uint8_t)(0xE0 | (p.pmt >> 8))); s.push_back((uint8_t)p.pmt); }
            section(pk, 0, s);
            return;
        }
        if (slot == 1) {
            std::vector<uint8_t> s = {0x42, 0, 0, 0, 1, 0xC1, 0, 0, 0xFF, 0x01, 0xFF};
            for (const auto& p : progs) {
                const size_t dl = 2 + 3 + 1 + p.name.size();
                s.insert(s.end(), {(uint8_t)(p.sid >> 8), (uint8_t)p.sid, 0xFC, (uint8_t)(0x80 | (dl >> 8)), (uint8_t)dl, 0x48, (uint8_t)(dl - 2), 0x01, 1, 'P', (uint8_t)p.name.size()});
                s.insert(s.end(), p.name.begin(), p.name.end());
            }
            section(pk, 0x11, s);
            return;
        }
        if (slot < 2 + progs.size()) {
            const FakeProg& p = progs[slot - 2];
            std::vector<uint8_t> s = {0x02, 0, 0, (uint8_t)(p.sid >> 8), (uint8_t)p.sid, 0xC1, 0, 0, (uint8_t)(0xE0 | (p.es[0] >> 8)), (uint8_t)p.es[0], 0xF0, 0};
            for (int e : p.es) s.insert(s.end(), {0x1B, (uint8_t)(0xE0 | (e >> 8)), (uint8_t)e, 0xF0, 0});
            section(pk, p.pmt, s);
            return;
        }
        const size_t k = slot - 2 - progs.size();
        const FakeProg& p = progs[k % progs.size()];
        const int pid = p.es[(k / progs.size()) % p.es.size()];
        std::fill(pk, pk + 188, uint8_t(0x5A));
        pk[0] = 0x47; pk[1] = (uint8_t)(pid >> 8); pk[2] = (uint8_t)pid; pk[3] = (uint8_t)(0x10 | (cc[pid]++ & 15));
    }
};

struct Seen {
    std::map<int, int> pat;                    // programme -> PMT PID
    std::map<int, std::vector<int>> es;        // PMT PID -> streams
    std::map<int, int> pmtProg, pcr;
    std::string sdt;
    std::set<int> pids;
    int crcBad = 0;
};

Seen run(dect2::TsRemux& mux, int packets) {
    Seen s;
    std::set<int> pmts;
    for (int i = 0; i < packets; i++) {
        uint8_t pk[188];
        mux.next(pk);
        const int pid = ((pk[1] & 0x1F) << 8) | pk[2];
        s.pids.insert(pid);
        if (!(pk[1] & 0x40) || pid == 0x1FFF) continue;
        const size_t at = 5 + (size_t)pk[4];
        const size_t n = 3 + (((size_t)pk[at + 1] & 15) << 8 | pk[at + 2]);
        if (at + n > 188) continue;
        const std::vector<uint8_t> sec(pk + at, pk + at + n);
        if (dect2::tsp::crc32(sec.data(), (int)n) != 0) { s.crcBad++; continue; }
        if (sec[0] == 0x00) {
            s.pat.clear();
            for (size_t p = 8; p + 4 <= n - 4; p += 4) s.pat[sec[p] << 8 | sec[p + 1]] = (sec[p + 2] & 0x1F) << 8 | sec[p + 3];
        } else if (sec[0] == 0x42) s.sdt.assign(sec.begin(), sec.end());
        else if (sec[0] == 0x02) {
            s.pmtProg[pid] = sec[3] << 8 | sec[4];
            s.pcr[pid] = (sec[8] & 0x1F) << 8 | sec[9];
            auto& v = s.es[pid];
            v.clear();
            for (size_t p = 12; p + 5 <= n - 4; p += 5 + (((size_t)sec[p + 3] & 15) << 8 | sec[p + 4])) v.push_back((sec[p + 1] & 0x1F) << 8 | sec[p + 2]);
        }
    }
    return s;
}

void testSetNumbering() {
    FakeMux a, b;
    a.progs = {{1, 0x1000, {0x100, 0x101}, "Cam"}};
    b.progs = {{1, 0x1000, {0x100, 0x101}, "Bars"}};
    std::vector<dect2::TsRemuxInput> in(2);
    in[0].pop = [&](uint8_t* pk) { a.next(pk); return true; };
    in[0].rate = [] { return 1e6; };
    in[0].serviceId = 300; in[0].pmtPid = 0x50; in[0].esPids = {0x60, 0x61};
    in[1].pop = [&](uint8_t* pk) { b.next(pk); return true; };
    in[1].rate = [] { return 1e6; };
    in[1].esPids = {0, 0x90};   // video automatic, audio set
    dect2::TsRemuxConfig c;
    c.outBitrate = 3e6; c.firstServiceId = 10;
    dect2::TsRemux mux(in, c);
    const Seen s = run(mux, 20000);
    MCHECK(s.crcBad == 0, "set numbering: good CRCs (%d bad)", s.crcBad);
    MCHECK(s.pat.size() == 2 && s.pat.count(300) && s.pat.at(300) == 0x50 && s.pat.count(11) && s.pat.at(11) == 0x1001, "set numbering: PAT 300 -> 0x50 and 11 -> 0x1001 (%zu)", s.pat.size());
    MCHECK(s.es.count(0x50) && s.es.at(0x50) == std::vector<int>({0x60, 0x61}) && s.pcr.at(0x50) == 0x60, "set numbering: the set stream PIDs");
    MCHECK(s.es.count(0x1001) && s.es.at(0x1001) == std::vector<int>({0x200, 0x90}), "set numbering: automatic and set PIDs mixed");
    MCHECK(s.sdt.find("Cam") != std::string::npos && s.sdt.find("Bars") != std::string::npos, "set numbering: SDT names");
    const std::set<int> want = {0, 0x11, 0x50, 0x60, 0x61, 0x1001, 0x200, 0x90, 0x1FFF};
    MCHECK(s.pids == want, "set numbering: only the set PIDs (%zu seen)", s.pids.size());
    const auto progs = mux.programmes();
    MCHECK(progs.size() == 2 && progs[0].serviceId == 300 && progs[0].pmtPid == 0x50 && progs[0].name == "Cam", "set numbering: programmes() reports them");
}

void testKeptNumbering() {
    FakeMux a, b;
    a.progs = {{10, 0x100, {0x200, 0x201}, "A ten"}, {11, 0x110, {0x210}, "A eleven"}};
    b.progs = {{10, 0x100, {0x300}, "B ten"}, {20, 0x120, {0x320}, "B twenty"}};   // programme 10 and PMT 0x100 collide with input A
    std::vector<dect2::TsRemuxInput> in(2);
    in[0].pop = [&](uint8_t* pk) { a.next(pk); return true; };
    in[0].rate = [] { return 2e6; };
    in[0].keepIds = true;
    in[1].pop = [&](uint8_t* pk) { b.next(pk); return true; };
    in[1].rate = [] { return 2e6; };
    in[1].keepIds = true;
    dect2::TsRemuxConfig c;
    c.outBitrate = 5e6;
    dect2::TsRemux mux(in, c);
    const Seen s = run(mux, 30000);
    MCHECK(s.crcBad == 0, "kept numbering: good CRCs (%d bad)", s.crcBad);
    std::string patText;
    for (const auto& [p, pmt] : s.pat) patText += " " + std::to_string(p) + "->0x" + std::to_string(pmt);
    MCHECK(s.pat.size() == 4, "kept numbering: four programmes (%zu:%s)", s.pat.size(), patText.c_str());
    MCHECK(s.pat.count(10) && s.pat.at(10) == 0x100 && s.pat.count(11) && s.pat.at(11) == 0x110, "kept numbering: input A as it is (%s)", patText.c_str());
    MCHECK(s.es.count(0x100) && s.es.at(0x100) == std::vector<int>({0x200, 0x201}) && s.es.count(0x110) && s.es.at(0x110) == std::vector<int>({0x210}), "kept numbering: A's PIDs kept");
    MCHECK(s.pat.count(20) && s.pat.at(20) == 0x120 && s.es.count(0x120) && s.es.at(0x120) == std::vector<int>({0x320}), "kept numbering: B's programme 20 kept");
    int movedProg = 0;
    for (const auto& [p, pmt] : s.pat) if (p != 10 && p != 11 && p != 20) movedProg = p;
    MCHECK(movedProg == 12 && s.pat.at(12) == 0x1002 && s.es.count(0x1002) && s.es.at(0x1002) == std::vector<int>({0x300}), "kept numbering: B's programme 10 moved to 12 on PMT 0x1002 (%d)", movedProg);
    for (const char* name : {"A ten", "A eleven", "B ten", "B twenty"}) MCHECK(s.sdt.find(name) != std::string::npos, "kept numbering: SDT has %s", name);
}

using namespace dect2::vgen;

bool waitUntil(const std::function<bool()>& f, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

// The names of this process's threads, as the system shows them (Linux /proc, Windows GetThreadDescription)
std::vector<std::string> threadNames() {
    std::vector<std::string> v;
#ifdef _WIN32
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto getDesc = reinterpret_cast<GetDesc>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    if (!getDesc) return v;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return v;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR d = nullptr;
        if (SUCCEEDED(getDesc(h, &d)) && d) {
            std::string n;
            for (const wchar_t* c = d; *c; c++) n += (char)(*c < 128 ? *c : '?');
            v.push_back(n);
            LocalFree(d);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
#else
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/comm");
            std::string n;
            std::getline(f, n);
            v.push_back(n);
        }
        closedir(d);
    }
#endif
    return v;
}
int countPrefix(const std::vector<std::string>& v, const std::string& p) {
    int n = 0;
    for (const auto& x : v) if (x.compare(0, p.size(), p) == 0) n++;
    return n;
}

GenConfig patternSource(int pattern, int sid) {
    GenConfig c;
    c.source = Source::Generator;
    c.pattern = pattern;
    c.audio = AudioSource::Silence;
    c.enc.content = TsContent::VideoOnly;
    c.enc.videoEncoder = "mpeg2video";
    c.enc.width = 352; c.enc.height = 288; c.enc.fps = 10;
    c.enc.serviceId = sid > 0 ? sid : 1;   // its own programme number; the multiplex renumbers it
    c.enc.serviceName = "Pattern " + std::to_string(pattern);
    return c;
}

// the programmes of the PAT in a run of packets
std::map<int, int> patOf(const std::vector<uint8_t>& d) {
    std::map<int, int> pat;
    for (size_t o = 0; o + 188 <= d.size(); o += 188) {
        const uint8_t* pk = &d[o];
        if (pk[0] != 0x47 || (((pk[1] & 0x1F) << 8) | pk[2]) != 0 || !(pk[1] & 0x40)) continue;
        const size_t at = 5 + (size_t)pk[4];
        const size_t n = 3 + (((size_t)pk[at + 1] & 15) << 8 | pk[at + 2]);
        if (at + n > 188 || dect2::tsp::crc32(pk + at, (int)n) != 0) continue;
        pat.clear();
        for (size_t p = at + 8; p + 4 <= at + n - 4; p += 4) pat[pk[p] << 8 | pk[p + 1]] = (pk[p + 2] & 0x1F) << 8 | pk[p + 3];
    }
    return pat;
}

void testSessionThreads() {
    VgLinkServer srv;
    std::string err;
    MCHECK(srv.start(true, 0, false, err, vgTestPipeName()), "session: the link server listens (%s)", err.c_str());
    VgLinkClient link;
    link.start("127.0.0.1", srv.port(), "Session test", 3, "");   // over UDP (the chain test below uses the pipe)
    MCHECK(waitUntil([&] { return link.connected(); }, 5), "session: the generator is linked (%s)", link.status().c_str());
    const int slot = link.slot();
    std::vector<uint8_t> got;
    auto drain = [&] { uint8_t pk[188]; while (generatorPop(slot, pk)) got.insert(got.end(), pk, pk + 188); };
    {
        Session ses;
        std::vector<Session::SourceSpec> src(3);
        for (int k = 0; k < 3; k++) src[(size_t)k].cfg = patternSource(k, 0);
        src[2].enabled = false;   // added while the mux runs
        MuxConfig m;
        m.output = Output::Transmitter;
        m.link = &link;
        m.channelBps = 6e6;
        m.firstServiceId = 500;
        MCHECK(ses.start(src, m, err), "session: start (%s)", err.c_str());
        MCHECK(waitUntil([&] { drain(); return patOf(got).size() == 2; }, 8), "session: two programmes on air (%zu)", patOf(got).size());
        MCHECK(generatorRate(slot) == 6e6, "session: the hub sees the mux rate (%.0f)", generatorRate(slot));
        const std::vector<std::string> names = threadNames();
        if (!names.empty()) {
            MCHECK(countPrefix(names, "vg-src-1") == 1 && countPrefix(names, "vg-src-2") == 1 && countPrefix(names, "vg-mux") == 1,
                   "session: named threads vg-src-1, vg-src-2, vg-mux (%d %d %d)", countPrefix(names, "vg-src-1"), countPrefix(names, "vg-src-2"), countPrefix(names, "vg-mux"));
        } else std::printf("session: thread names cannot be read here\n");
        // sources come and go in random order while the mux runs
        std::mt19937 rng(458);
        std::array<bool, 3> on = {true, true, false};
        for (int step = 0; step < 8; step++) {
            const int k = (int)(rng() % 3);
            if (on[(size_t)k]) ses.stopSource(k);
            else MCHECK(ses.startSource(k, patternSource(k, 0), MuxNumbering(), err), "session: start source %d (%s)", k + 1, err.c_str());
            on[(size_t)k] = !on[(size_t)k];
            std::this_thread::sleep_for(std::chrono::milliseconds(150 + rng() % 250));
            drain();
            MCHECK(ses.running(), "session: the mux keeps running while sources change (step %d)", step);
        }
        for (int k = 0; k < 3; k++) if (!on[(size_t)k]) { MCHECK(ses.startSource(k, patternSource(k, 0), MuxNumbering(), err), "session: start %d (%s)", k + 1, err.c_str()); on[(size_t)k] = true; }
        got.clear();
        MCHECK(waitUntil([&] { drain(); return patOf(got).size() == 3; }, 8), "session: all three programmes after the changes (%zu)", patOf(got).size());
        // the preview: on, switched, off while streaming
        ses.startPreview(1);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        uint64_t ser = 0;
        MCHECK(waitUntil([&] { return ses.previewFrame(rgba, w, h, ser); }, 5) && w == 352 && h == 288 && rgba.size() == (size_t)w * h * 4, "session: a preview picture of source 2 (%dx%d)", w, h);
        if (!threadNames().empty()) MCHECK(countPrefix(threadNames(), "vg-preview") == 1, "session: the preview thread is named vg-preview");
        ses.startPreview(2);
        const uint64_t f0 = ses.previewStatus().frames;
        MCHECK(waitUntil([&] { return ses.previewStatus().frames > f0 + 3 && ses.previewStatus().source == 2; }, 5), "session: the preview follows source 3");
        ses.stopPreview();
        MCHECK(!ses.previewRunning(), "session: the preview stops");
        ses.startPreview(0);
        MCHECK(waitUntil([&] { return ses.previewFrame(rgba, w, h, ser); }, 5), "session: the preview starts again");
        // a stalled source: slot 6 says it runs but sends nothing; the mux pads its slot and goes on at its rate
        ses.queue(5).rate = 1.5e6;
        const MuxStatus a = ses.muxStatus();
        const uint64_t s0 = ses.sourceStatus(0).gen.frames;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        drain();
        const MuxStatus b = ses.muxStatus();
        const double sent = (double)(b.packets - a.packets) * 1504.0;
        MCHECK(sent > 6e6 * 1.6 && sent < 6e6 * 2.4, "session: with a stalled source the mux still sends its rate (%.2f Mbit in 2 s)", sent / 1e6);
        MCHECK(ses.sourceStatus(0).gen.frames - s0 >= 15, "session: the other sources keep encoding (%llu frames in 2 s)", (unsigned long long)(ses.sourceStatus(0).gen.frames - s0));
        MCHECK(b.nulls > a.nulls, "session: the stalled slots are padded with null packets");
        ses.queue(5).rate = 0;
        const SourceStatus st0 = ses.sourceStatus(0);
        MCHECK(st0.running && st0.gen.error.empty(), "session: source 1 runs without error (%s)", st0.gen.error.c_str());
        // Stop: sources, then mux; the preview thread stops with the session
        ses.stop();
        MCHECK(!ses.running() && !ses.sourceRunning(0) && !ses.sourceRunning(1), "session: Stop stops the sources and the mux");
        MCHECK(waitUntil([&] { return generatorRate(slot) == 0; }, 3), "session: OnAir sees the stream stop");
    }   // the Session's destructor joins the preview thread
    // the session's own threads are joined; helper threads that libraries start from them (FFmpeg, the encoders) carry the same name on
    // Linux and may end a moment later: wait for those, and name what is left
    auto ours = [] {
        std::string left;
        for (const auto& n : threadNames()) if (n.rfind("vg-src", 0) == 0 || n.rfind("vg-mux", 0) == 0 || n.rfind("vg-preview", 0) == 0) left += " " + n;
        return left;
    };
    MCHECK(waitUntil([&] { return ours().empty(); }, 3), "session: no generator thread is left after the session ends:%s", ours().c_str());
    link.stop();
    srv.stop();
}

void testSessionFile() {
    namespace fs = std::filesystem;
    const fs::path f = fs::temp_directory_path() / ("onair_vg_mux_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()) + ".ts");
    Session ses;
    std::vector<Session::SourceSpec> src(2);
    src[0].cfg = patternSource(0, 0); src[0].cfg.seconds = 3;
    src[1].cfg = patternSource(1, 0); src[1].cfg.seconds = 3;
    src[1].num.serviceId = 777; src[1].num.pmtPid = 0x40; src[1].num.esPids = {0x41};
    MuxConfig m;
    m.output = Output::File;
    m.tsPath = f.string();
    m.firstServiceId = 10;
    std::string err;
    MCHECK(ses.start(src, m, err), "file: start (%s)", err.c_str());
    MCHECK(waitUntil([&] { return !ses.running(); }, 60), "file: the mux ends when the sources have");
    const MuxStatus ms = ses.muxStatus();
    MCHECK(ms.finished && ms.error.empty(), "file: finished (%s)", ms.error.c_str());
    MCHECK(ses.sourceStatus(0).dropped == 0 && ses.sourceStatus(1).dropped == 0, "file: nothing dropped");
    AVFormatContext* fc = nullptr;
    int progs = 0;
    bool has777 = false;
    if (avformat_open_input(&fc, f.string().c_str(), nullptr, nullptr) == 0) {
        avformat_find_stream_info(fc, nullptr);
        progs = (int)fc->nb_programs;
        for (unsigned i = 0; i < fc->nb_programs; i++) if (fc->programs[i]->program_num == 777) has777 = true;
        avformat_close_input(&fc);
    }
    MCHECK(progs == 2 && has777, "file: FFmpeg finds two programmes, one of them 777 (%d)", progs);
    const double secs = ms.seconds;
    MCHECK(secs > 2.5 && secs < 4.5, "file: about 3 s of multiplex (%.2f s)", secs);
    std::error_code ec;
    fs::remove(f, ec);
}

// Easy mode: the stream follows the transmitter's usable rate
void testEasyFit() {
    Settings e;
    e.videoEncoder = "libx264";
    e.width = 720; e.height = 576; e.fps = 25;
    MCHECK(fitVideoToMux(e, 4850), "easy: fits 4.85 Mbit/s");
    const Budget b = estimate(e);
    MCHECK(e.muxKbps == 4850 && b.suggestedMux <= 4850 && b.suggestedMux > 4850 - 200, "easy: the mux rate is the usable rate and the video fills it (%d of 4850)", b.suggestedMux);
    MCHECK(!e.autoBitrate && !e.autoMux && e.videoKbps > 3500, "easy: a set video bitrate (%d kbit/s)", e.videoKbps);
    Settings m;
    m.videoEncoder = "mpeg2video";
    m.width = 1920; m.height = 1080;
    MCHECK(!fitVideoToMux(m, 150) && m.videoKbps == minVideoKbps(m), "easy: too small a channel is reported, at the lowest bitrate");
}

void testMuxRate() {
    MuxConfig m;
    m.channelBps = 10e6;
    MCHECK(muxRateFor(m, {1e6, 2e6}) == 10e6, "mux rate: padded to the channel");
    m.padding = false;
    MCHECK(std::abs(muxRateFor(m, {1e6, 2e6}) - 3.08e6) < 1, "mux rate: the sources plus a margin without padding");
    m.channelBps = 0;
    m.padding = true;
    MCHECK(std::abs(muxRateFor(m, {1e6}) - 1.04e6) < 1, "mux rate: no channel known: the sources");
    m.channelBps = 2e6;
    m.padding = false;
    MCHECK(muxRateFor(m, {3e6}) == 2e6, "mux rate: never above the channel");
}

// The whole chain (#458): the generator program's multiplex over the RTP link into OnAir, read by the DVB-T transmitter as
// "generator:N" (the TX panel's video source), on the air and decoded by OnAir's own DVB-T receiver: its programme number comes out.
void testTxChain() {
    using dect2::TxModeSpec; using dect2::TxRequest; using dect2::makeTxGenerator; using dect2::txModeById; using dect2::txTsBitrate; using dect2::DvbtReceiver; using dect2::cf32;
    VgLinkServer srv;
    std::string err;
    const std::string pipe = vgTestPipeName();
    MCHECK(srv.start(false, 0, false, err, pipe), "chain: the link server listens (%s)", err.c_str());
    VgLinkClient link;
    link.startLocal(pipe, "Chain test", 0, "");   // the local pipe, as OnAir starts its generator
    MCHECK(waitUntil([&] { return link.connected(); }, 5), "chain: the generator is linked (%s)", link.status().c_str());
    const int slot = link.slot();
    const TxModeSpec* mode = txModeById("dvbt");
    MCHECK(mode != nullptr, "chain: DVB-T is a transmit mode");
    if (!mode) return;
    TxRequest r;
    r.modeId = "dvbt";
    r.bandwidthMhz = 8;
    r.loop = true;
    r.file = generatorSpec(slot);
    const double fs = 64e6 / 7;
    auto tx = makeTxGenerator(*mode, r, fs, err);
    MCHECK(tx != nullptr, "chain: the DVB-T transmitter reads %s (%s)", generatorSpec(slot).c_str(), err.c_str());
    if (!tx) return;
    MCHECK(txTsBitrate() > 4e6 && txTsBitrate() < 32e6, "chain: the transmitter tells its TS rate (%.0f)", txTsBitrate());
    VgTxInfo t;   // what the TX panel tells the generators
    t.running = t.digitalTv = true;
    t.channelBps = txTsBitrate();
    t.system = "DVB-T";
    t.slots = {slot};
    srv.setTxInfo(t);
    double usable = 0;
    MCHECK(waitUntil([&] { link.txInfo(usable); return usable > 0; }, 5), "chain: the generator learns the usable rate");
    Session ses;
    std::vector<Session::SourceSpec> src(1);
    src[0].cfg = patternSource(0, 0);
    MuxNumbering num;
    num.serviceId = 458;
    src[0].num = num;
    MuxConfig m;
    m.output = Output::Transmitter;
    m.link = &link;
    m.channelBps = usable;
    MCHECK(ses.start(src, m, err), "chain: the generator starts (%s)", err.c_str());
    DvbtReceiver rx;
    rx.configure(fs, 8);
    std::vector<uint8_t> got;
    size_t bad = 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            if (pk[i * 188 + 1] & 0x80) { bad++; continue; }
            got.insert(got.end(), pk + i * 188, pk + i * 188 + 188);
        }
    });
    std::vector<cf32> buf(1 << 15);
    const auto t0 = std::chrono::steady_clock::now();
    double sent = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) {   // the transmitter at the channel's pace
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        while (sent < el * fs) { tx->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); sent += (double)buf.size(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ses.stop();
    const std::map<int, int> pat = patOf(got);
    MCHECK(rx.detectLevel() == 3, "chain: the receiver decodes the transport stream (level %d)", rx.detectLevel());
    MCHECK(pat.count(458) == 1, "chain: the generator's programme 458 comes out of the receiver (%zu programmes, %zu packets, %zu bad)", pat.size(), got.size() / 188, bad);
    size_t video = 0;   // packets on the programme's first stream PID (the generator's automatic numbering: 0x0100)
    for (size_t o = 0; o + 188 <= got.size(); o += 188) if ((((got[o + 1] & 0x1F) << 8) | got[o + 2]) == 0x0100) video++;
    MCHECK(video > 100, "chain: its picture is on the air (%zu video packets)", video);
    std::printf("chain: generator -> RTP link -> DVB-T transmitter -> receiver: %zu packets, %zu video, %zu bad\n", got.size() / 188, video, bad);
}

} // namespace

int videogenMuxTests() {
    testSetNumbering();
    testKeptNumbering();
    testMuxRate();
    testEasyFit();
    testSessionFile();
    testSessionThreads();
    testTxChain();
    return gFail;
}
