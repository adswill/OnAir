// Per-mode cost benchmark: every OnAir mode through the real receiver path (Engine, file source, no sound, no pacing), measured in CPU time.
//
//   bench_modes [--list] [--mode id[,id..]] [--seconds N] [--rates default|both|<Msps>[,<Msps>]] [--timeout CPU_S] [--wall-timeout S]
//               [--out path-prefix] [--spectrum] [--tmp dir]
//
// The machine may be loaded by other jobs, so the speed is never wall-clock: every figure is CPU seconds (user + system, all threads) the
// receiver used per second of signal ("cost"), and the real-time factor is its inverse (how many such streams one core could carry).
// How it runs: the parent starts a child process per mode and rate (a crash, a hang or a CPU-time overrun of one mode only marks that row);
// stage 1 (--gen) plays the mode's built-in test signal through the synthetic source as fast as it goes and stores it as a CS8 file, with the
// radio's tune offset mixed out as the engine does for a live radio; stage 2 (--run) is a fresh process that feeds that file through the Engine
// (DECT2_NOPACE: as fast as the receiver takes it) and measures CPU, peak RSS and threads. Generation cost is therefore never in the figures.
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
using namespace dect2;
using Clock = std::chrono::steady_clock;

namespace {

// ------------------------------------------------------------------ the mode list

struct Entry {
    std::string id, name;
    int stdMode = 0;            // the engine standard (Engine::setStandard)
    int synthMode = 0;          // SynthConfig::mode
    const SubMode* sub = nullptr;
    const ModeTuning* mt = nullptr;
    double defMhz = 100, bwMhz = 1, rate = 2e6, minRate = 0, offsetHz = 0;
    bool fixedRate = false;     // the test signal's rate is fixed (DVB-T2, DVB-T, ATSC): no second rate
    int kind = 0;               // 0 table, 1 t2, 2 dvbt, 3 atsc, 4 dab, 5 atsc3, 6 isdbt, 7 fm
};

std::vector<Entry> allEntries() {
    std::vector<Entry> v;
    auto core = [&](const char* id, const char* name, int std_, int kind, double bw, double rate, bool fixed, double mhz) {
        Entry e; e.id = id; e.name = name; e.stdMode = std_; e.synthMode = std_ >= 4 ? std_ : 0; e.kind = kind; e.bwMhz = bw; e.rate = rate; e.fixedRate = fixed; e.defMhz = mhz;
        e.minRate = minSampleRateFor(std_, bw);
        v.push_back(e);
    };
    core("t2", "DVB-T2", 1, 1, 8, 0, true, 522);
    core("dvbt", "DVB-T", 2, 2, 8, 0, true, 522);
    core("atsc", "ATSC 1.0", 3, 3, 6, 8e6, true, 600);
    core("dab", "DAB", 4, 4, 8, 10e6, false, 225);
    core("atsc3", "ATSC 3.0", 5, 5, 8, 10e6, false, 600);
    core("isdbt", "ISDB-T", 6, 6, 6, 8e6, false, 600);
    core("fm", "FM", 7, 7, 0.25, 4e6, false, 93.9);
    std::vector<std::string> seen;
    for (int s = 8; s < 200; s++) {
        const ModeTuning* m = modeTuning(s);
        if (!m || std::find(seen.begin(), seen.end(), m->id) != seen.end()) continue;
        seen.push_back(m->id);
        Entry e; e.id = m->id; e.name = m->name; e.stdMode = m->stdMode; e.synthMode = m->stdMode; e.mt = m;
        e.defMhz = m->defMhz; e.bwMhz = m->bandwidthMhz; e.rate = m->sampleRate; e.minRate = m->minSampleRate; e.offsetHz = m->tuneOffsetHz;
        v.push_back(e);
    }
    int n = 0;
    const SubMode* sm = subModes(n);
    for (int i = 0; i < n; i++) {
        const ModeTuning* m = modeTuning(sm[i].stdMode);
        if (!m) continue;
        Entry e; e.id = sm[i].id; e.name = sm[i].name; e.stdMode = sm[i].stdMode; e.synthMode = sm[i].stdMode; e.sub = &sm[i]; e.mt = m;
        e.defMhz = sm[i].defMhz; e.bwMhz = m->bandwidthMhz; e.rate = m->sampleRate; e.minRate = m->minSampleRate; e.offsetHz = m->tuneOffsetHz;
        v.push_back(e);
    }
    return v;
}

// the second rate: twice the default (a radio that cannot run slow), or half of it when that would pass 20 Msps
std::vector<double> ratesFor(const Entry& e, const std::string& spec) {
    std::vector<double> r;
    if (spec == "default" || spec == "both") {
        r.push_back(e.rate);
        if (spec == "both" && !e.fixedRate && e.rate > 0) {
            double o = e.rate * 2 <= 20e6 ? e.rate * 2 : e.rate / 2;
            if (o >= e.minRate && o > 0) r.push_back(o);
        }
    } else {
        size_t p = 0;
        while (p < spec.size()) {
            size_t q = spec.find(',', p);
            if (q == std::string::npos) q = spec.size();
            const double mhz = atof(spec.substr(p, q - p).c_str());
            if (mhz > 0 && (e.fixedRate ? mhz * 1e6 == e.rate : true)) r.push_back(mhz * 1e6);
            p = q + 1;
        }
        if (e.fixedRate) r.assign(1, e.rate);
    }
    return r;
}

void tuneFor(const Entry& e, double rate, TuneSettings& t) {
    t.bandwidthMhz = e.bwMhz;
    t.sampleRate = rate;
    t.centerHz = e.defMhz * 1e6;
    t.synth.mode = e.synthMode;
    t.synth.snrDb = 30;
    if (e.kind == 2) t.synth.dvbt = true;
    if (e.kind == 3) t.synth.atsc = true;
    if (e.kind == 6) { t.isdbtBwMhz = 6; t.synth.isdbtBwMhz = 6; }
    if (e.sub) applySubModeSynth(t.synth, *e.sub);
}

void muteAndConfigure(Engine& e, const Entry& en, bool spectrum) {
    e.setComputeMode(0);
    e.fm().setSilent(true); e.vor().setSilent(true); e.m17().setSilent(true); e.cw().setSilent(true);
    e.psk().setListen(false); e.js8().setListen(false); e.ale().setListen(false); e.hfdig().setListen(false);
    e.specx().setSilent(true); e.uplink().setSilent(true); e.specan().setSilent(true); e.astro().setSilent(true); e.iono().setSilent(true);
    e.qo100().setSilent(true); e.general().setSilent(true); e.hfbeacon().setSilent(true); e.same().setSilent(true);
    e.dstar().setSilent(true); e.nxdn().setSilent(true); e.dpmr().setSilent(true); e.p25().setSilent(true); e.mac().setSilent(true);
    e.setWideMuted(true);
    e.setSpectrumEnabled(spectrum);
    e.setStandard(en.stdMode);
    if (en.sub) applySubMode(e, en.sub);
    if (en.stdMode == 22) e.mesh().setTunedHz(en.defMhz * 1e6);
}

// ------------------------------------------------------------------ measuring helpers

double cpuSeconds() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}
double peakRssMb() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return u.ru_maxrss / 1048576.0;
#else
    return u.ru_maxrss / 1024.0;
