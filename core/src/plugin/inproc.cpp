// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The in-process runner: the plugin's code runs on a worker thread of its own, fed through a bounded queue, with a watchdog thread.
// A plugin call that runs too long marks the plugin Hung: the host stops feeding it and stops waiting for it (a thread inside foreign code cannot
// be killed safely; it is abandoned and its library stays loaded). Use the helper-process runner for plugins that must be killable.
#include "dect2/common/thread_name.h"
#include "loader.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>

namespace dect2::plugin {

const char* runStateName(RunState s) {
    switch (s) {
    case RunState::Idle: return "idle";
    case RunState::Starting: return "starting";
    case RunState::Running: return "running";
    case RunState::Failed: return "failed";
    case RunState::Hung: return "hung";
    case RunState::Stopped: return "stopped";
    }
    return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

struct Job {
    enum K { Config, Iq, Event, Tick, Setting } k = Config;
    double a = 0, b = 0;
    std::vector<float> iq;
    IqMeta meta;
    automation::Event ev;
    std::string key;
};

// Everything the threads touch lives here, shared by the runner and the threads, so an abandoned thread never touches freed memory.
struct Core : ApiSink {
    StartOptions o;
    Manifest man;
    Lib lib;
    OnAirHostApi api{};
    void* inst = nullptr;

    std::mutex mu;
    std::condition_variable cv, doneCv;
    std::deque<Job> jobs;
    size_t queuedBytes = 0;
    std::atomic<int> state{(int)RunState::Idle};
    std::string err;
    std::atomic<int64_t> callStart{0};
    std::atomic<bool> stopReq{false}, abandoned{false}, workerDone{false};
    std::atomic<uint64_t> dropped{0}, processed{0};

    // ApiSink
    bool alive() override { return !abandoned.load() && !stopReq.load(); }
    void log(int level, const std::string& m) override { if (o.services.log) o.services.log(level, m); }
    std::string settingText(const std::string& k, bool& found) override {
        found = false;
        if (!o.settings) return {};
        for (const auto& s : man.settings) if (s.key == k) { found = true; return o.settings->text(k); }
        return {};
    }
    int requestIq(double hz, double sr) override { return o.services.requestIq ? o.services.requestIq(hz, sr) : -1; }
    int tableDefine(const std::string& i, const std::string& t, const std::vector<Col>& c) override { return o.results->defineTable(i, t, c); }
    int tableRow(const std::string& i, const std::vector<Cell>& c) override { return o.results->addRow(i, c); }
    int tableClear(const std::string& i) override { return o.results->clearTable(i); }
    int status(const std::string& k, const std::string& t, int l) override { return o.results->setStatus(k, t, l); }
    int resultLog(const std::string& l) override { return o.results->addLog(l); }
    int plot(const std::string& i, const std::string& t, int k, const float* d, size_t n, double x0, double dx) override { return o.results->setPlot(i, t, k, d, n, x0, dx); }
    int image(const std::string& i, int f, uint32_t w, uint32_t h, const uint8_t* d, size_t n) override { return o.results->setImage(i, f, w, h, d, n); }
    int mapPoint(const std::string& i, const std::string& l, double la, double lo) override { return o.results->setPoint(i, l, la, lo); }
    int emitEvent(const std::string& t, const automation::Event& e) override { return o.services.emitEvent ? o.services.emitEvent(t, e) : -1; }
    int audioOut(const float* p, size_t n, double r) override { return o.services.audioOut ? o.services.audioOut(p, n, r) : -1; }
    std::string dataDir() override { return o.services.dataDir; }
    uint32_t granted() override { return o.granted; }

    void fail(RunState s, const std::string& why) {
        { std::lock_guard<std::mutex> lk(mu); if (err.empty()) err = why; }
        state = (int)s;
        if (o.services.log) o.services.log(3, why);
        doneCv.notify_all();
    }
    bool ready() const { return state.load() == (int)RunState::Running; }
};

struct Guard {   // marks "inside plugin code" for the watchdog
    Core& c;
    explicit Guard(Core& x) : c(x) { c.callStart = nowNs(); }
    ~Guard() { c.callStart = 0; }
};

void callJob(Core& c, const Job& j) {
    const OnAirPluginVTable* v = c.lib.vt;
    auto has = [&](size_t off) { return v->struct_size >= off + sizeof(void*); };
    switch (j.k) {
    case Job::Config:
        if (has(offsetof(OnAirPluginVTable, configure)) && v->configure) v->configure(c.inst, j.a, j.b);
        break;
    case Job::Iq:
        if (has(offsetof(OnAirPluginVTable, process_iq)) && v->process_iq) {
            OnAirIqMeta m{sizeof m, j.meta.centreHz, j.meta.sampleRate, j.meta.timestamp, j.meta.firstSample};
            v->process_iq(c.inst, j.iq.data(), (uint32_t)(j.iq.size() / 2), &m);
        }
        break;
    case Job::Event:
        if (has(offsetof(OnAirPluginVTable, on_event)) && v->on_event) {
            std::vector<OnAirField> f;
            std::vector<std::string> keep;
            keep.reserve(j.ev.f.size());
            for (const auto& kv : j.ev.f) {
                if (f.size() >= 64) break;
                keep.push_back(kv.first);
                OnAirField x{};
                x.key = keep.back().c_str();
                if (kv.second.t == automation::Value::Str) { x.is_text = 1; x.s = kv.second.s.c_str(); }
                else if (kv.second.t == automation::Value::Bool) x.d = kv.second.b ? 1 : 0;
                else x.d = kv.second.n;
                f.push_back(x);
            }
            OnAirEvent e{sizeof e, j.ev.type.c_str(), j.ev.mode.c_str(), j.ev.time, j.ev.freqHz, (uint32_t)f.size(), f.data()};
            v->on_event(c.inst, &e);
        }
        break;
    case Job::Tick:
        if ((v->caps & ONAIR_CAP_TICK) && has(offsetof(OnAirPluginVTable, tick)) && v->tick) v->tick(c.inst, j.a);
        break;
    case Job::Setting:
        if ((v->caps & ONAIR_CAP_SETTINGS) && has(offsetof(OnAirPluginVTable, setting_changed)) && v->setting_changed) v->setting_changed(c.inst, j.key.c_str());
        break;
    }
}

void workerMain(std::shared_ptr<Core> c) {
    c->state = (int)RunState::Starting;
    try {
        Guard g(*c);
        c->inst = c->lib.vt->create(&c->api, c->o.modeId.c_str());
    } catch (...) { c->inst = nullptr; }
    if (c->abandoned) { c->workerDone = true; c->doneCv.notify_all(); return; }
    if (!c->inst) { c->fail(RunState::Failed, "the plugin refused to start (create returned nothing)"); c->workerDone = true; c->doneCv.notify_all(); return; }
    c->state = (int)RunState::Running;
    c->doneCv.notify_all();
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(c->mu);
            c->cv.wait(lk, [&] { return c->stopReq || c->abandoned || !c->jobs.empty(); });
            if (c->stopReq || c->abandoned) break;
            j = std::move(c->jobs.front());
            c->jobs.pop_front();
            if (j.k == Job::Iq) c->queuedBytes -= j.iq.size() * 4;
        }
        try {
            Guard g(*c);
            callJob(*c, j);
            if (j.k == Job::Iq) c->processed++;
        } catch (...) { c->fail(RunState::Failed, "the plugin threw an exception (C++ exceptions must not cross the plugin ABI)"); break; }
        if (c->abandoned) break;
    }
    if (!c->abandoned && c->inst) {
        try { Guard g(*c); c->lib.vt->destroy(c->inst); } catch (...) {}
    }
    c->inst = nullptr;
    if (c->state.load() == (int)RunState::Running) c->state = (int)RunState::Stopped;
    c->workerDone = true;
    c->doneCv.notify_all();
}

