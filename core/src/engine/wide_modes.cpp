// Whole-span decoding for the narrowband channel modes: see wide_modes.h.
#include "dect2/engine/wide_modes.h"
#include "dect2/channels/freq_db.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/fusion/fusion_rx.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/dstar/dstar_tel.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/t2/t2rx.h"
#include "dect2/engine/wide_bank.h"
#include <algorithm>
#include <cstdio>
#include <map>

namespace dect2 {

namespace {

constexpr size_t kLogCap = 200;
constexpr double kPreRoll = 0.5;   // a carrier found late gets the half second before it was found: its decoder sees the start of the transmission

// newest first lists: stamp, append, order by start time, cap
template <class T> void appendNewestFirst(std::vector<T>& d, const std::vector<T>& s, double hz) {
    for (T c : s) { c.carrierId = 1; c.carrierHz = hz; d.push_back(std::move(c)); }
    std::stable_sort(d.begin(), d.end(), [](const T& a, const T& b) { return a.wallTime > b.wallTime; });
    if (d.size() > kLogCap) d.resize(kLogCap);
}

std::string fmt(const char* f, int v) { char b[48]; snprintf(b, sizeof b, f, v); return b; }

template <class Rx> void logSetup(Rx& r, const std::function<void(const std::string&)>& log) {
    if (log) r.setLogCallback([log](const std::string& line) { if (!WideLogCapture::sink(line)) log(line); });   // inside a parallel feed the line is held and logged in carrier order
}
template <class Rx> void commonSetup(Rx& r, const std::function<void(const std::string&)>& log) { r.setSilent(true); logSetup(r, log); }

template <class Rx> void audioSwitch(Rx& r, bool play) { r.setSilent(!play); r.setMuted(!play); }
// clearResults where the receiver has one (Fusion keeps no lists)
template <class Rx> auto clearIf(Rx& r, int) -> decltype(r.clearResults(), void()) { r.clearResults(); }
template <class Rx> void clearIf(Rx&, long) {}

template <class Bank, class Tel> bool fillTel(Bank& b, uint64_t& lastSeq, RxTelemetry& t, int stdm, Tel& slot, bool ready) {
    Tel mt; uint64_t seq = 0;
    if (!b.snapshot(mt, t.wide, lastSeq, &seq, &t.wideStatus)) return false;
    lastSeq = seq;
    t.standard = stdm; t.seq = seq; t.state = mt.state; t.cfoHz = mt.cfoHz; t.dataValid = mt.dataValid; t.dataSnrDb = mt.snrDb;
    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad; t.rateOk = ready;
    slot = std::move(mt);
    return true;
}

// The mode-independent face of one bank
struct BankBase {
    virtual ~BankBase() = default;
    virtual void configure(double rate, const CarrierCfg& c) = 0;
    virtual void reset() = 0;
    virtual void clearResults() = 0;
    virtual void feed(const cf32* x, size_t n) = 0;
    virtual bool telemetry(RxTelemetry& t, uint64_t& lastSeq, int stdm) = 0;
    virtual void setSelected(int id) = 0;
    virtual void setMuted(bool m) = 0;
    virtual void setCenterHz(double hz) = 0;
    virtual size_t liveCount() const = 0;
    virtual std::vector<double> liveHz() const = 0;
    virtual bool ready() const = 0;
    virtual void setThresholds(double on, double off, double minSnr, double dwell, double hold) = 0;
    virtual void setMaxCarriers(int n) = 0;
    virtual void setCpuBudget(double f) = 0;
    virtual void setPriorsAbs(std::vector<double> hz, bool only) = 0;
    virtual std::string startRecording(int id, const std::string& dir) = 0;
    virtual void stopRecording(int id) = 0;
    virtual void setLoadOverride(double v) = 0;
    double rate = 0;
};

template <class Rx, class Tel> struct Bank : BankBase {
    WideBank<Rx, Tel> b;
    Tel RxTelemetry::*slot;
    Bank(WideOps<Rx, Tel> ops, Tel RxTelemetry::*s) : b(std::move(ops)), slot(s) {}
    void configure(double r, const CarrierCfg& c) override { rate = r; b.configure(r, c); }
    void reset() override { b.reset(); }
    void clearResults() override { b.clearResults(); }
    void feed(const cf32* x, size_t n) override { b.feed(x, n); }
    bool telemetry(RxTelemetry& t, uint64_t& lastSeq, int stdm) override { return fillTel(b, lastSeq, t, stdm, t.*slot, b.ready()); }
    void setSelected(int id) override { b.setSelected(id); }
    void setMuted(bool m) override { b.setMuted(m); }
    void setCenterHz(double hz) override { b.setCenterHz(hz); }
    size_t liveCount() const override { return b.liveCount(); }
    std::vector<double> liveHz() const override { return b.liveHz(); }
    bool ready() const override { return b.ready(); }
    void setThresholds(double on, double off, double minSnr, double dwell, double hold) override { b.setThresholds(on, off, minSnr, dwell, hold); }
    void setMaxCarriers(int n) override { b.setMaxCarriers(n); }
    void setCpuBudget(double f) override { b.setCpuBudget(f); }
    void setPriorsAbs(std::vector<double> hz, bool only) override { b.setPriorsAbs(std::move(hz), only); }
    std::string startRecording(int id, const std::string& dir) override { return b.startRecording(id, dir); }
    void stopRecording(int id) override { b.stopRecording(id); }
    void setLoadOverride(double v) override { b.setLoadOverride(v); }
};

} // namespace

struct WideModes::Impl {
    std::function<void(const std::string&)> log;
    std::map<int, std::unique_ptr<BankBase>> banks;
    WideBank<DmrReceiver, DmrTelemetry>* dmr = nullptr;
    WideBank<M17Receiver, M17Telemetry>* m17 = nullptr;
    WideBank<DstarReceiver, DstarTelemetry>* dstar = nullptr;
    WideBank<NxdnReceiver, NxdnTelemetry>* nxdn = nullptr;
    WideBank<DpmrReceiver, DpmrTelemetry>* dpmr = nullptr;
    WideBank<P25Receiver, P25Telemetry>* p25 = nullptr;
    WideBank<PagerReceiver, PagerTelemetry>* pager = nullptr;
    WideBank<PacketReceiver, PacketTelemetry>* packet = nullptr;
    WideBank<FusionReceiver, FusionTelemetry>* fusion = nullptr;
    bool pagerNarrow = false, packetNarrow = false;   // the single receivers' channel width, copied to every carrier receiver (setNarrowChannels)
    int packetFix = 0, packetEmph = 0;                // setPacketOptions: the packet receiver's fix bits and emphasis, for every carrier receiver
    int pagerProtos = 3, pagerBauds = 7;   // the single receiver's settings, copied to every carrier's receiver (setPagerOptions)
    bool muted = false;
    float volume = 1.f;
    int sel = 0;
    WideTuning tuning = [] { WideTuning t; t.cpuBudget = 0; return t; }();   // no CPU limit until the engine sets one (tests run faster than real time)
    double centerHz = 0;
    std::vector<double> extraPriors;
    bool followOnly = false;            // setFollowOnly: only these channels are decoded (a trunking follower's granted calls)
    std::vector<double> followHz;

