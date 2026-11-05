// The out-of-process runner. A helper executable (onair-plugin-host) loads the library; the app sends it IQ, events, settings and ticks over a pipe and
// reads results back. Messages are [u32 length][u8 type][body] (results.h, Wire). The helper acknowledges every call; a call that is not acknowledged
// within the watchdog limit gets the helper killed (SIGKILL), and a crash of the plugin only ends the helper. Nothing but plain data crosses the pipe.
#include "dect2/common/thread_name.h"
#include "loader.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace dect2::plugin {

#ifdef _WIN32
std::unique_ptr<Runner> startProcess(const StartOptions&, std::string& err) { err = "the helper-process mode is not available on Windows yet"; return nullptr; }
int helperMain(const std::string&, int, int) { return 1; }
#else

namespace {

constexpr size_t kMaxFrame = 4u << 20;
using Clock = std::chrono::steady_clock;
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w; n -= (size_t)w;
    }
    return true;
}
bool readAll(int fd, uint8_t* p, size_t n) {
    while (n) {
        const ssize_t r = ::read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}
bool readFrame(int fd, Wire& type, std::vector<uint8_t>& body) {
    uint8_t h[4];
    if (!readAll(fd, h, 4)) return false;
    const uint32_t len = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
    if (len < 1 || len > kMaxFrame) return false;
    std::vector<uint8_t> b(len);
    if (!readAll(fd, b.data(), len)) return false;
    type = (Wire)b[0];
    body.assign(b.begin() + 1, b.end());
    return true;
}

// ---------------- parent side ----------------
class ProcRunner : public Runner {
public:
    StartOptions o;
    Manifest man;
    pid_t pid = -1;
    int toChild = -1, fromChild = -1;
    std::atomic<int> st{(int)RunState::Starting};
    std::atomic<bool> stopping{false}, joined{false};
    std::mutex mu;                       // queue, err, ack times
    std::condition_variable cv, readyCv;
    std::deque<std::vector<uint8_t>> out;
    size_t outBytes = 0;
    std::deque<int64_t> pending;         // send times of unacknowledged calls
    std::string err;
    std::atomic<uint64_t> dropped{0}, processed{0};
    std::thread writer, reader, dog;

    ~ProcRunner() override { stop(); }

    void fail(RunState s, const std::string& why) {
        { std::lock_guard<std::mutex> lk(mu); if (err.empty()) err = why; }
        st = (int)s;
        if (o.services.log) o.services.log(3, why);
        readyCv.notify_all();
    }
    void post(Wire t, const std::vector<uint8_t>& body, bool expectAck, bool droppable = false) {
        std::vector<uint8_t> m;
        wirePut(m, t, body);
        std::lock_guard<std::mutex> lk(mu);
        if (droppable && outBytes + m.size() > kIqQueueBytes) { dropped++; return; }
        outBytes += m.size();
        // the ack bookkeeping is done when the writer sends the message
        if (expectAck) m.push_back(0);   // marker byte, removed by the writer
        out.push_back(std::move(m));
        cv.notify_one();
    }
    void configure(double hz, double sr) override { WireWriter w; w.f64(hz); w.f64(sr); post(Wire::ToChildConfigure, w.b, true); }
    void feedIq(const float* iq, size_t n, const IqMeta& m) override {
        if (st.load() != (int)RunState::Running) { dropped++; return; }
        constexpr size_t kChunk = 65536;
        for (size_t off = 0; off < n; off += kChunk) {
            const size_t k = std::min(kChunk, n - off);
            WireWriter w;
            w.f64(m.centreHz); w.f64(m.sampleRate); w.f64(m.timestamp + (m.sampleRate > 0 ? (double)off / m.sampleRate : 0)); w.u64(m.firstSample + off); w.u32((uint32_t)k);
            const size_t o0 = w.b.size();
            w.b.resize(o0 + k * 8);
            memcpy(w.b.data() + o0, iq + 2 * off, k * 8);
            post(Wire::ToChildIq, w.b, true, true);
        }
    }
    void feedEvent(const automation::Event& ev) override {
        if (st.load() != (int)RunState::Running) return;
        WireWriter w;
        w.str(ev.type, 60); w.str(ev.mode, 60); w.f64(ev.time); w.f64(ev.freqHz);
        const uint32_t n = (uint32_t)std::min<size_t>(ev.f.size(), 64);
        w.u32(n);
        uint32_t i = 0;
        for (const auto& kv : ev.f) {
            if (i++ >= n) break;
            w.str(kv.first, 40);
            const bool text = kv.second.t == automation::Value::Str;
            w.u8(text); w.f64(kv.second.t == automation::Value::Bool ? (kv.second.b ? 1 : 0) : kv.second.n); w.str(text ? kv.second.s : std::string(), automation::kMaxFieldText);
        }
        post(Wire::ToChildEvent, w.b, true, true);
    }
    void settingChanged(const std::string& key) override {
        if (!o.settings) return;
        WireWriter w; w.str(key, 40); w.str(o.settings->text(key), 600);
        post(Wire::ToChildSetting, w.b, false);
    }
    void tick(double now) override { WireWriter w; w.f64(now); post(Wire::ToChildTick, w.b, true); }
    RunState state() const override { return (RunState)st.load(); }
    std::string lastError() const override { std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu)); return err; }
    uint64_t droppedBlocks() const override { return dropped; }
    uint64_t processedBlocks() const override { return processed; }

    void stop() override {
        if (joined.exchange(true)) return;
        stopping = true;
        if (pid > 0 && st.load() == (int)RunState::Running) { std::vector<uint8_t> m; wirePut(m, Wire::ToChildQuit, {}); std::lock_guard<std::mutex> lk(mu); out.push_front(std::move(m)); cv.notify_all(); }
        // give a healthy helper a moment to leave, then end it
        if (pid > 0) {
            for (int i = 0; i < 50; i++) { int s; if (waitpid(pid, &s, WNOHANG) == pid) { pid = -1; break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            if (pid > 0) { kill(pid, SIGKILL); int s; waitpid(pid, &s, 0); pid = -1; }
        }
        { std::lock_guard<std::mutex> lk(mu); cv.notify_all(); }
        if (toChild >= 0) { ::close(toChild); toChild = -1; }
        if (writer.joinable()) writer.join();
        if (reader.joinable()) reader.join();
        if (dog.joinable()) dog.join();
        if (fromChild >= 0) { ::close(fromChild); fromChild = -1; }
        if (st.load() == (int)RunState::Running || st.load() == (int)RunState::Starting) st = (int)RunState::Stopped;
    }

    void writerMain() {
        for (;;) {
            std::vector<uint8_t> m;
            bool ack = false;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stopping.load() || !out.empty(); });
                if (out.empty()) return;
                m = std::move(out.front());
                out.pop_front();
                // message length field says whether a marker byte was appended: frame length = 4 + len
                uint32_t len = m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t)m[3] << 24);
                if (m.size() == 4u + len + 1) { m.pop_back(); ack = true; }
                outBytes -= std::min(outBytes, m.size() + (ack ? 1 : 0));
                if (ack) pending.push_back(nowNs());
            }
            if (!writeAll(toChild, m.data(), m.size())) return;
        }
    }
    void readerMain() {
        Wire t; std::vector<uint8_t> b;
        while (readFrame(fromChild, t, b)) {
            switch (t) {
            case Wire::Hello: st = (int)RunState::Running; readyCv.notify_all(); break;
            case Wire::Fatal: { WireReader r(b.data(), b.size()); fail(RunState::Failed, r.str(500)); break; }
            case Wire::Ack: { std::lock_guard<std::mutex> lk(mu); if (!pending.empty()) pending.pop_front(); processed++; break; }
            case Wire::Log: { WireReader r(b.data(), b.size()); const int lv = (int)r.u32(); const std::string s = r.str(2000); if (r.ok && o.services.log) o.services.log(lv, s); break; }
            case Wire::Event: {
                WireReader r(b.data(), b.size());
                const std::string type = r.str(40);
                automation::Event ev;
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && i < 32 && r.ok; i++) {
                    const std::string k = r.str(40); const uint32_t text = r.u8(); const double d = r.f64(); const std::string s = r.str(automation::kMaxFieldText);
                    if (!r.ok || k.empty()) break;
                    if (text) ev.str(k, s); else ev.num(k, d);
                }
                if (r.ok && o.services.emitEvent) o.services.emitEvent(type, ev);
                break;
            }
            case Wire::Audio: {
                WireReader r(b.data(), b.size());
                const double rate = r.f64(); const uint32_t n = r.u32();
                // the same limits as cAudio (loader.cpp): the helper runs the plugin's code, so its messages are not trusted
                if (r.ok && (o.granted & PermAudioOut) && n > 0 && n <= (1u << 20) && rate >= 1000 && rate <= 400000 && r.pos + (size_t)n * 4 <= b.size() && o.services.audioOut) {
                    std::vector<float> pcm(n);
                    if (n) memcpy(pcm.data(), b.data() + r.pos, (size_t)n * 4);
                    o.services.audioOut(pcm.data(), n, rate);
                }
                break;
            }
            case Wire::RequestIq: { WireReader r(b.data(), b.size()); const double hz = r.f64(), sr = r.f64(); if (r.ok && o.services.requestIq) o.services.requestIq(hz, sr); break; }
            default: wireApply(*o.results, t, b.data(), b.size()); break;
            }
        }
        if (!stopping.load() && st.load() != (int)RunState::Failed && st.load() != (int)RunState::Hung) fail(RunState::Failed, "the plugin helper process ended unexpectedly (the plugin crashed); the app is not affected");
        readyCv.notify_all();
    }
    void dogMain() {
        const int64_t limit = (int64_t)(o.watchdogSeconds * 1e9);
        while (!stopping.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            bool hung = false;
            { std::lock_guard<std::mutex> lk(mu); hung = !pending.empty() && nowNs() - pending.front() > limit; }
            if (hung && pid > 0) {
                fail(RunState::Hung, "the plugin stopped answering for more than " + std::to_string((int)o.watchdogSeconds) + " s; its helper process was killed");
                kill(pid, SIGKILL);
                return;
            }
        }
    }
};

} // namespace

