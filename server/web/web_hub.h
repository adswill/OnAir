// OnAir Web session manager ("hub"): many listeners, one capture.
//
//   pushIq()  (the radio's thread: a copy into a bounded queue, never waits)
//     -> DSP thread: SharedFft + Waterfall once, then every session's VirtualRx and every shared decoder on the worker threads
//        (fair: the starting point rotates every block, each item's CPU time is measured), audio frames and spectrum rows into the
//        sessions' outgoing queues (bounded: a slow listener loses spectrum rows first, then audio, and never holds anyone else up)
//   admit() / handle() / drain() / leave()  (the connection threads of web_server.cpp)
//   admin: usersJson(), kick(), statsJson(), the policy
//
// Limits: users (global and per address), per-session CPU budget, a global CPU cap, per-session and global bandwidth, decoders; the
// listener that would go over gets {"type":"error","code":"busy"} and the connection is closed (never a half-working session).
// Decoders are shared: listeners who subscribe to the same decoder kind on the same frequency (rounded to 1 kHz) share one instance.
#pragma once
#include "web_dsp.h"
#include "web_policy.h"
#include "web_ranges.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace onair_web {

struct HubLimits {
    int maxUsers = 8;
    int maxPerIp = 2;
    double cpuCapPct = 75;        // of the whole machine (all cores) for all web DSP together; new listeners are refused above it
    double sessionCpuPct = 25;    // of one core for one listener; over it the listener is slowed down, then disconnected
    int maxKbpsTotal = 8000;      // all listeners together (kbit/s)
    int maxKbpsUser = 512;        // one listener
    int maxDecoders = 8;          // shared decoder instances
    int idleMin = 30;             // no tuning or other action for this long: disconnected (0: never)
    int publicMaxMin = 0;         // session time limit for open access (0: none)
    int memberMaxMin = 0;         // for password / invite users (an invite's own limit wins)
    int maxFps = 15, maxBins = 2048;
    bool spectrum = true;         // web_spectrum=0: audio only, listeners get no spectrum / waterfall rows (the setup window's "audio only")
    int threads = 0;              // worker threads (0: automatic)
    size_t queueBytes = 384 * 1024;   // outgoing queue of one listener
};

// A decoder web listeners can subscribe to
class WebDecoder {
public:
    virtual ~WebDecoder() = default;
    virtual void feed(const cf32* /*x*/, size_t /*n*/) {}               // baseband at the factory's rate (rate 0: not called)
    virtual void poll(std::vector<std::string>& eventsJson) = 0;        // new results, each a complete JSON value (at most 64 KB each; bigger ones are not sent)
    virtual std::string policyKind() const { return std::string(); }    // the kind the policy checks now (the host decoder: the host's mode)
    virtual void snapshot(std::vector<std::string>& /*eventsJson*/) {}  // the current state for a listener who subscribes now (that listener only; same size limit as poll())
};
struct DecoderFactory {
    std::string kind, name;
    double rateHz = 0;            // baseband rate it wants; 0: it takes no samples (results from elsewhere, e.g. the host's own receiver)
    bool needsFreq = false;       // one instance per frequency
    std::function<std::unique_ptr<WebDecoder>(double freqHz)> make;
};

struct OutMsg { std::string data; bool binary = false; int prio = 0; };   // prio 0 control, 1 events, 2 audio, 3 spectrum

struct AdmitRequest { std::string ip; UserClass cls = UserClass::Public; std::string name, label; int maxMinutes = 0; };

class WebHub {
public:
    WebHub();
    ~WebHub();
    void setLimits(const HubLimits& l);
    HubLimits limits() const;
    // web_allow_tx: whether listeners may send transmit messages. The host plugs its transmitter in with setTxHandler (type, raw JSON, session id) -> reply text
    // (empty: nothing to say). Without a handler an allowed transmit message answers "this host has no transmitter for web listeners".
    bool allowTx() { std::lock_guard<std::mutex> g(polMu_); return policy_.allowTx(); }
    void setTxHandler(std::function<std::string(int, const std::string&, const std::string&)> h) { txHandler_ = std::move(h); }
    WebPolicy& policy() { return policy_; }   // guard with policyMutex() when changing it while running
    std::mutex& policyMutex() { return polMu_; }
    WebRanges& ranges() { return ranges_; }   // guard with policyMutex() when changing it while running; call rangesChanged() afterwards
    // After the host's frequency ranges changed: listeners get a "ranges" message, tunings and decoder subscriptions that are no longer allowed are
    // silenced or dropped (with a forbidden_range error), spectrum rows follow the new ranges. Call without holding policyMutex().
    void rangesChanged();
    void setLogIp(bool on) { logIp_ = on; }
    void setLog(std::function<void(const std::string&)> f) { log_ = std::move(f); }
    void registerDecoder(DecoderFactory f);
    void start();
    void stop();