#endif
}
// thread count and the CPU seconds of the busiest thread (cumulative)
void threadStats(int& count, double& busiest) {
    count = 0; busiest = 0;
#ifdef __APPLE__
    thread_act_array_t th; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
    count = (int)n;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS) {
            const double s = bi.user_time.seconds + bi.system_time.seconds + (bi.user_time.microseconds + bi.system_time.microseconds) * 1e-6;
            busiest = std::max(busiest, s);
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    const long tck = sysconf(_SC_CLK_TCK);
    for (auto& d : fs::directory_iterator("/proc/self/task", ec)) {
        count++;
        if (FILE* f = fopen((d.path() / "stat").string().c_str(), "r")) {
            char buf[1024]; size_t k = fread(buf, 1, sizeof buf - 1, f); buf[k] = 0; fclose(f);
            const char* p = strrchr(buf, ')');
            unsigned long ut = 0, st = 0;
            if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) busiest = std::max(busiest, (double)(ut + st) / tck);
        }
    }
#endif
}

// ------------------------------------------------------------------ stage 1: generate the signal file

int genChild(const Entry& en, double rate, double secs, const std::string& path) {
    setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    tuneFor(en, rate, t);
    t.centerHz = (en.defMhz * 1e6) + en.offsetHz;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    auto src = makeSource(dev);
    IqRing ring(1u << 22);
    std::string err;
    if (!src || !src->start(t, ring, err)) { fprintf(stderr, "synthetic source: %s\n", err.c_str()); return 3; }
    const double r = src->sampleRate();
    const uint64_t total = (uint64_t)(r * secs);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return 4;
    std::vector<cf32> buf(1 << 16);
    std::vector<int8_t> out(buf.size() * 2);
    uint64_t done = 0;
    double ph = 0;
    const double dph = en.offsetHz != 0 && en.kind == 0 ? 2 * M_PI * en.offsetHz / r : 0;   // as the engine mixes out the tune offset of a live radio (the test signal sits at -offset)
    auto lastProgress = Clock::now();
    while (done < total) {
        const size_t n = ring.read(buf.data(), (size_t)std::min<uint64_t>(buf.size(), total - done));
        if (!n) {
            if (Clock::now() - lastProgress > std::chrono::seconds(60)) { fprintf(stderr, "no samples for 60 s\n"); return 5; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        lastProgress = Clock::now();
        for (size_t i = 0; i < n; i++) {
            cf32 s = buf[i];
            if (dph != 0) { s *= cf32((float)cos(ph), (float)sin(ph)); ph += dph; if (ph < -M_PI) ph += 2 * M_PI; }
            out[2 * i] = (int8_t)std::lround(std::min(127.f, std::max(-128.f, s.real() * 128.f)));
            out[2 * i + 1] = (int8_t)std::lround(std::min(127.f, std::max(-128.f, s.imag() * 128.f)));
        }
        fwrite(out.data(), 1, n * 2, f);
        done += n;
    }
    fclose(f);
    src->stop();
    printf("RATE %.6f\n", r);
    return 0;
}

// ------------------------------------------------------------------ stage 2: run the file through the engine

int runChild(const Entry& en, double rate, double secs, const std::string& path, bool spectrum) {
    setenv("DECT2_NOPACE", "1", 1);
    TuneSettings t;
    tuneFor(en, rate, t);
    t.synth = SynthConfig();   // the engine reads a file: no generator
    Engine e;
    muteAndConfigure(e, en, spectrum);
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = path;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = rate; fo.loop = false;
    // order-sensitive FNV-1a of every transport-stream packet the receiver delivers, with a checkpoint every 64 packets: the run stops when the
    // file has been processed, so the tail varies with timing, but the common prefix of the checkpoints of two runs must be identical
    std::string tsCps; uint64_t tsN = 0, tsH = 1469598103934665603ull;
    e.setPacketTap([&](const uint8_t* pk, size_t n, const TsSnapshot*, double) {
        for (size_t i = 0; i < n; i++) {
            for (int b = 0; b < 188; b++) tsH = (tsH ^ pk[i * 188 + b]) * 1099511628211ull;
            if (++tsN % 64 == 0) { char h[24]; snprintf(h, sizeof h, "%s%llx", tsCps.empty() ? "" : ",", (unsigned long long)tsH); tsCps += h; }
        }
    });
    const double cpu0 = cpuSeconds();
    if (!e.start(dev, t, fo)) {
        size_t n; std::string last; for (auto& l : e.logSnapshot(n)) last = l;
        fprintf(stderr, "start failed: %s\n", last.c_str());
        return 3;
    }
    struct Pt { double cpu, proc; };
    std::vector<Pt> pts;
    int thr = 0, maxThr = 0; double busiest = 0;
    double proc = 0, lastProc = 0;
    auto tStart = Clock::now(), lastMove = tStart;
    auto processed = [&] {
        double rx = 0, sig = 0;
        const std::string lp = e.loadProfile();
        const char* p = strstr(lp.c_str(), "receiver ");
        if (p && sscanf(p, "receiver %lf s for %lf s", &rx, &sig) == 2) return sig;
        return 0.0;
    };
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        proc = processed();
        pts.push_back({cpuSeconds() - cpu0, proc});
        threadStats(thr, busiest);
        maxThr = std::max(maxThr, thr);
        if (proc > lastProc + 1e-6) { lastProc = proc; lastMove = Clock::now(); }
        if (proc >= secs - 0.05) break;
        if (Clock::now() - lastMove > std::chrono::seconds(5) && proc >= 0.9 * secs) break;   // the file ended a block short
        if (Clock::now() - lastMove > std::chrono::seconds(60)) break;                         // stuck: report what there is
    }
    const double cpuEnd = cpuSeconds() - cpu0;
    const double wall = std::chrono::duration<double>(Clock::now() - tStart).count();
    threadStats(thr, busiest);
    // steady state: from the point 25% of the signal was processed (skips plan creation and warm-up) when the run is long enough
    double cost = proc > 0 ? cpuEnd / proc : 0, startup = 0;
    if (secs >= 4 && proc > 0) {
        for (auto& p : pts) if (p.proc >= 0.25 * proc) {
            if (proc - p.proc > 0.1) { cost = (cpuEnd - p.cpu) / (proc - p.proc); startup = std::max(0.0, p.cpu - 0.25 * proc * cost); }
            break;
        }
    }
    RxTelemetry rx; uint64_t seq = 0;
    e.latestRx(rx, seq);
    size_t nlog = 0;
    e.logSnapshot(nlog);
    char dir[] = "/tmp/bench_modes_res_XXXXXX";
    uint64_t resBytes = 0, resHash = 1469598103934665603ull;   // FNV-1a over the saved results (order-independent per file): proves the output did not change
    if (mkdtemp(dir)) {
        e.saveResults(dir);
        std::error_code ec;
        for (auto& f : std::filesystem::directory_iterator(dir, ec)) {
            resBytes += f.file_size(ec);
            uint64_t fh = 1469598103934665603ull;
            if (FILE* rf = fopen(f.path().c_str(), "rb")) { int c; while ((c = fgetc(rf)) != EOF) fh = (fh ^ (uint64_t)c) * 1099511628211ull; fclose(rf); }
            for (char c : f.path().filename().string()) fh = (fh ^ (uint8_t)c) * 1099511628211ull;
            resHash += fh;
        }
        std::filesystem::remove_all(dir, ec);
    }
    const uint64_t lost = e.droppedSamples();
    e.stop();
    printf("{\"cost\":%.6f,\"cpu_total\":%.4f,\"startup_cpu\":%.4f,\"processed\":%.4f,\"rss_mb\":%.1f,\"threads\":%d,\"busiest_thread_frac\":%.3f,"
           "\"log_lines\":%zu,\"blocks_ok\":%llu,\"result_bytes\":%llu,\"result_hash\":\"%016llx\",\"ts_n\":%llu,\"ts_cps\":\"%s\",\"dropped\":%llu,\"wall\":%.3f}\n",
           cost, cpuEnd, startup, proc, peakRssMb(), maxThr, cpuEnd > 0 ? busiest / std::max(cpuSeconds(), 1e-9) : 0.0, nlog,
           (unsigned long long)rx.blocksOk, (unsigned long long)resBytes, (unsigned long long)resHash, (unsigned long long)tsN, tsCps.c_str(), (unsigned long long)lost, wall);
    fflush(stdout);
    _exit(0);   // the engine's destructor adds nothing to the figures and some receivers are slow to tear down
}

// ------------------------------------------------------------------ parent

struct ChildResult { int status = 0; bool timedOut = false, cpuLimit = false; std::string out; };

ChildResult spawn(const std::vector<std::string>& args, double cpuLimit, double wallLimit) {
    ChildResult r;
    int fd[2];
    if (pipe(fd) != 0) { r.status = -1; return r; }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]); close(fd[1]);
        const int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0 && !getenv("BENCH_MODES_VERBOSE")) dup2(dn, 2);
        rlimit rl; rl.rlim_cur = (rlim_t)cpuLimit; rl.rlim_max = (rlim_t)cpuLimit + 5;
        setrlimit(RLIMIT_CPU, &rl);
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(av[0], av.data());
        _exit(127);
    }
    close(fd[1]);
    fcntl(fd[0], F_SETFL, O_NONBLOCK);
    const auto t0 = Clock::now();
    int st = 0;
    char buf[4096];
    for (;;) {
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof buf)) > 0) r.out.append(buf, (size_t)n);
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (std::chrono::duration<double>(Clock::now() - t0).count() > wallLimit) { kill(pid, SIGKILL); waitpid(pid, &st, 0); r.timedOut = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) r.out.append(buf, (size_t)n);
    close(fd[0]);
    if (WIFSIGNALED(st)) { r.status = 128 + WTERMSIG(st); r.cpuLimit = WTERMSIG(st) == SIGXCPU || WTERMSIG(st) == SIGKILL; }
    else r.status = WEXITSTATUS(st);
    return r;
}