std::unique_ptr<Runner> startProcess(const StartOptions& o, std::string& err) {
    if (!o.manifest || !o.results) { err = "bad start options"; return nullptr; }
    if (o.hostExe.empty() || access(o.hostExe.c_str(), X_OK) != 0) { err = "the plugin helper (onair-plugin-host) was not found"; return nullptr; }
    signal(SIGPIPE, SIG_IGN);
    int inP[2], outP[2];
    if (pipe(inP) != 0) { err = "cannot create a pipe"; return nullptr; }
    if (pipe(outP) != 0) { ::close(inP[0]); ::close(inP[1]); err = "cannot create a pipe"; return nullptr; }
    const int devnull = open("/dev/null", O_RDWR);
    std::vector<std::string> args = {o.hostExe, "--plugin-host", o.libraryPath};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { err = "cannot start the helper process"; return nullptr; }
    if (pid == 0) {
        dup2(inP[0], 0);
        dup2(outP[1], 3);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        for (int fd = 4; fd < 256; fd++) ::close(fd);
        execv(argv[0], argv.data());
        _exit(127);
    }
    ::close(inP[0]); ::close(outP[1]); if (devnull >= 0) ::close(devnull);
    auto r = std::make_unique<ProcRunner>();
    r->o = o;
    r->man = *o.manifest;
    r->o.manifest = &r->man;
    if (!r->o.settings) r->o.settings = std::make_shared<SettingValues>(r->man);
    r->pid = pid; r->toChild = inP[1]; r->fromChild = outP[0];
    r->writer = dect2::namedThread("onair-plg-wr", [p = r.get()] { p->writerMain(); });
    r->reader = dect2::namedThread("onair-plg-rd", [p = r.get()] { p->readerMain(); });
    r->dog = dect2::namedThread("onair-plg-dog", [p = r.get()] { p->dogMain(); });
    // Init: plugin id, kind, mode id, permissions, data folder, then every setting
    {
        WireWriter w;
        w.str(r->man.id, 64); w.str(r->man.kind, 16); w.str(o.modeId, 32); w.u32(o.granted); w.str(o.services.dataDir, 1000);
        w.u32((uint32_t)r->man.settings.size());
        for (const auto& s : r->man.settings) { w.str(s.key, 40); w.str(r->o.settings->text(s.key), 600); }
        r->post(Wire::ToChildInit, w.b, false);
    }
    {
        std::unique_lock<std::mutex> lk(r->mu);
        r->readyCv.wait_for(lk, std::chrono::milliseconds((int)(o.watchdogSeconds * 1000) + 3000), [&] { return r->st.load() != (int)RunState::Starting; });
    }
    if (r->st.load() != (int)RunState::Running) {
        err = r->err.empty() ? "the plugin helper did not start" : r->err;
        r->stop();
        return nullptr;
    }
    return r;
}