void watchdogMain(std::shared_ptr<Core> c) {
    const int64_t limit = (int64_t)(c->o.watchdogSeconds * 1e9);
    while (!c->workerDone.load()) {
        {
            std::unique_lock<std::mutex> lk(c->mu);
            c->doneCv.wait_for(lk, std::chrono::milliseconds(20), [&] { return c->workerDone.load(); });
        }
        const int64_t s = c->callStart.load();
        if (s && nowNs() - s > limit && !c->abandoned.load()) {
            c->abandoned = true;
            c->fail(RunState::Hung, "the plugin stopped answering for more than " + std::to_string((int)c->o.watchdogSeconds) + " s and was switched off");
            c->cv.notify_all();
            return;
        }
    }
}

class InProc : public Runner {
public:
    std::shared_ptr<Core> c;
    std::thread worker, dog;
    ~InProc() override { stop(); }

    void configure(double hz, double sr) override { Job j; j.k = Job::Config; j.a = hz; j.b = sr; push(std::move(j)); }
    void feedIq(const float* iq, size_t n, const IqMeta& meta) override {
        constexpr size_t kChunk = 262144;
        for (size_t o = 0; o < n; o += kChunk) {
            const size_t m = std::min(kChunk, n - o);
            Job j;
            j.k = Job::Iq;
            {
                std::lock_guard<std::mutex> lk(c->mu);
                if (c->state.load() != (int)RunState::Running || c->queuedBytes + m * 8 > kIqQueueBytes) { c->dropped++; return; }
                c->queuedBytes += m * 8;
            }
            j.iq.assign(iq + 2 * o, iq + 2 * (o + m));
            j.meta = meta;
            j.meta.firstSample += o;
            j.meta.timestamp += meta.sampleRate > 0 ? (double)o / meta.sampleRate : 0;
            { std::lock_guard<std::mutex> lk(c->mu); c->jobs.push_back(std::move(j)); }
            c->cv.notify_one();
        }
    }
    void feedEvent(const automation::Event& ev) override {
        Job j; j.k = Job::Event; j.ev = ev;
        std::lock_guard<std::mutex> lk(c->mu);
        if (c->state.load() != (int)RunState::Running || c->jobs.size() > 2000) { c->dropped++; return; }
        c->jobs.push_back(std::move(j));
        c->cv.notify_one();
    }
    void settingChanged(const std::string& key) override { Job j; j.k = Job::Setting; j.key = key; push(std::move(j)); }
    void tick(double now) override { Job j; j.k = Job::Tick; j.a = now; push(std::move(j)); }
    RunState state() const override { return (RunState)c->state.load(); }
    std::string lastError() const override { std::lock_guard<std::mutex> lk(c->mu); return c->err; }
    uint64_t droppedBlocks() const override { return c->dropped; }
    uint64_t processedBlocks() const override { return c->processed; }
    void stop() override {
        if (!c) return;
        c->stopReq = true;
        c->cv.notify_all();
        {
            std::unique_lock<std::mutex> lk(c->mu);
            c->doneCv.wait_for(lk, std::chrono::milliseconds((int)(c->o.watchdogSeconds * 1000) + 1000), [&] { return c->workerDone.load(); });
        }
        if (c->workerDone.load()) { if (worker.joinable()) worker.join(); if (dog.joinable()) dog.join(); }
        else {
            c->abandoned = true;   // stuck in the plugin: leave it, the shared state keeps itself alive
            if (c->state.load() != (int)RunState::Hung) c->fail(RunState::Hung, "the plugin did not stop in time and was abandoned");
            if (worker.joinable()) worker.detach();
            if (dog.joinable()) dog.join();
        }
        if (c->state.load() == (int)RunState::Running) c->state = (int)RunState::Stopped;
    }
private:
    void push(Job&& j) {
        std::lock_guard<std::mutex> lk(c->mu);
        if (c->state.load() != (int)RunState::Running && c->state.load() != (int)RunState::Starting) return;
        c->jobs.push_back(std::move(j));
        c->cv.notify_one();
    }
};

} // namespace