    // The radio's thread: samples centred on centreHz at rateHz. Copies; drops (and counts) when the DSP thread is behind.
    void pushIq(const cf32* x, size_t n, double centreHz, double rateHz);
    // For tests and the load tool: run the DSP of everything queued now on the calling thread (after start() it takes turns with the
    // DSP thread)
    void processPending();

    // connections
    int admit(const AdmitRequest& r, std::string& code, std::string& why);   // > 0: the session id; else code / why say why
    void handle(int id, const ClientMsg& m);                                  // the replies go into the session's queue
    void error(int id, const std::string& code, const std::string& why);      // a text error into the session's queue
    // Takes the queued messages. False: the session has ended (kicked: reason set, the connection sends it and closes)
    bool drain(int id, std::vector<OutMsg>& out, std::string& kickReason);
    void leave(int id);
    void tick();                         // once a second: time limits, idle, budgets, bandwidth figures
    std::string welcomeJson(int id);     // the first message a session gets
    std::string infoJson() const;        // public server info (no addresses)

    // admin
    std::string usersJson() const;
    bool kick(int id, const std::string& reason);
    std::string statsJson() const;
    int userCount() const;
    void setServerInfo(const std::string& name, const std::string& location, const std::string& antenna);

private:
    struct Session;
    struct SharedDec;
    struct Pool;
    mutable std::mutex mu_;              // sessions_, decs_, capture config
    std::map<int, std::shared_ptr<Session>> sessions_;
    std::map<std::string, std::shared_ptr<SharedDec>> decs_;
    std::map<std::string, DecoderFactory> factories_;
    HubLimits lim_;
    std::atomic<size_t> qCap_{384 * 1024};   // lim_.queueBytes, read without the lock
    WebPolicy policy_;
    std::function<std::string(int, const std::string&, const std::string&)> txHandler_;
    WebRanges ranges_;                       // under polMu_
    std::atomic<unsigned> rangeGen_{1};      // bumped by rangesChanged(); sessions refresh their cached view when it differs
    std::mutex polMu_;
    std::atomic<bool> logIp_{false};
    std::function<void(const std::string&)> log_;
    int nextId_ = 1;
    std::string srvName_ = "OnAir Web", srvLoc_, srvAnt_;

    // capture (DSP thread)
    double centreHz_ = 0, rateHz_ = 0;
    std::atomic<double> pubCentre_{0}, pubRate_{0};
    SharedFft fft_;
    Waterfall wf_;
    uint64_t wfSeen_ = 0;
    double streamSec_ = 0;   // seconds of samples processed

    // ingest
    std::mutex inMu_;
    std::condition_variable inCv_;
    struct Chunk { std::vector<cf32> x; double centre = 0, rate = 0; };
    std::deque<Chunk> inQ_;
    size_t inSamples_ = 0;
    std::atomic<uint64_t> inDropped_{0};
    std::mutex procMu_;   // one processChunk() at a time: the pool runs one job, and the waterfall and stream clock are not shared

    std::thread dsp_;
    std::atomic<bool> run_{false};
    std::unique_ptr<Pool> pool_;
    unsigned rot_ = 0;

    // figures
    std::atomic<double> dspCpuPct_{0};   // all web DSP, % of the machine
    double cpuAcc_ = 0, cpuT0_ = 0;
    std::atomic<uint64_t> rejected_{0}, kicked_{0}, admitted_{0};
    double kbpsTotal_ = 0, lastTick_ = 0, overSince_ = 0;
    std::atomic<uint64_t> workerCpuUs_{0};
    std::thread::id dspId_;
    unsigned cores_ = 1;

    void dspLoop();
    void processChunk(Chunk& c);
    void reconfigureCapture(double centre, double rate);   // DSP thread
    void configureRx(Session& s);                           // under s.m
    void enqueue(Session& s, OutMsg m);
    void sendError(Session& s, const std::string& code, const std::string& why);
    void sendState(Session& s);
    void refreshView(Session& s);                           // under s.m: the session's cached RangeView follows rangeGen_
    void sendRanges(Session& s);                            // the "ranges" message (under s.m)
    void subscribe(const std::shared_ptr<Session>& s, const ClientMsg& m);
    void unsubscribe(Session& s, const std::string& key);
    void dropSession(int id, const std::string& reason);    // under mu_
    void logLine(const std::string& l) { if (log_) log_(l); }
    std::string ipShown(const std::string& ip) const;
};

double threadCpuSeconds();   // CPU time of the calling thread
double wallSeconds();

} // namespace onair_web