// ---------------- child side ----------------
namespace {

class ChildSink : public ApiSink {
public:
    int fd;
    std::mutex wm;
    std::map<std::string, std::string> settings;
    uint32_t perms = 0;
    std::string dir;
    explicit ChildSink(int f) : fd(f) {}
    void send(Wire t, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> m;
        wirePut(m, t, body);
        std::lock_guard<std::mutex> lk(wm);
        if (!writeAll(fd, m.data(), m.size())) _exit(0);   // the app is gone
    }
    bool alive() override { return true; }
    void log(int level, const std::string& m) override { WireWriter w; w.u32((uint32_t)level); w.str(m, 2000); send(Wire::Log, w.b); }
    std::string settingText(const std::string& k, bool& found) override { auto it = settings.find(k); found = it != settings.end(); return found ? it->second : std::string(); }
    int requestIq(double hz, double sr) override { WireWriter w; w.f64(hz); w.f64(sr); send(Wire::RequestIq, w.b); return 0; }
    int tableDefine(const std::string& i, const std::string& t, const std::vector<Col>& c) override { send(Wire::TableDefine, wireTableDefine(i, t, c)); return 0; }
    int tableRow(const std::string& i, const std::vector<Cell>& c) override { send(Wire::TableRow, wireTableRow(i, c)); return 0; }
    int tableClear(const std::string& i) override { WireWriter w; w.str(i, kMaxIdLen); send(Wire::TableClear, w.b); return 0; }
    int status(const std::string& k, const std::string& t, int l) override { WireWriter w; w.str(k, kMaxIdLen); w.str(t, kMaxCellText); w.u32((uint32_t)l); send(Wire::Status, w.b); return 0; }
    int resultLog(const std::string& l) override { WireWriter w; w.str(l, kMaxLogLine); send(Wire::ResultLog, w.b); return 0; }
    int plot(const std::string& i, const std::string& t, int k, const float* d, size_t n, double x0, double dx) override { send(Wire::Plot, wirePlot(i, t, k, d, n, x0, dx)); return 0; }
    int image(const std::string& i, int f, uint32_t w, uint32_t h, const uint8_t* d, size_t n) override { if (n > kMaxImageBytes) return kLimit; send(Wire::Image, wireImage(i, f, w, h, d, n)); return 0; }
    int mapPoint(const std::string& i, const std::string& l, double la, double lo) override { WireWriter w; w.str(i, kMaxIdLen); w.str(l, 80); w.f64(la); w.f64(lo); send(Wire::MapPoint, w.b); return 0; }
    int emitEvent(const std::string& t, const automation::Event& e) override {
        WireWriter w; w.str(t, 40); w.u32((uint32_t)std::min<size_t>(e.f.size(), 32));
        uint32_t i = 0;
        for (const auto& kv : e.f) { if (i++ >= 32) break; const bool tx = kv.second.t == automation::Value::Str; w.str(kv.first, 40); w.u8(tx); w.f64(kv.second.n); w.str(tx ? kv.second.s : std::string(), automation::kMaxFieldText); }
        send(Wire::Event, w.b);
        return 0;
    }
    int audioOut(const float* p, size_t n, double r) override {
        WireWriter w; w.f64(r); w.u32((uint32_t)n);
        const size_t o = w.b.size(); w.b.resize(o + n * 4); memcpy(w.b.data() + o, p, n * 4);
        send(Wire::Audio, w.b);
        return 0;
    }
    std::string dataDir() override { return dir; }
    uint32_t granted() override { return perms; }
};

} // namespace

