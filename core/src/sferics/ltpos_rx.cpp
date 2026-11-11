// Lightning position (network): the mode's receiver and its UDP transport (ltpos_rx.h).
#include "dect2/sferics/ltpos_rx.h"
#include "dect2/sferics/ltpos_core.h"
#include "dect2/common/thread_name.h"
#include "dect2/net/api_policy.h"
#include "dect2/source/hw_time.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

namespace dect2 {

using namespace ltpos;

namespace {
constexpr size_t kMinKeyLen = 12;
constexpr double kHwMarkEveryS = 0.5;
int64_t wallNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}

struct LtPosReceiver::Impl {
    mutable std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, centerHz = 0;
    const void* clockKey = nullptr;
    LtPosSettings s;
    StationCore core;
    bool coreOk = false;
    uint64_t lastHwSeq = 0;
    double lastHwS = -1e9;
    size_t sinceSnap = 0;
    uint64_t seq = 0;
    LtPosTelemetry snap;
    uint64_t sent = 0, sendErrors = 0;
    std::deque<LtStrikeRow> strikes;
    uint32_t pktSeq = 0;

    // network
    std::thread th;
    std::atomic<bool> run{false};
    sock_t sock = kBadSock;
    sockaddr_in to{};
    bool listening = false;
    std::string netStatus;
    std::deque<std::vector<uint8_t>> outbox;
    std::vector<uint8_t> key;
    std::unique_ptr<ServerCore> server;
    std::unique_ptr<apipol::RateLimiter> limiter;
    int64_t lastHello = 0;

    uint32_t stationId() const { return stationIdOf(s.name); }

    void rebuildCore() {
        coreOk = false;
        if (rate <= 0) return;
        StationCfg c;
        c.thresholdDb = s.thresholdDb;
        c.sysKind = s.sysKind;
        c.sysSigmaUs = s.sysSigmaUs;
        const RefTx* r = s.refId > 0 ? refById(s.refId) : nullptr;
        if (r) {
            const double off = r->freqHz - centerHz;
            if (std::fabs(off) < rate * 0.45) { c.refOn = true; c.ref = *r; c.refOffsetHz = off; }
        }
        core.configure(rate, c);
        coreOk = true;
        lastHwSeq = 0; lastHwS = -1e9;
    }

    StationHdr hdr(TimeSrc src, double sigmaNs) const {
        StationHdr h;
        h.lat = s.lat; h.lon = s.lon; h.altM = s.altM; h.src = src; h.clockSigmaNs = sigmaNs;
        h.flags = (uint8_t)((s.posSet ? 1 : 0) | (src == TimeSrc::CommonView ? 2 : 0));
        return h;
    }

    // ---- the network thread
    void stopThread() {   // without the lock: the thread takes it
        run = false;
        if (th.joinable()) th.join();
    }
    void closeNet() {
        if (sock != kBadSock) { sockClose(sock); sock = kBadSock; }
        listening = false;
        server.reset();
        limiter.reset();
        outbox.clear();
    }

    void openNet() {
        netStatus.clear();
        if (s.role == 0) return;
        if (s.key.size() < kMinKeyLen) { netStatus = "the network needs a key of at least 12 characters"; return; }
        if (s.name.empty()) { netStatus = "the network needs a station name"; return; }
        if (s.port <= 0 || s.port > 65535) { netStatus = "the port must be 1 to 65535"; return; }
        netInit();
        key = networkKey(s.key);
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == kBadSock) { netStatus = "cannot open a socket"; return; }
        if (s.role == 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (s.host.empty() || getaddrinfo(s.host.c_str(), std::to_string(s.port).c_str(), &hints, &res) != 0 || !res) {
                if (res) freeaddrinfo(res);
                netStatus = "cannot find the server " + s.host;
                sockClose(sock); sock = kBadSock;
                return;
            }
            std::memcpy(&to, res->ai_addr, sizeof to);
            freeaddrinfo(res);
            netStatus = "sending to " + s.host + ":" + std::to_string(s.port);
        } else {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons((uint16_t)s.port);
            if (bind(sock, (const sockaddr*)&a, sizeof a) != 0) {
                netStatus = "cannot listen on port " + std::to_string(s.port) + " (in use?)";
                sockClose(sock); sock = kBadSock;
                return;
            }
            listening = true;
            netStatus = "listening on port " + std::to_string(s.port);
            MatchOpts mo; mo.minStations = std::max(3, s.minStations);
            PropModel pm; pm.velocityFactor = s.velocityFactor; pm.hops = s.hops; pm.hopHeightKm = s.hopHeightKm;
            server = std::make_unique<ServerCore>(s.key, mo, pm);
            limiter = std::make_unique<apipol::RateLimiter>(200.0, 400.0, 512);
        }
        sockRecvTimeout(sock, 50);
        // numbers keep rising over restarts: tenths of a second since 2020 fit 32 bits for 13 years
        pktSeq = (uint32_t)(wallNs() / 100000000LL - 15778368000LL);
        run = true;
        th = dect2::namedThread("onair-ltpos", [this] { netLoop(); });
    }