std::unique_ptr<Runner> startInProcess(const StartOptions& o, std::string& err) {
    if (!o.manifest || !o.results) { err = "bad start options"; return nullptr; }
    auto c = std::make_shared<Core>();
    c->o = o;
    c->man = *o.manifest;
    c->o.manifest = &c->man;
    if (!c->o.settings) c->o.settings = std::make_shared<SettingValues>(c->man);
    if (!loadLibrary(o.libraryPath, c->man, c->lib, err)) return nullptr;
    fillHostApi(c->api, c.get());
    auto r = std::make_unique<InProc>();
    r->c = c;
    r->worker = dect2::namedThread("onair-plg-wk", workerMain, c);
    r->dog = dect2::namedThread("onair-plg-dog", watchdogMain, c);
    // wait for create() to return (it is under the watchdog too)
    {
        std::unique_lock<std::mutex> lk(c->mu);
        c->doneCv.wait_for(lk, std::chrono::milliseconds((int)(o.watchdogSeconds * 1000) + 1500), [&] {
            const int s = c->state.load();
            return s == (int)RunState::Running || s == (int)RunState::Failed || s == (int)RunState::Hung;
        });
    }
    if (c->state.load() != (int)RunState::Running) {
        err = c->err.empty() ? "the plugin did not start" : c->err;
        r->stop();
        return nullptr;
    }
    return r;
}

} // namespace dect2::plugin