double jnum(const std::string& s, const char* key, double def = 0) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = s.find(k);
    return p == std::string::npos ? def : atof(s.c_str() + p + k.size());
}

struct Row {
    std::string id, name, status;
    double rate = 0, cost = 0, rtf = 0, startup = 0, rss = 0, busiest = 0, wallFactor = 0;
    int threads = 0;
    unsigned long long items = 0, resBytes = 0, dropped = 0;
};

std::string jstr(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    std::string self = argv[0];
    {
        std::error_code ec;
        const auto c = std::filesystem::canonical(self, ec);
        if (!ec) self = c.string();
    }
    double secs = 10, cpuLimit = 240, wallLimit = 1200;
    std::string modes, rates = "both", outPrefix, tmpDir;
    bool list = false, spectrum = false;
    std::string stage, stId, stFile;
    double stRate = 0, stSecs = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") list = true;
        else if (a == "--mode") modes = next();
        else if (a == "--seconds") secs = atof(next().c_str());
        else if (a == "--rates") rates = next();
        else if (a == "--timeout") cpuLimit = atof(next().c_str());
        else if (a == "--wall-timeout") wallLimit = atof(next().c_str());
        else if (a == "--out") outPrefix = next();
        else if (a == "--tmp") tmpDir = next();
        else if (a == "--spectrum") spectrum = true;
        else if (a == "--gen" || a == "--run") { stage = a; stId = next(); stRate = atof(next().c_str()); stSecs = atof(next().c_str()); stFile = next(); }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const std::vector<Entry> all = allEntries();
    auto find = [&](const std::string& id) -> const Entry* { for (auto& e : all) if (e.id == id) return &e; return nullptr; };
    if (!stage.empty()) {
        const Entry* en = find(stId);
        if (!en) return 2;
        return stage == "--gen" ? genChild(*en, stRate, stSecs, stFile) : runChild(*en, stRate, stSecs, stFile, spectrum);
    }
    std::vector<const Entry*> sel;
    if (modes.empty()) for (auto& e : all) sel.push_back(&e);
    else {
        size_t p = 0;
        while (p < modes.size()) {
            size_t q = modes.find(',', p);
            if (q == std::string::npos) q = modes.size();
            const std::string id = modes.substr(p, q - p);
            if (const Entry* e = find(id)) sel.push_back(e); else { fprintf(stderr, "no mode '%s' (--list shows them)\n", id.c_str()); return 2; }
            p = q + 1;
        }
    }
    if (list) {
        for (auto* e : sel) {
            printf("%-14s %-34s std %3d  rates:", e->id.c_str(), e->name.c_str(), e->stdMode);
            for (double r : ratesFor(*e, rates)) printf(" %.4g", r / 1e6);
            printf(" Msps\n");
        }
        printf("%zu modes\n", sel.size());
        return 0;
    }
    if (tmpDir.empty()) tmpDir = (std::filesystem::temp_directory_path() / ("bench_modes_" + std::to_string(getpid()))).string();
    std::filesystem::create_directories(tmpDir);
    std::vector<Row> rows;
    for (auto* e : sel) for (double rate : ratesFor(*e, rates)) {
        Row r; r.id = e->id; r.name = e->name; r.rate = rate;
        const std::string file = tmpDir + "/" + e->id + ".cs8";
        char rs[64], ss[64];
        snprintf(rs, sizeof rs, "%.4f", rate); snprintf(ss, sizeof ss, "%.3f", secs);
        fprintf(stderr, "[%s @ %.4g Msps] ", e->id.c_str(), rate / 1e6); fflush(stderr);
        const ChildResult g = spawn({self, "--gen", e->id, rs, ss, file}, cpuLimit, wallLimit);
        double actual = rate;
        const size_t rp = g.out.find("RATE ");
        if (rp != std::string::npos) actual = atof(g.out.c_str() + rp + 5);
        if (g.status != 0 || rp == std::string::npos) {
            r.status = g.timedOut ? "gen timeout" : g.cpuLimit ? "gen cpu-limit" : "gen failed (" + std::to_string(g.status) + ")";
        } else {
            r.rate = actual;
            snprintf(rs, sizeof rs, "%.4f", actual);
            const ChildResult c = spawn({self, "--run", e->id, rs, ss, file}, cpuLimit, wallLimit);
            const size_t jp = c.out.rfind('{');
            if (c.timedOut) r.status = "timeout";
            else if (c.cpuLimit) r.status = "cpu-limit";
            else if (c.status != 0 || jp == std::string::npos) r.status = "failed (" + std::to_string(c.status) + ")";
            else {
                const std::string j = c.out.substr(jp);
                r.cost = jnum(j, "cost");
                r.rtf = r.cost > 0 ? 1.0 / r.cost : 0;
                r.startup = jnum(j, "startup_cpu");
                r.rss = jnum(j, "rss_mb");
                r.threads = (int)jnum(j, "threads");
                r.busiest = jnum(j, "busiest_thread_frac");
                r.items = (unsigned long long)jnum(j, "log_lines") + (unsigned long long)jnum(j, "blocks_ok");
                r.resBytes = (unsigned long long)jnum(j, "result_bytes");
                r.dropped = (unsigned long long)jnum(j, "dropped");
                r.wallFactor = jnum(j, "processed") > 0 ? jnum(j, "wall") / jnum(j, "processed") : 0;
                r.status = jnum(j, "processed") < 0.9 * secs ? "stalled" : (r.items == 0 && r.resBytes == 0) ? "ok (nothing decoded)" : "ok";
            }
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
        fprintf(stderr, "%s cost %.4f\n", r.status.c_str(), r.cost);
        rows.push_back(r);
    }
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cost > b.cost; });
    printf("\n%-14s %9s %9s %9s %8s %7s %4s %6s %8s  %s\n", "mode", "rate Msps", "cost", "RT factor", "startup", "RSS MB", "thr", "busy%", "items", "status");
    for (auto& r : rows)
        printf("%-14s %9.4g %9.4f %9.1f %8.2f %7.0f %4d %6.0f %8llu  %s\n", r.id.c_str(), r.rate / 1e6, r.cost, r.rtf, r.startup, r.rss, r.threads, r.busiest * 100, r.items, r.status.c_str());
    printf("cost = CPU seconds (all threads) per second of signal; RT factor = 1/cost: streams one core could carry; startup = one-off CPU seconds; busy%% = busiest thread's share of the CPU\n");
    if (!outPrefix.empty()) {
        if (FILE* f = fopen((outPrefix + ".json").c_str(), "w")) {
            fprintf(f, "{\"seconds\":%.3f,\"rows\":[\n", secs);
            for (size_t i = 0; i < rows.size(); i++) {
                const Row& r = rows[i];
                fprintf(f, " {\"id\":\"%s\",\"name\":\"%s\",\"rate_hz\":%.0f,\"cost\":%.6f,\"rt_factor\":%.2f,\"startup_cpu\":%.3f,\"rss_mb\":%.1f,\"threads\":%d,\"busiest_thread_frac\":%.3f,"
                          "\"items\":%llu,\"result_bytes\":%llu,\"dropped\":%llu,\"wall_factor\":%.2f,\"status\":\"%s\"}%s\n",
                        jstr(r.id).c_str(), jstr(r.name).c_str(), r.rate, r.cost, r.rtf, r.startup, r.rss, r.threads, r.busiest, r.items, r.resBytes, r.dropped, r.wallFactor, jstr(r.status).c_str(), i + 1 < rows.size() ? "," : "");
            }
            fprintf(f, "]}\n");
            fclose(f);
        }
        if (FILE* f = fopen((outPrefix + ".md").c_str(), "w")) {
            fprintf(f, "| # | Mode | Rate (Msps) | Cost (CPU s per signal s) | Real-time factor | Startup CPU (s) | Peak RSS (MB) | Threads | Busiest thread | Items | Wall/signal | Status |\n|--:|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|\n");
            int k = 1;
            for (auto& r : rows)
                fprintf(f, "| %d | %s (`%s`) | %.4g | %.4f | %.1f | %.2f | %.0f | %d | %.0f%% | %llu | %.2f | %s |\n", k++, r.name.c_str(), r.id.c_str(), r.rate / 1e6, r.cost, r.rtf, r.startup, r.rss, r.threads, r.busiest * 100, r.items, r.wallFactor, r.status.c_str());
            fclose(f);
        }
    }
    return 0;
}