    void sendHelloLocked(int64_t now) {
        if (!s.posSet) return;
        double sg, ppm, pps;
        const TimeSrc src = coreOk ? core.activeSource(sg, ppm, pps) : TimeSrc::None;
        Packet p;
        p.type = PktType::Hello; p.station = stationId(); p.seq = pktSeq++; p.sendNs = now; p.name = s.name;
        p.hdr = hdr(src, src == TimeSrc::None ? 0 : sg);
        if (s.role == 1) outbox.push_back(encodePacket(p, key));
        else if (server && s.serverIsStation) server->local(p, now);
    }

    void netLoop() {
        uint8_t buf[kMaxPacket + 64];
        int64_t lastSolve = 0;
        while (run) {
            const int64_t now = wallNs();
            if (s.role == 2) {
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const long long n = (long long)recvfrom(sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
                std::lock_guard<std::mutex> l(mu);
                if (n > 0 && server && limiter) {
                    char ip[32];
                    snprintf(ip, sizeof ip, "%u", (unsigned)from.sin_addr.s_addr);
                    if (limiter->allow(ip, (double)now * 1e-9) && (size_t)n <= kMaxPacket) server->ingest(buf, (size_t)n, now);
                }
                if (now - lastHello > 10000000000LL) { lastHello = now; sendHelloLocked(now); }
                if (server && now - lastSolve > 200000000LL) {
                    lastSolve = now;
                    for (const Strike& k : server->process(now)) {
                        strikes.push_front(strikeRow(k));
                        while (strikes.size() > kLtStrikesMax) strikes.pop_back();
                    }
                    snapshot();
                }
            } else {
                std::vector<std::vector<uint8_t>> q;
                {
                    std::lock_guard<std::mutex> l(mu);
                    if (now - lastHello > 10000000000LL) { lastHello = now; sendHelloLocked(now); }
                    q.assign(outbox.begin(), outbox.end());
                    outbox.clear();
                }
                for (const auto& p : q) {
                    const long long r = sockSendTo(sock, p.data(), p.size(), to);
                    std::lock_guard<std::mutex> l(mu);
                    if (r != (long long)p.size()) sendErrors++;
                }
                if (q.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    void route(Packet& p, int64_t now) {
        p.station = stationId();
        p.seq = pktSeq++;
        p.sendNs = now;
        if (s.role == 1 && run) {
            if (outbox.size() < 512) outbox.push_back(encodePacket(p, key));
            else sendErrors++;
        } else if (s.role == 2 && server && s.serverIsStation) server->local(p, now);
    }

    void snapshot() {
        LtPosTelemetry t;
        t.seq = ++seq;
        const bool haveSamples = coreOk && core.pulses().samples() > 0;
        t.role = s.role;
        t.state = !haveSamples ? 0 : !core.pulses().floorValid() ? 1 : (s.role != 0 && run) ? 3 : 2;
        t.pulses = coreOk ? core.pulseCount() : 0;
        t.snrDb = coreOk ? core.lastPeakDb() : 0;
        t.dataValid = t.pulses > 0;
        t.floorDb = coreOk ? (float)core.pulses().floorDb() : -200.f;
        t.thresholdDb = (float)s.thresholdDb;
        t.inputRate = rate; t.centerHz = centerHz;
        t.timeSec = coreOk ? core.sampleS() : 0;
        t.sent = sent; t.sendErrors = sendErrors;
        if (coreOk) {
            double sg = 0, ppm = 0, pps = 0;
            const TimeSrc a = core.activeSource(sg, ppm, pps);
            t.timeSource = (int)a; t.clockSigmaUs = (float)(sg / 1e3); t.ppm = ppm; t.ppmSigma = (float)pps; t.fine = fineTiming(a, sg);
            for (TimeSrc q : {TimeSrc::GpsHw, TimeSrc::Gnss, TimeSrc::CommonView, TimeSrc::Ptp, TimeSrc::Ntp, TimeSrc::System}) {
                const double v = core.sourceSigmaNs(q);
                t.srcSigmaUs.push_back(v < 0 ? -1.f : (float)(v / 1e3));
            }
            t.refOn = core.cfg().refOn;
            if (t.refOn) { t.refName = core.cfg().ref.name; t.refLocked = core.refTimer().locked(); t.refSnrDb = core.refTimer().lastSnrDb(); t.refSigmaUs = (float)(core.refTimer().lastSigmaS() * 1e6); t.refFreqErrHz = core.refTimer().freqErrHz(); }
        }
        t.listening = listening;
        t.netStatus = netStatus;
        if (s.role != 0 && !s.posSet) t.netStatus = "set this station's position first: nothing is sent without it";
        const int64_t now = wallNs();
        if (server) {
            t.packetsOk = server->ok(); t.packetsAuth = server->authFail(); t.packetsReplay = server->replays(); t.packetsBad = server->bad();
            const MatchStats& m = server->stats();
            t.groups = m.groups; t.fixes = m.fixes; t.rejected = m.rejected; t.unmatched = m.unmatched; t.coarseOnly = m.coarseOnly;
            for (const auto& kv : server->stations()) {
                const ServerStation& st = kv.second;
                LtStationRow r;
                r.id = st.id; r.name = st.name.empty() ? std::to_string(st.id) : st.name;
                r.lat = st.hdr.lat; r.lon = st.hdr.lon; r.timeSource = (int)st.hdr.src;
                r.sigmaUs = (float)(st.sigmaNs / 1e3); r.ageS = (float)((double)(now - st.lastNs) * 1e-9);
                r.detections = st.dets; r.used = st.used; r.fine = fineTiming(st.hdr.src, st.sigmaNs); r.tiedUtc = st.tied; r.corrUs = (float)(st.corrNs / 1e3);
                t.stations.push_back(r);
            }
        }
        t.blocksOk = s.role == 2 ? t.fixes : t.sent;
        t.blocksBad = t.packetsAuth + t.packetsReplay + t.packetsBad;
        t.strikes.assign(strikes.begin(), strikes.end());
        snap = std::move(t);
    }
};

LtPosReceiver::LtPosReceiver() : p_(std::make_unique<Impl>()) {}
LtPosReceiver::~LtPosReceiver() { p_->stopThread(); p_->closeNet(); }

void LtPosReceiver::configure(double rate) {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rate = rate;
    p_->rebuildCore();
    p_->snapshot();
}
void LtPosReceiver::setSignalOffset(double) {}
void LtPosReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (hz == p_->centerHz) return;
    p_->centerHz = hz;
    p_->rebuildCore();
}
bool LtPosReceiver::ready() const { std::lock_guard<std::mutex> l(p_->mu); return p_->rate > 0; }
void LtPosReceiver::reset() {
    std::lock_guard<std::mutex> l(p_->mu);
    p_->rebuildCore();
    p_->sinceSnap = 0;
    p_->snapshot();
}
void LtPosReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> l(p_->mu); p_->log = std::move(cb); }
void LtPosReceiver::setClockKey(const void* key) { std::lock_guard<std::mutex> l(p_->mu); p_->clockKey = key; }

void LtPosReceiver::apply(const LtPosSettings& s) {
    Impl& m = *p_;
    bool netSame, coreSame;
    {
        std::lock_guard<std::mutex> l(m.mu);
        netSame = m.s.role == s.role && m.s.host == s.host && m.s.port == s.port && m.s.key == s.key && m.s.name == s.name && m.s.velocityFactor == s.velocityFactor && m.s.hops == s.hops && m.s.hopHeightKm == s.hopHeightKm && m.s.minStations == s.minStations;
        coreSame = m.s.thresholdDb == s.thresholdDb && m.s.sysKind == s.sysKind && m.s.sysSigmaUs == s.sysSigmaUs && m.s.refId == s.refId;
    }
    if (!netSame) m.stopThread();   // joins the network thread: outside the lock
    std::lock_guard<std::mutex> l(m.mu);
    if (!netSame) m.closeNet();
    m.s = s;
    if (!coreSame) m.rebuildCore();
    if (!netSame) { m.lastHello = 0; m.openNet(); }
    m.snapshot();
}

void LtPosReceiver::addExternalMark(const TimeMark& mk) { std::lock_guard<std::mutex> l(p_->mu); if (p_->coreOk) p_->core.addMark(mk); }
void LtPosReceiver::clearStrikes() { std::lock_guard<std::mutex> l(p_->mu); p_->strikes.clear(); p_->snapshot(); }

void LtPosReceiver::feed(const std::complex<float>* x, size_t n) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> l(m.mu);
    if (!m.coreOk || n == 0) return;
    const int64_t now = wallNs();
    m.core.feed(x, n, now);
    if (m.s.useHwTime && m.clockKey) {   // the radio's own GPS-disciplined time, mapped through the count of samples handed on
        hwtime::Mark hm;
        uint64_t consumed = 0;
        if (hwtime::board().latest(m.clockKey, hm, consumed) && hm.seq != m.lastHwSeq && hm.srcRate > 0) {
            m.lastHwSeq = hm.seq;
            const double s = m.core.sampleS() + ((double)hm.srcIndex - (double)consumed) / hm.srcRate;
            if (s - m.lastHwS >= kHwMarkEveryS) {
                m.lastHwS = s;
                TimeMark t;
                t.src = TimeSrc::GpsHw; t.sampleS = s; t.utcNs = hm.utcNs; t.sigmaNs = 20; t.biasNs = 100 + 2e9 / m.rate;   // the receive chain's filters: a couple of samples
                m.core.addMark(t);
            }
        }
    }
    TimeSrc src = TimeSrc::None;
    double sg = 0;
    std::vector<DetRec> d = m.core.takeDetections(src, sg);
    if (!d.empty() && m.s.posSet && m.s.role != 0) {
        for (size_t i = 0; i < d.size(); i += kMaxDetPerPacket) {
            Packet p;
            p.type = PktType::Detections;
            p.hdr = m.hdr(src, sg);
            p.dets.assign(d.begin() + (long)i, d.begin() + (long)std::min(d.size(), i + kMaxDetPerPacket));
            m.route(p, now);
            m.sent += p.dets.size();
        }
    }
    RefRec rr;
    if (m.core.takeRef(rr) && m.s.posSet && m.s.role != 0) {
        double a = 0, b = 0, c = 0;
        const TimeSrc as = m.core.activeSource(a, b, c);
        Packet p;
        p.type = PktType::Reference;
        p.hdr = m.hdr(as, a);
        p.ref = rr;
        m.route(p, now);
    }
    m.sinceSnap += n;
    if (m.sinceSnap >= (size_t)(m.rate * 0.2) || m.snap.seq == 0) { m.sinceSnap = 0; m.snapshot(); }
}

bool LtPosReceiver::telemetry(LtPosTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> l(p_->mu);
    if (p_->snap.seq == 0 || p_->snap.seq == lastSeq) return false;
    out = p_->snap;
    return true;
}

ModeTuning ltposTuning() {
    ModeTuning t;
    t.stdMode = 792;
    t.id = "ltpos";
    t.name = "Lightning position (network)";
    t.minMhz = 0.003;
    t.maxMhz = 30.0;
    t.defMhz = 0.1;
    t.sampleRate = 1e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.5;
    t.minSampleRate = 100000;     // 10 us per sample: slower radios cannot time a leading edge usefully
    t.tuneOffsetHz = 0;
    t.notCircular = true;         // impulses, not a circular signal
    return t;
}

std::string ltposSummary(const LtPosTelemetry& t) {
    char b[200];
    if (t.state == 0) return "Lightning position: waiting for samples";
    if (t.state == 1) return "Lightning position: measuring the noise floor";
    const char* src = timeSrcName((TimeSrc)t.timeSource);
    if (t.role == 2) snprintf(b, sizeof b, "Lightning position: %llu strikes located, %zu stations, %llu sferics here (%s)", (unsigned long long)t.fixes, t.stations.size(), (unsigned long long)t.pulses, src);
    else if (t.role == 1) snprintf(b, sizeof b, "Lightning position: %llu detections sent, clock %s, %.3g us", (unsigned long long)t.sent, src, (double)t.clockSigmaUs);
    else snprintf(b, sizeof b, "Lightning position: %llu sferics timed, clock %s, %.3g us", (unsigned long long)t.pulses, src, (double)t.clockSigmaUs);
    return b;
}

}  // namespace dect2