    template <class Rx, class Tel> WideBank<Rx, Tel>* put(int m, WideOps<Rx, Tel> ops, Tel RxTelemetry::*slot) {
        auto* bk = new Bank<Rx, Tel>(std::move(ops), slot);
        banks[m].reset(bk);
        return &bk->b;
    }
    template <class Rx, class Tel> WideOps<Rx, Tel> baseOps(double outRate) {
        // every single-channel receiver does its own channel filtering and decimation to 48 kHz, and some refuse an input below 1 Msps (DMR) or 250 ksps:
        // the carrier is handed over at that low rate, mixed to 0 Hz, with only the aliases of the narrowing removed here
        WideOps<Rx, Tel> o;
        o.outRate = outRate; o.occupiedHz = 60000; o.preRollSec = kPreRoll;
        o.log = [this](const std::string& line) { if (log) log(line); };
        o.setup = [this](Rx& r) { commonSetup(r, log); r.setVolume(volume); };
        o.audio = [](Rx& r, bool play) { audioSwitch(r, play); };
        o.clear = [](Rx& r) { clearIf(r, 0); };
        return o;
    }
    // the trunking control channels: no sound, no setSilent
    template <class Rx, class Tel> WideOps<Rx, Tel> dataOps(double outRate) {
        WideOps<Rx, Tel> o;
        o.outRate = outRate; o.occupiedHz = 30000; o.preRollSec = kPreRoll;
        o.log = [this](const std::string& line) { if (log) log(line); };
        o.setup = [this](Rx& r) { logSetup(r, log); };
        o.clear = [](Rx& r) { clearIf(r, 0); };
        return o;
    }
    // the known channels of every bank inside its span
    void priors() {
        const std::vector<FreqEntry>& db = builtinFrequencies();
        for (auto& [m, bk] : banks) {
            std::vector<double> v;
            if (followOnly) {   // nothing but the channels asked for (none: no carrier at all)
                for (double f : followHz) if (bk->rate > 0 && std::fabs(f - centerHz) < bk->rate / 2) v.push_back(f);
                bk->setPriorsAbs(std::move(v), true);
                continue;
            }
            if (tuning.usePriors && bk->rate > 0 && centerHz > 0) {
                const std::vector<std::string> ids = freqDbModes(m);
                for (const FreqEntry& e : db)
                    if (std::find(ids.begin(), ids.end(), e.mode) != ids.end() && std::fabs((double)e.hz - centerHz) < bk->rate / 2) v.push_back((double)e.hz);
                for (double f : extraPriors) if (std::fabs(f - centerHz) < bk->rate / 2) v.push_back(f);
            }
            bk->setPriorsAbs(std::move(v), tuning.usePriors && tuning.priorsOnly);
        }
    }
    void applyTuning(int m, BankBase& bk) {
        const CarrierCfg c = cfgFor(m);
        bk.setThresholds(tuning.onDb, tuning.offDb, tuning.minSnrDb, tuning.dwellSec >= 0 ? tuning.dwellSec : c.debounceSec, tuning.holdSec >= 0 ? tuning.holdSec : c.holdSec);
        bk.setMaxCarriers(tuning.maxCarriers);
        bk.setCpuBudget(tuning.cpuBudget);
    }
    Impl() {
        auto d = baseOps<DmrReceiver, DmrTelemetry>(1.2e6);
        d.clearLists = [](DmrTelemetry& t) { t.callLog.clear(); t.messages.clear(); };
        d.append = [](DmrTelemetry& t, const DmrTelemetry& s, double hz) {
            for (DmrCall c : s.callLog) { c.carrierId = 1; c.carrierHz = hz; t.callLog.push_back(c); }
            for (DmrMessage m : s.messages) { m.carrierId = 1; m.carrierHz = hz; t.messages.push_back(m); }
            if (t.callLog.size() > kLogCap) t.callLog.erase(t.callLog.begin(), t.callLog.end() - (long)kLogCap);
            if (t.messages.size() > kLogCap) t.messages.erase(t.messages.begin(), t.messages.end() - (long)kLogCap);
        };
        d.info = [](const DmrTelemetry& t) { return t.cc >= 0 ? fmt("CC %d", t.cc) + (t.link.empty() ? "" : " " + t.link) : t.link; };
        dmr = put(10, d, &RxTelemetry::dmr);

        auto m = baseOps<M17Receiver, M17Telemetry>(300000);
        m.clearLists = [](M17Telemetry& t) { t.log.clear(); };
        m.append = [](M17Telemetry& t, const M17Telemetry& s, double hz) { appendNewestFirst(t.log, s.log, hz); };
        m.info = [](const M17Telemetry& t) { return t.log.empty() ? std::string() : t.log.front().src; };
        m17 = put(32, m, &RxTelemetry::m17);

        auto s = baseOps<DstarReceiver, DstarTelemetry>(300000);
        s.clearLists = [](DstarTelemetry& t) { t.log.clear(); };
        s.append = [](DstarTelemetry& t, const DstarTelemetry& x, double hz) { appendNewestFirst(t.log, x.log, hz); };
        s.info = [](const DstarTelemetry& t) { return t.log.empty() ? std::string() : t.log.front().my; };
        dstar = put(34, s, &RxTelemetry::dstar);

        auto n = baseOps<NxdnReceiver, NxdnTelemetry>(300000);
        n.clearLists = [](NxdnTelemetry& t) { t.log.clear(); };
        n.append = [](NxdnTelemetry& t, const NxdnTelemetry& s2, double hz) { appendNewestFirst(t.log, s2.log, hz); };
        n.info = [](const NxdnTelemetry& t) { return t.ran >= 0 ? fmt("RAN %d", t.ran) : std::string(); };
        nxdn = put(39, n, &RxTelemetry::nxdn);

        auto p = baseOps<DpmrReceiver, DpmrTelemetry>(300000);
        p.clearLists = [](DpmrTelemetry& t) { t.log.clear(); };
        p.append = [](DpmrTelemetry& t, const DpmrTelemetry& s2, double hz) { appendNewestFirst(t.log, s2.log, hz); };
        p.info = [](const DpmrTelemetry& t) { return t.cc >= 0 ? fmt("CC %d", t.cc) : std::string(); };
        dpmr = put(46, p, &RxTelemetry::dpmr);

        auto q = baseOps<P25Receiver, P25Telemetry>(300000);
        q.clearLists = [](P25Telemetry& t) { t.log.clear(); t.tsbks.clear(); };
        q.append = [](P25Telemetry& t, const P25Telemetry& s2, double hz) {
            appendNewestFirst(t.log, s2.log, hz);
            // the control channel's messages of every carrier, oldest first by their process-wide number
            t.tsbks.insert(t.tsbks.end(), s2.tsbks.begin(), s2.tsbks.end());
            std::sort(t.tsbks.begin(), t.tsbks.end(), [](const P25TsbkRec& a, const P25TsbkRec& b) { return a.seq < b.seq; });
            t.tsbks.erase(std::unique(t.tsbks.begin(), t.tsbks.end(), [](const P25TsbkRec& a, const P25TsbkRec& b) { return a.seq == b.seq; }), t.tsbks.end());
            if (t.tsbks.size() > kP25TsbkMax) t.tsbks.erase(t.tsbks.begin(), t.tsbks.end() - (long)kP25TsbkMax);
        };
        q.info = [](const P25Telemetry& t) { return t.nac >= 0 ? fmt("NAC %03X", t.nac) : std::string(); };
        p25 = put(51, q, &RxTelemetry::p25);

        // pagers: one 25 kHz channel each; the carrier is brought to 192 ksps (the receiver's own front end takes it from there). Pages come in bursts,
        // so a carrier found late gets the half second before it was found, and a carrier stays for half a minute after its last burst.
        WideOps<PagerReceiver, PagerTelemetry> g;
        g.outRate = 192000; g.occupiedHz = 24000; g.preRollSec = kPreRoll;
        g.setup = [this](PagerReceiver& r) { r.setWideInput(true); r.setProtocols(pagerProtos); r.setPocsagBauds(pagerBauds); r.setNarrowChannel(pagerNarrow); };
        g.clear = [](PagerReceiver& r) { r.clearResults(); };
        g.clearLists = [](PagerTelemetry& t) {
            t.messages.reset(); t.messagesTotal = 0; t.blocksOk = t.blocksBad = 0; t.signal = t.sync = false;
            for (auto& st : t.speeds) st = PagerSpeedStat();
        };
        g.hasLog = [](const PagerTelemetry& t) { return t.messages && !t.messages->empty(); };
        g.append = [](PagerTelemetry& t, const PagerTelemetry& x, double hz) {
            t.messagesTotal += x.messagesTotal; t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad;
            t.signal = t.signal || x.signal; t.sync = t.sync || x.sync;
            for (int i = 0; i < kPagerSpeeds; i++) {
                t.speeds[i].ok += x.speeds[i].ok; t.speeds[i].fixed += x.speeds[i].fixed; t.speeds[i].failed += x.speeds[i].failed;
                t.speeds[i].messages += x.speeds[i].messages; t.speeds[i].transmissions += x.speeds[i].transmissions;
            }
            if (!x.messages || x.messages->empty()) return;
            auto v = std::make_shared<std::vector<PagerMessage>>(t.messages ? *t.messages : std::vector<PagerMessage>());
            for (PagerMessage m2 : *x.messages) { m2.carrierId = 1; m2.carrierHz = hz; v->push_back(std::move(m2)); }
            std::stable_sort(v->begin(), v->end(), [](const PagerMessage& a, const PagerMessage& b) { return a.wallTime > b.wallTime; });
            if (v->size() > 2000) v->resize(2000);
            t.messages = std::move(v);
        };
        g.freqCorrection = [](const PagerTelemetry& t) { return t.cfoHz; };   // the receiver's carrier offset (its AFC measures it on every transmission)
        g.rowState = [](const PagerTelemetry& t) { return t.state == 1 ? 2 : 0; };
        g.info = [](const PagerTelemetry& t) {
            if (t.lastSpeed < 0 && !t.messagesTotal) return std::string();
            char b[80]; snprintf(b, sizeof b, "%s, %llu pages", t.lastSpeed >= 0 ? pagerSpeedName(t.lastSpeed) : "no sync", (unsigned long long)t.messagesTotal);
            return std::string(b);
        };
        pager = put(24, g, &RxTelemetry::pager);

        // APRS / packet: one 25 kHz FM channel each (144.800, 144.390 and the neighbours inside one capture); no sound from the carrier receivers
        WideOps<PacketReceiver, PacketTelemetry> k;
        k.outRate = 192000; k.occupiedHz = 24000; k.preRollSec = kPreRoll;
        k.setup = [this](PacketReceiver& r) { r.setWideInput(true); r.setSilent(true); r.setNarrowChannel(packetNarrow); r.setFixBits(packetFix); r.setEmphasis(packetEmph); };
        k.clear = [](PacketReceiver& r) { r.clearResults(); };
        k.clearLists = [](PacketTelemetry& t) {
            t.frames.clear(); t.stations.clear(); t.blocksOk = t.blocksBad = 0; t.ok1200 = t.bad1200 = t.ok9600 = t.bad9600 = t.aprsFrames = 0; t.carrier = false;
        };
        k.hasLog = [](const PacketTelemetry& t) { return !t.frames.empty() || !t.stations.empty(); };
        k.append = [](PacketTelemetry& t, const PacketTelemetry& x, double hz) {
            const double shift = t.timeSec - x.timeSec;   // the carriers' signal clocks started at different moments: bring x onto the primary's
            t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad; t.ok1200 += x.ok1200; t.bad1200 += x.bad1200; t.ok9600 += x.ok9600; t.bad9600 += x.bad9600;
            t.aprsFrames += x.aprsFrames; t.carrier = t.carrier || x.carrier;
            t.last1200Sec = std::max(t.last1200Sec, x.last1200Sec + shift); t.last9600Sec = std::max(t.last9600Sec, x.last9600Sec + shift); t.lastAprsSec = std::max(t.lastAprsSec, x.lastAprsSec + shift);
            for (PacketFrameInfo f : x.frames) { f.timeSec += shift; f.carrierId = 1; f.carrierHz = hz; t.frames.push_back(std::move(f)); }
            std::stable_sort(t.frames.begin(), t.frames.end(), [](const PacketFrameInfo& a, const PacketFrameInfo& b) { return a.timeSec < b.timeSec; });
            if (t.frames.size() > 200) t.frames.erase(t.frames.begin(), t.frames.end() - 200);
            for (PacketStation s2 : x.stations) {   // one row per callsign: the newest hearing, the frames of all
                s2.lastHeardSec += shift; s2.carrierId = 1; s2.carrierHz = hz;
                auto it = std::find_if(t.stations.begin(), t.stations.end(), [&](const PacketStation& q2) { return q2.call == s2.call; });
                if (it == t.stations.end()) t.stations.push_back(std::move(s2));
                else { const uint32_t n2 = it->count + s2.count; if (s2.lastHeardSec > it->lastHeardSec) *it = std::move(s2); it->count = n2; }
            }
            std::stable_sort(t.stations.begin(), t.stations.end(), [](const PacketStation& a, const PacketStation& b) { return a.lastHeardSec > b.lastHeardSec; });
            if (t.stations.size() > 300) t.stations.resize(300);
        };
        k.info = [](const PacketTelemetry& t) {
            if (!t.blocksOk) return std::string();
            char b[80]; snprintf(b, sizeof b, "%s%s, %llu frames", t.ok1200 ? "1200" : "", t.ok9600 ? (t.ok1200 ? " + 9600" : "9600") : "", (unsigned long long)t.blocksOk);
            return std::string(b);
        };
        packet = put(25, k, &RxTelemetry::packet);

        // MPT1327 control channels (12.5 kHz FFSK): every system's control channel in the span; the message rows of all, oldest first
        auto mp = dataOps<MptReceiver, MptTelemetry>(192000);
        mp.clearLists = [](MptTelemetry& t) { t.rows.clear(); t.channels.clear(); t.good = t.bad = t.messages = 0; t.blocksOk = t.blocksBad = 0; };
        mp.hasLog = [](const MptTelemetry& t) { return !t.rows.empty(); };
        mp.append = [](MptTelemetry& t, const MptTelemetry& x, double hz) {
            t.good += x.good; t.bad += x.bad; t.messages += x.messages; t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad;
            for (MptMessageRow r : x.rows) { r.carrierId = 1; r.carrierHz = hz; t.rows.push_back(std::move(r)); }
            std::stable_sort(t.rows.begin(), t.rows.end(), [](const MptMessageRow& a, const MptMessageRow& b) { return a.t < b.t; });
            if (t.rows.size() > kLogCap) t.rows.erase(t.rows.begin(), t.rows.end() - (long)kLogCap);
            for (const MptChannelRow& c : x.channels) {
                auto it = std::find_if(t.channels.begin(), t.channels.end(), [&](const MptChannelRow& q2) { return q2.channel == c.channel; });
                if (it == t.channels.end()) t.channels.push_back(c); else { it->count += c.count; if (c.lastT > it->lastT) { it->lastT = c.lastT; it->called = c.called; it->caller = c.caller; } }
            }
            std::sort(t.channels.begin(), t.channels.end(), [](const MptChannelRow& a, const MptChannelRow& b) { return a.channel < b.channel; });
        };
        mp.info = [](const MptTelemetry& t) { return t.sysId >= 0 ? fmt("SYS %04X", t.sysId) + fmt(", %d msgs", (int)t.messages) : std::string(); };
        put(128, mp, &RxTelemetry::mpt1327);

        // Yaesu Fusion: the callsigns of every carrier; the sound of the one listened to
        auto fu = baseOps<FusionReceiver, FusionTelemetry>(300000);
        fu.hasLog = [](const FusionTelemetry&) { return false; };
        fu.info = [](const FusionTelemetry& t) { return t.src.empty() ? std::string() : t.src + (t.dest.empty() ? "" : " > " + t.dest); };
        fusion = put(129, fu, &RxTelemetry::fusion);

        // LTR control data (sub-audible, on every repeater of the site): one carrier per repeater, the messages of all
        auto lt = dataOps<LtrReceiver, LtrTelemetry>(300000);
        lt.clear = [](LtrReceiver& r) { r.clearMessages(); };
        lt.clearLists = [](LtrTelemetry& t) { t.messages.clear(); t.repeaters.clear(); t.blocksOk = t.blocksBad = t.syncs = 0; };
        lt.hasLog = [](const LtrTelemetry& t) { return !t.messages.empty(); };
        lt.append = [](LtrTelemetry& t, const LtrTelemetry& x, double hz) {
            t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad; t.syncs += x.syncs;
            appendNewestFirst(t.messages, x.messages, hz);
            for (const LtrRepeater& r : x.repeaters) {
                auto it = std::find_if(t.repeaters.begin(), t.repeaters.end(), [&](const LtrRepeater& q2) { return q2.ch == r.ch; });
                if (it == t.repeaters.end()) t.repeaters.push_back(r); else { it->asGoto += r.asGoto; it->asHome += r.asHome; it->asFree += r.asFree; it->lastSec = std::max(it->lastSec, r.lastSec); }
            }
            std::sort(t.repeaters.begin(), t.repeaters.end(), [](const LtrRepeater& a, const LtrRepeater& b) { return a.ch < b.ch; });
        };
        lt.info = [](const LtrTelemetry& t) { return t.area >= 0 ? fmt("area %d", t.area) + fmt(", %d words", (int)t.blocksOk) : std::string(); };
        put(130, lt, &RxTelemetry::ltr);

        // EDACS control channels (9600 bd): the frames of all
        auto ed = dataOps<EdacsReceiver, EdacsTelemetry>(307200);
        ed.clear = [](EdacsReceiver& r) { r.clearMessages(); };
        ed.clearLists = [](EdacsTelemetry& t) { t.frames.clear(); t.words.clear(); t.blocksOk = t.blocksBad = t.syncs = 0; };
        ed.hasLog = [](const EdacsTelemetry& t) { return !t.frames.empty(); };
        ed.append = [](EdacsTelemetry& t, const EdacsTelemetry& x, double hz) {
            t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad; t.syncs += x.syncs;
            appendNewestFirst(t.frames, x.frames, hz);
            for (const EdacsWordCount& w : x.words) {
                auto it = std::find_if(t.words.begin(), t.words.end(), [&](const EdacsWordCount& q2) { return q2.word == w.word; });
                if (it == t.words.end()) t.words.push_back(w); else it->count += w.count;
            }
            std::sort(t.words.begin(), t.words.end(), [](const EdacsWordCount& a, const EdacsWordCount& b) { return a.count > b.count; });
            if (t.words.size() > 64) t.words.resize(64);
        };
        ed.info = [](const EdacsTelemetry& t) { return t.blocksOk ? fmt("%d frames", (int)t.blocksOk) : std::string(); };
        put(131, ed, &RxTelemetry::edacs);
    }
    BankBase* bank(int m) const { auto it = banks.find(m); return it == banks.end() ? nullptr : it->second.get(); }
};

WideModes::WideModes() : p_(new Impl()) {
    for (auto& [m, bk] : p_->banks) p_->applyTuning(m, *bk);
}
WideModes::~WideModes() = default;

const std::vector<int>& WideModes::modes() { static const std::vector<int> v = {10, 32, 34, 39, 46, 51, 24, 25, 128, 129, 130, 131}; return v; }
bool WideModes::supports(int m) { const auto& v = modes(); return std::find(v.begin(), v.end(), m) != v.end(); }

std::vector<std::string> WideModes::freqDbModes(int m) {
    switch (m) {
    case 10: return {"dmr"};
    case 32: return {"m17"};
    case 34: return {"dstar"};
    case 39: return {"nxdn"};
    case 46: return {"dpmr"};
    case 51: return {"p25"};
    case 24: return {"pocsag", "flex"};
    case 25: return {"packet"};
    case 128: return {"mpt1327"};
    case 129: return {"fusion", "ysf"};
    case 130: return {"ltr"};
    case 131: return {"edacs"};
    }
    return {};
}

CarrierCfg WideModes::cfgFor(int m) {
    CarrierCfg c;
    c.stepHz = m == 46 ? 6250.0 : m == 24 || m == 25 || m == 131 ? 25000.0 : 12500.0;
    c.maxCarriers = 8;
    c.rejectTones = m != 130;   // a spur or the LO's leak is not a carrier; an LTR repeater's carrier is (its data is sub-audible)
    if (m == 24 || m == 25) { c.debounceSec = 0.12; c.holdSec = 30; }   // pages and packets come in short bursts
    if (m == 128 || m == 130 || m == 131) c.holdSec = 15;               // a control channel is on all the time; a short fade must not lose its decoder
    return c;
}

void WideModes::configure(int m, double rate, double origin) {
    BankBase* b = p_->bank(m);
    if (!b) return;
    CarrierCfg c = cfgFor(m); c.gridOriginHz = origin;
    b->configure(rate, c);
    p_->applyTuning(m, *b);
    p_->priors();
}

void WideModes::reset() { for (auto& [m, b] : p_->banks) b->reset(); }
void WideModes::clearResults() { for (auto& [m, b] : p_->banks) b->clearResults(); }
void WideModes::feed(int m, const cf32* x, size_t n) { if (BankBase* b = p_->bank(m)) b->feed(x, n); }
size_t WideModes::liveCount(int m) const { const BankBase* b = p_->bank(m); return b ? b->liveCount() : 0; }
std::vector<double> WideModes::liveHz(int m) const { const BankBase* b = p_->bank(m); return b ? b->liveHz() : std::vector<double>(); }
bool WideModes::ready(int m) const { const BankBase* b = p_->bank(m); return b && b->ready(); }
void WideModes::setLoadOverride(int m, double v) { if (BankBase* b = p_->bank(m)) b->setLoadOverride(v); }

void WideModes::setSelected(int id) { p_->sel = id; for (auto& [m, b] : p_->banks) b->setSelected(id); }
void WideModes::setMuted(bool mu) { p_->muted = mu; for (auto& [m, b] : p_->banks) b->setMuted(mu); }

void WideModes::setVolume(float v) {
    p_->volume = v;
    p_->dmr->forEach([v](DmrReceiver& r) { r.setVolume(v); });
    p_->m17->forEach([v](M17Receiver& r) { r.setVolume(v); });
    p_->dstar->forEach([v](DstarReceiver& r) { r.setVolume(v); });
    p_->nxdn->forEach([v](NxdnReceiver& r) { r.setVolume(v); });
    p_->dpmr->forEach([v](DpmrReceiver& r) { r.setVolume(v); });
    p_->p25->forEach([v](P25Receiver& r) { r.setVolume(v); });
    p_->fusion->forEach([v](FusionReceiver& r) { r.setVolume(v); });
}

void WideModes::setCenterHz(double hz) {
    for (auto& [m, b] : p_->banks) b->setCenterHz(hz);
    if (hz != p_->centerHz) { p_->centerHz = hz; p_->priors(); }
}

void WideModes::setTuning(const WideTuning& t) {
    p_->tuning = t;
    for (auto& [m, b] : p_->banks) p_->applyTuning(m, *b);
    p_->priors();
}
const WideTuning& WideModes::tuning() const { return p_->tuning; }
void WideModes::setExtraPriors(const std::vector<double>& absHz) { p_->extraPriors = absHz; p_->priors(); }
void WideModes::setFollowOnly(bool on, const std::vector<double>& absHz) { p_->followOnly = on; p_->followHz = absHz; p_->priors(); }

std::string WideModes::startRecording(int m, int id, const std::string& dir) { BankBase* b = p_->bank(m); return b ? b->startRecording(id, dir) : std::string(); }
void WideModes::stopRecording(int m, int id) { if (BankBase* b = p_->bank(m)) b->stopRecording(id); }

void WideModes::setPagerOptions(int protocols, int bauds) {
    if (protocols == p_->pagerProtos && bauds == p_->pagerBauds) return;
    p_->pagerProtos = protocols; p_->pagerBauds = bauds;
    p_->pager->forEach([&](PagerReceiver& r) { r.setProtocols(protocols); r.setPocsagBauds(bauds); });
}

// The carriers stay 25 kHz apart in the bank (WideBank steps by stepHz); the width only narrows the filter inside each carrier's receiver
void WideModes::setNarrowChannels(bool pager, bool packet) {
    if (pager != p_->pagerNarrow) { p_->pagerNarrow = pager; p_->pager->forEach([&](PagerReceiver& r) { r.setNarrowChannel(pager); }); }
    if (packet != p_->packetNarrow) { p_->packetNarrow = packet; p_->packet->forEach([&](PacketReceiver& r) { r.setNarrowChannel(packet); }); }
}

void WideModes::setPacketOptions(int fixBits, int emphasis) {
    if (fixBits == p_->packetFix && emphasis == p_->packetEmph) return;
    p_->packetFix = fixBits; p_->packetEmph = emphasis;
    p_->packet->forEach([&](PacketReceiver& r) { r.setFixBits(fixBits); r.setEmphasis(emphasis); });
}

void WideModes::setLogCallback(std::function<void(const std::string&)> cb) { p_->log = std::move(cb); }

bool WideModes::telemetry(int m, RxTelemetry& t, uint64_t& lastSeq) {
    BankBase* b = p_->bank(m);
    return b && b->telemetry(t, lastSeq, m);
}

} // namespace dect2