int helperMain(const std::string& libPath, int fdIn, int fdOut) {
    signal(SIGPIPE, SIG_IGN);
    ChildSink sink(fdOut);
    OnAirHostApi api;
    Lib lib;
    void* inst = nullptr;
    Wire t; std::vector<uint8_t> b;
    auto ack = [&] { sink.send(Wire::Ack, {}); };
    auto fatal = [&](const std::string& why) { WireWriter w; w.str(why, 500); sink.send(Wire::Fatal, w.b); };
    while (readFrame(fdIn, t, b)) {
        WireReader r(b.data(), b.size());
        switch (t) {
        case Wire::ToChildInit: {
            Manifest m;
            m.id = r.str(64); m.kind = r.str(16);
            const std::string mode = r.str(32);
            sink.perms = r.u32(); sink.dir = r.str(1000);
            const uint32_t ns = r.u32();
            for (uint32_t i = 0; i < ns && i < kMaxSettings && r.ok; i++) { const std::string k = r.str(40), v = r.str(600); sink.settings[k] = v; }
            if (!r.ok) { fatal("bad init message"); return 2; }
            std::string err;
            if (!loadLibrary(libPath, m, lib, err)) { fatal(err); return 3; }
            fillHostApi(api, &sink);
            inst = lib.vt->create(&api, mode.c_str());
            if (!inst) { fatal("the plugin refused to start (create returned nothing)"); return 4; }
            sink.send(Wire::Hello, {});
            break;
        }
        case Wire::ToChildSetting: {
            const std::string k = r.str(40), v = r.str(600);
            if (!r.ok) break;
            sink.settings[k] = v;
            if (inst && (lib.vt->caps & ONAIR_CAP_SETTINGS) && lib.vt->setting_changed) lib.vt->setting_changed(inst, k.c_str());
            break;
        }
        case Wire::ToChildConfigure: {
            const double hz = r.f64(), sr = r.f64();
            if (inst && r.ok && lib.vt->configure) lib.vt->configure(inst, hz, sr);
            ack();
            break;
        }
        case Wire::ToChildIq: {
            OnAirIqMeta m{sizeof m, r.f64(), r.f64(), r.f64(), r.u64()};
            const uint32_t n = r.u32();
            if (inst && r.ok && (size_t)n * 8 + r.pos <= b.size() && lib.vt->process_iq) {
                std::vector<float> iq(n * 2);
                if (n) memcpy(iq.data(), b.data() + r.pos, (size_t)n * 8);
                lib.vt->process_iq(inst, iq.data(), n, &m);
            }
            ack();
            break;
        }
        case Wire::ToChildEvent: {
            const std::string type = r.str(60), mode = r.str(60);
            const double time = r.f64(), hz = r.f64();
            const uint32_t n = r.u32();
            std::vector<std::string> keys, texts;
            std::vector<OnAirField> f;
            for (uint32_t i = 0; i < n && i < 64 && r.ok; i++) { keys.push_back(r.str(40)); const uint32_t tx = r.u8(); const double d = r.f64(); texts.push_back(r.str(automation::kMaxFieldText)); OnAirField x{}; x.is_text = (int32_t)tx; x.d = d; f.push_back(x); }
            for (size_t i = 0; i < f.size(); i++) { f[i].key = keys[i].c_str(); f[i].s = texts[i].c_str(); }
            if (inst && r.ok && lib.vt->on_event) { OnAirEvent e{sizeof e, type.c_str(), mode.c_str(), time, hz, (uint32_t)f.size(), f.data()}; lib.vt->on_event(inst, &e); }
            ack();
            break;
        }
        case Wire::ToChildTick: { const double now = r.f64(); if (inst && r.ok && (lib.vt->caps & ONAIR_CAP_TICK) && lib.vt->tick) lib.vt->tick(inst, now); ack(); break; }
        case Wire::ToChildQuit: if (inst) lib.vt->destroy(inst); return 0;
        default: break;
        }
    }
    if (inst) lib.vt->destroy(inst);
    return 0;
}

#endif

} // namespace dect2::plugin
