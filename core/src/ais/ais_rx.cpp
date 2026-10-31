// AIS receiver: up to four channels at the same time, in two pairs 50 kHz wide: AIS 1 + 2 (161.975 / 162.025 MHz) and the long-range channels
// 75 + 76 (156.775 / 156.825 MHz). With the radio's centre known every pair the capture holds is placed by its frequency; without it AIS 1 and 2 sit
// at -25 and +25 kHz of the user's frequency.
// Front end, per pair: mix the pair's middle to 0 Hz, decimate to about 100 kHz, resample to exactly 96 kHz; each channel is mixed to zero, filtered
// and decimated to 48 kHz (5 samples per bit). A power detector finds the bursts (they are short and stand alone), each one is cut out of a ring of samples
// and decoded offline by aisDecodeBurst (ais_phy.cpp). The message layer and the station table are here.
#include "dect2/session/mode_adapters.h"
#include "dect2/marine/marine_mmsi.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/automation/engine.h"
#include "dect2/net/feeds.h"
#include "dect2/ais/ais_phy.h"
#include "dect2/ais/ais_alerts.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/ais/ais_longrange.h"
#include "dect2/ais/ais_sat.h"
#include "dect2/automation/event.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include "dect2/results/results_io.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kChanHz = 25000.0;
constexpr double kMidRate = 96000.0;
constexpr int kW = 24;                      // power smoothing: 0.5 ms
constexpr size_t kRing = 16384;
constexpr size_t kMaxBurst = 7200;          // 150 ms: five slots
constexpr size_t kPre = 480, kPost = 120;   // samples kept around a burst
constexpr size_t kBlock = 960;              // noise blocks of 20 ms
constexpr size_t kBlocks = 64;              // the noise floor is a low percentile of the last 1.3 s
constexpr double kOn = 2.0;                 // burst starts at 3 dB above the noise
constexpr double kEndAbs = 1.5, kEndRel = 0.08;
constexpr size_t kMaxVessels = 300, kTrackCap = 100, kTrackShown = 25, kNmeaCap = 50;
constexpr size_t kPublishSamples = 12000;   // 0.25 s at 48 kHz

int pickDecimation(double fs) {
    const int hi = std::max(1, (int)std::floor(fs / kMidRate));
    const int lo = std::max(1, (int)std::floor(fs / 130000.0));
    auto largestPrime = [](int v) {
        int lp = 1;
        for (int p = 2; p <= v; p++) while (v % p == 0) { lp = p; v /= p; }
        return lp;
    };
    int best = hi, bestPrime = largestPrime(hi);
    for (int d = hi; d >= lo; d--) {
        const int lp = largestPrime(d);
        if (lp <= 7) { best = d; bestPrime = lp; break; }
        if (lp < bestPrime) { best = d; bestPrime = lp; }
    }
    return best;
}

std::vector<int> splitFactors(int d) {
    std::vector<int> primes;
    for (int p = 2; d > 1;) {
        if (d % p == 0) { primes.push_back(p); d /= p; }
        else p++;
    }
    std::sort(primes.rbegin(), primes.rend());
    std::vector<int> stages;
    for (int p : primes) {
        if (!stages.empty() && stages.back() * p <= 8) stages.back() *= p;
        else stages.push_back(p);
    }
    return stages;
}

struct Pending { uint64_t start, end, decodeAt; };

// the front end of one pair of channels
struct Front {
    bool on = false;
    double midHz = 0;           // where the pair's middle sits relative to 0 Hz in the input
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    uint64_t n96 = 0;
    double offPhase = 0;
    std::vector<cf32> bufA, bufB, mixBuf, o2;
    void reset() {
        for (auto& st : stages) st.reset();
        if (!rsPass) rs.reset();
        n96 = 0; offPhase = 0;
    }
};

struct Chan {
    dmr::Fir<cf32> dec, filt;
    std::vector<cf32> ring = std::vector<cf32>(kRing);
    uint64_t total = 0;
    double delay[kW] = {};
    double sumW = 0;
    // noise floor from blocks of 20 ms
    double blkSum = 0; size_t blkN = 0; bool blkTaint = false;
    std::vector<double> blocks;
    double nf = 0; bool nfKnown = false;
    double levelEma = 0;
    // burst
    bool active = false;
    uint64_t start = 0; double peak = 0; int low = 0;
    std::deque<Pending> pend;
    double epoch = 0;           // signal time (of the receiver's clock) of this channel's sample 0
    AisDopplerTracker dop;
    void reset() {
        dec.reset(); filt.reset();
        total = 0;
        for (double& d : delay) d = 0;
        sumW = 0; blkSum = 0; blkN = 0; blkTaint = false;
        blocks.clear(); nf = 0; nfKnown = false; levelEma = 0;
        active = false; start = 0; peak = 0; low = 0;
        pend.clear();
        dop.reset();
    }
};

struct Rec {
    AisVessel v;
    double lastHeard = 0;
    double autoEmit = -1e9;   // the automations: when this vessel was last reported
};

} // namespace

struct AisReceiver::Impl {
    std::mutex procMu;          // feed / reset / configure
    std::mutex telMu;           // the published snapshot
    std::function<void(const std::string&)> log;

    double fs = 0, offsetHz = 0, centreHz = 0;
    int planMask = 15;
    bool isReady = false;
    Front fr[2];                // 0: AIS 1 + 2, 1: channels 75 + 76
    double enbw = 10000;        // noise bandwidth of the channel chain in Hz
    cf32 mixTab[2][96];
    Chan ch[4];
    std::vector<cf32> tmp, o1, o3, clean;
    std::vector<float> h2;      // the channel filter at 48 kHz (the cancelled bursts go through it)
    uint64_t inSamples = 0;     // input samples since the start or the last reset: the receiver's clock
    bool sat = false;
    std::unique_ptr<Sgp4> satSgp;
    double timeBase = 0;        // UNIX time of signal time 0 (0 = take the computer's clock at the first sample)
    bool timeBaseFixed = false;
    std::deque<AisRxRecord> recent;
    uint64_t recN = 0;
    std::vector<double> hints[4];   // satellite mode: frequency candidates per channel, refreshed with every report

    // results
    AisTelemetry t;             // counters and tables (the vessels and sentences are kept separately below)
    std::unordered_map<uint32_t, Rec> vessels;
    std::deque<std::string> nmea;
    std::deque<double> goodTimes[4];
    int nmeaSeq = 0;
    double lastGood = -1e9, lastBurst = -1e9, cfoEma = 0, snrEma = 0;
    bool haveSnr = false;
    uint64_t pubMark = 0;
    int lastState = 0;
    uint64_t seq = 0;
    AisTelemetry pub;           // what telemetry() hands out
    double lastNow = 0;         // signal time of the last report: the kept stations move back by it when the clock starts again

    AisPhyConfig phy;
    std::function<void(char, const ais::Bits&, double, double, double)> frameCb;   // every frame with a good FCS (VDES ASM listens through it)
    bool forwardFeeds = true;
    bool collisionCancel = true;
    AisAlertEngine alertEng;
    std::deque<AisAlertRecord> alertLog;

    void logLine(const std::string& s) { if (log) log(s); }
    double now() const { return fs > 0 ? (double)inSamples / fs : 0.0; }

    bool designFront(Front& F) {
        F.stages.clear();
        const int d = pickDecimation(fs);
        const std::vector<int> f = splitFactors(d);
        double r = fs;
        F.stages.assign(f.size(), dmr::Fir<cf32>());
        for (size_t i = 0; i < f.size(); i++) {
            const double out = r / f[i];
            const double pass = 38000.0, stop = std::max(pass + 4000.0, out - pass);
            F.stages[i].design(dmr::lowpassTaps(pass, std::min(stop, r * 0.5), r, 60, 9), f[i]);
            r = out;
        }
        F.rsPass = std::fabs(r - kMidRate) < 0.01;
        return F.rsPass || F.rs.configure(r, kMidRate);
    }

    // which pairs the capture holds, and where
    void place() {
        const bool known = centreHz > 1e6;
        fr[0].midHz = known ? ais::kAisMidHz - centreHz : offsetHz;
        fr[1].midHz = known ? ais::kLongRangeMidHz - centreHz : offsetHz + (ais::kLongRangeMidHz - ais::kAisMidHz);
        for (int p = 0; p < 2; p++) {
            const bool fits = std::fabs(fr[p].midHz) + ais::kPairMarginHz <= 0.49 * fs;
            fr[p].on = ((planMask >> (2 * p)) & 3) != 0 && (fits || (p == 0 && !known));   // without the centre, AIS 1 + 2 as before
        }
    }

    // the channel filters (the satellite mode widens them for the Doppler shift)
    void designChannels() {
        const double pass = sat ? 12000.0 : phy.chanPassHz, stop = sat ? 18000.0 : phy.chanStopHz;
        const std::vector<float> h1 = dmr::lowpassTaps(std::max(9500.0, pass + 1500.0), 38000.0, kMidRate, 60, 9);   // 96 -> 48 kHz (rejects the other channel)
        h2 = bbf::overrideTaps("ais", dmr::lowpassTaps(pass, stop, kAisWorkRate, 60, 9), kAisWorkRate, pass + stop);
        for (int c = 0; c < 4; c++) { ch[c].dec.design(h1, 2); ch[c].filt.design(h2, 1); }
        // noise bandwidth of both filters together: sum of |H1 H2|^2 df over the 48 kHz of the output
        double acc = 0;
        const int np = 4096;
        for (int k = 0; k < np; k++) {
            const double fHz = (k - np / 2) * (kAisWorkRate / np);
            std::complex<double> a1(0, 0), a2(0, 0);
            for (size_t i = 0; i < h1.size(); i++) a1 += (double)h1[i] * std::polar(1.0, -2 * kPi * fHz * (double)i / kMidRate);
            for (size_t i = 0; i < h2.size(); i++) a2 += (double)h2[i] * std::polar(1.0, -2 * kPi * fHz * (double)i / kAisWorkRate);
            acc += std::norm(a1 * a2);
        }
        enbw = std::max(1000.0, acc * (kAisWorkRate / np));
    }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        if (fs < aisTuning().minSampleRate - 1) return;
        for (Front& F : fr) if (!designFront(F)) return;
        // the two channels of a pair: mixers at +-25 kHz of the 96 kHz stream
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 96; i++) {
                const double a = 2 * kPi * (c ? -kChanHz : kChanHz) * i / kMidRate;      // mixing the channel down
                mixTab[c][i] = cf32((float)std::cos(a), (float)std::sin(a));
            }
        designChannels();
        place();
        isReady = true;
        resetState();
    }

    // after the placement may have changed (a retune): the front ends and channels start again, the counters and tables stay
    void replace() {
        const bool was0 = fr[0].on, was1 = fr[1].on;
        const double m0 = fr[0].midHz, m1 = fr[1].midHz;
        place();
        if (!isReady || (was0 == fr[0].on && was1 == fr[1].on && m0 == fr[0].midHz && m1 == fr[1].midHz)) return;
        for (Front& F : fr) F.reset();
        for (int c = 0; c < 4; c++) { ch[c].reset(); ch[c].epoch = now(); }
    }

    void resetState() {
        for (Front& F : fr) F.reset();
        for (Chan& C : ch) { C.reset(); C.epoch = 0; }
        inSamples = 0;
        if (!timeBaseFixed) timeBase = 0;
        t = AisTelemetry();
        for (auto& g : goodTimes) g.clear();
        for (auto& kv : vessels) kv.second.lastHeard -= lastNow;   // the stations and sentences stay (clearResults)
        lastNow = 0;
        lastGood = lastBurst = -1e9; cfoEma = snrEma = 0; haveSnr = false;
        pubMark = 0; lastState = 0;
        republish();
    }

    // the lists of a report: the stations most recently heard first, the sentences
    void fillLists(AisTelemetry& o, double tnow) const {
        std::vector<const Rec*> order;
        order.reserve(vessels.size());
        for (const auto& kv : vessels) order.push_back(&kv.second);
        std::sort(order.begin(), order.end(), [](const Rec* a, const Rec* b) { return a->lastHeard > b->lastHeard; });
        o.vessels.clear();
        o.vessels.reserve(order.size());
        for (size_t i = 0; i < order.size(); i++) {
            AisVessel v = order[i]->v;
            v.ageSec = (float)(tnow - order[i]->lastHeard);
            if (i >= kTrackShown) v.track.clear();
            o.vessels.push_back(std::move(v));
        }
        o.vesselCount = (uint32_t)vessels.size();
        o.longRangeVessels = 0;
        for (const auto& kv : vessels) o.longRangeVessels += kv.second.v.longRange;
        o.nmea.assign(nmea.begin(), nmea.end());
        o.recent.assign(recent.begin(), recent.end());
    }
    // a report of the lists alone (after a reset, a load or a clear: no new signal to wait for)
    void republish() {
        std::lock_guard<std::mutex> lk(telMu);
        AisTelemetry o = t;
        o.timeSec = lastNow;
        fillLists(o, lastNow);
        o.seq = ++seq;
        pub = std::move(o);
    }

    // ---- stations
    void updateVessel(const ais::AisMsg& m, char chan, double tnow, double utc) {
        auto it = vessels.find(m.mmsi);
        if (it == vessels.end()) {
            if (vessels.size() >= kMaxVessels) {
                auto old = vessels.begin();
                for (auto j = vessels.begin(); j != vessels.end(); ++j) if (j->second.lastHeard < old->second.lastHeard) old = j;
                vessels.erase(old);
            }
            it = vessels.emplace(m.mmsi, Rec()).first;
            it->second.v.mmsi = m.mmsi;
            if (vessels.size() <= 40) logLine("AIS: new station " + std::to_string(m.mmsi));
        }
        Rec& r = it->second;
        AisVessel& v = r.v;
        r.lastHeard = tnow;
        v.messages++;
        v.lastType = m.type;
        v.channel = chan;
        v.lastUtc = utc;
        if (m.type == 27 && m.valid) { v.longRange = true; v.longRangeMsgs++; v.posLatency = m.posLatency; }
        if (m.cls != AIS_CLASS_OTHER) v.cls = m.cls;
        if (!m.valid) return;
        const bool movement = m.type == 1 || m.type == 2 || m.type == 3 || m.type == 9 || m.type == 18 || m.type == 19 || m.type == 27;
        if (m.hasPos) {
            const bool moved = !v.hasPos || std::fabs(v.lat - m.lat) > 0.0002 || std::fabs(v.lon - m.lon) > 0.0002;
            v.hasPos = true; v.lat = m.lat; v.lon = m.lon;
            v.lrPosition = m.type == 27;
            if (moved) {
                v.track.push_back({(float)m.lat, (float)m.lon});
                if (v.track.size() > kTrackCap) v.track.erase(v.track.begin());
            }
        }
        if (movement) {
            v.sog = m.sog; v.cog = m.cog;
            if (m.type != 27) v.heading = m.heading;   // message 27 has no heading: the last one stays
            if (m.type <= 3) { v.hasRot = m.hasRot; v.rotDegMin = m.rotDegMin; v.navStatus = m.navStatus; }
            if (m.type == 27) v.navStatus = m.navStatus;
            if (m.type == 9) v.altitudeM = m.altitudeM;
        }
        if (!m.name.empty()) v.name = m.name;
        if (!m.callsign.empty()) v.callsign = m.callsign;
        if (!m.destination.empty() || m.type == 5) v.destination = m.destination;
        if (m.imo) v.imo = m.imo;
        if (m.shipType >= 0) v.shipType = m.shipType;
        if ((m.type == 5 || m.type == 24) && (!m.name.empty() || !m.callsign.empty() || m.shipType > 0)) {   // static data: the local ship table (DSC and AIS views)
            char mm[16]; snprintf(mm, sizeof mm, "%09u", (unsigned)m.mmsi);
            marine::ShipDb::global().learn(mm, v.name, v.callsign, m.shipType > 0 ? aisShipTypeText(m.shipType) : std::string());
        }
        if (m.dimA || m.dimB || m.dimC || m.dimD) { v.dimA = m.dimA; v.dimB = m.dimB; v.dimC = m.dimC; v.dimD = m.dimD; }
        if (m.type == 5) {
            v.etaMonth = m.etaMonth; v.etaDay = m.etaDay; v.etaHour = m.etaHour; v.etaMin = m.etaMin;
            v.draughtM = m.draughtM;
        }
        if (m.type == 21) {
            v.aidType = m.aidType; v.virtualAid = m.virtualAid;
            if (m.second < 60) v.offPosition = m.offPosition;
        }
        if (m.hasPos && automation::wants(automation::FAis) && tnow - r.autoEmit >= 5.0) {   // the automations: a position report, at most every 5 s per vessel
            r.autoEmit = tnow;
            static const char* const cls[] = {"A", "B", "base", "aid", "SAR", "other"};
            automation::Event ev("ais.vessel");
            ev.at(automation::wallNow(), "AIS", aisChannelHz(aisChannelIndex(chan)));
            ev.num("mmsi", m.mmsi).str("name", v.name).str("callsign", v.callsign).num("ship_type", v.shipType).num("lat", v.lat).num("lon", v.lon)
                .num("sog_kt", v.sog).num("cog_deg", v.cog).num("nav_status", v.navStatus).str("class", cls[v.cls >= 0 && v.cls <= 5 ? v.cls : 5]).str("destination", v.destination);
            automation::emit(std::move(ev));
        }
        if (m.hasPos) raiseAlerts(v);
    }

    // CPA and geofence alerts for one position update
    void raiseAlerts(const AisVessel& v) {
        const double utc = v.lastUtc > 0 ? v.lastUtc : automation::wallNow();
        for (const AisAlert& a : alertEng.update(v, utc)) {
            AisAlertRecord r;
            r.utc = a.utc; r.kind = a.kind; r.fence = a.fence; r.mmsi = a.mmsi; r.name = a.name;
            r.lat = a.lat; r.lon = a.lon; r.distNm = a.distNm; r.dcpaNm = a.dcpaNm; r.tcpaMin = a.tcpaMin;
            alertLog.push_back(r);
            while (alertLog.size() > 50) alertLog.pop_front();
            char b[160];
            if (a.kind == "cpa") snprintf(b, sizeof b, "AIS alert: %u %s will pass %.2f nm in %.0f min", a.mmsi, a.name.c_str(), a.dcpaNm, a.tcpaMin);
            else snprintf(b, sizeof b, "AIS alert: %u %s %s %s", a.mmsi, a.name.c_str(), a.kind == "enter" ? "entered" : "left", a.fence.c_str());
            logLine(b);
            if (!automation::wants(automation::FAis)) continue;
            static const char* const cls[] = {"A", "B", "base", "aid", "SAR", "other"};
            const char* cn = cls[a.cls >= 0 && a.cls <= 5 ? a.cls : 5];
            if (a.kind == "cpa") {
                automation::Event ev("ais.cpa");
                ev.at(automation::wallNow(), "AIS", aisChannelHz(aisChannelIndex(v.channel)));
                ev.num("mmsi", a.mmsi).str("name", a.name).num("ship_type", a.shipType).str("class", cn).num("lat", a.lat).num("lon", a.lon)
                    .num("sog_kt", a.sogKn).num("cog_deg", a.cogDeg).num("distance_nm", a.distNm).num("cpa_nm", a.dcpaNm).num("tcpa_min", a.tcpaMin);
                automation::emit(std::move(ev));
            } else {
                automation::Event ev("ais.geofence");
                ev.at(automation::wallNow(), "AIS", aisChannelHz(aisChannelIndex(v.channel)));
                ev.num("mmsi", a.mmsi).str("name", a.name).num("ship_type", a.shipType).str("class", cn).num("lat", a.lat).num("lon", a.lon)
                    .num("sog_kt", a.sogKn).num("cog_deg", a.cogDeg).num("distance_nm", a.distNm).str("fence", a.fence).str("transition", a.kind);
                automation::emit(std::move(ev));
            }
        }
    }

    void onFrame(int c, const ais::Bits& bits, double snrDb, double cfoHz, double startSec = -1, int layer = 0) {
        const double tnow = now();
        const char chn = aisChannelLetter(c);
        if (bits.size() < 38) { t.blocksBad++; t.channelBad[c]++; return; }
        ais::AisMsg m;
        const bool decoded = ais::decodeMessage(bits, m);
        if (!ais::acceptableAis(m, decoded)) { if (frameCb) frameCb(chn, bits, tnow, snrDb, cfoHz); t.blocksBad++; t.channelBad[c]++; return; }   // a good FCS on something that is not an AIS message (#87)
        t.blocksOk++; t.channelOk[c]++;
        t.typeCount[c][m.type >= 1 && m.type <= 27 ? m.type : 0]++;
        goodTimes[c].push_back(tnow);
        lastGood = tnow;
        cfoEma = haveSnr ? 0.85 * cfoEma + 0.15 * cfoHz : cfoHz;
        snrEma = haveSnr ? 0.85 * snrEma + 0.15 * snrDb : snrDb;
        haveSnr = true;
        if (frameCb) frameCb(chn, bits, tnow, snrDb, cfoHz);
        const std::vector<std::string> sentences = ais::toNmea(bits, chn, nmeaSeq);
        if (forwardFeeds && Feeds::get().wantAis()) Feeds::get().ais(sentences);   // the data feed out (feeds.h), off unless the user turned it on
        for (const std::string& s : sentences) nmea.push_back(s);
        if (bits.size() > 6 * 60) nmeaSeq = (nmeaSeq + 1) % 10;
        while (nmea.size() > kNmeaCap) nmea.pop_front();
        ch[c].dop.add(tnow, cfoHz);
        if (layer == 1) t.recovered++;
        if (layer >= 2) t.cancelled++;
        if (!sat && layer >= 1) t.collisionRecovered++;
        // the time stamp of the reception (message 27 carries none)
        if (timeBase == 0) timeBase = automation::wallNow() - tnow;
        AisRxRecord rec;
        rec.n = ++recN;
        rec.timeSec = startSec >= 0 ? startSec : tnow;
        rec.utc = timeBase + rec.timeSec;
        rec.channel = chn; rec.type = m.type; rec.mmsi = m.mmsi;
        rec.snrDb = (float)snrDb; rec.cfoHz = (float)cfoHz;
        rec.hasPos = m.valid && m.hasPos; rec.lat = (float)m.lat; rec.lon = (float)m.lon;
        rec.sog = m.sog; rec.cog = m.cog; rec.navStatus = m.navStatus; rec.posLatency = m.posLatency;
        rec.layer = layer;
        recent.push_back(rec);
        while (recent.size() > 500) recent.pop_front();
        if (m.mmsi) updateVessel(m, chn, tnow, rec.utc);
    }

    // satellite mode: the frequency candidates of a channel: the bursts heard lately, and (with the satellite's elements) the Doppler shift each
    // ship heard with a position should have now. Clustered to 300 Hz, the most common first.
    void refreshHints(double tnow) {
        for (int c = 0; c < 4; c++) {
            std::vector<double> v = ch[c].dop.recent(tnow, 30);
            if (satSgp && timeBase > 0) {
                const double utc = timeBase + tnow;
                int used = 0;
                for (const auto& kv : vessels) {
                    const Rec& r = kv.second;
                    if (!r.v.hasPos || tnow - r.lastHeard > 900 || used >= 300) continue;
                    Observer o; o.latDeg = r.v.lat; o.lonDeg = r.v.lon;
                    Look l;
                    if (!lookAt(*satSgp, o, utc, l) || l.elDeg < -1) continue;
                    v.push_back(dopplerHz(aisChannelHz(c), l.rangeRateKmS));
                    used++;
                }
            }
            std::sort(v.begin(), v.end());
            std::vector<std::pair<int, double>> cl;   // count, centre
            for (size_t i = 0; i < v.size();) {
                size_t j = i; double s = 0;
                while (j < v.size() && v[j] - v[i] < 300) { s += v[j]; j++; }
                cl.push_back({(int)(j - i), s / (double)(j - i)});
                i = j;
            }
            std::stable_sort(cl.begin(), cl.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            hints[c].clear();
            for (size_t i = 0; i < cl.size() && i < 6; i++) hints[c].push_back(cl[i].second);
        }
    }

    // ---- bursts
    void decodeBurst(int c, const Pending& p) {
        Chan& C = ch[c];
        t.bursts++;
        lastBurst = now();
        const uint64_t oldest = C.total > kRing ? C.total - kRing : 0;
        uint64_t s0 = p.start > kPre ? p.start - kPre : 0;
        if (s0 < oldest) s0 = oldest;
        const uint64_t e0 = std::min(p.end + kPost, C.total);
        if (e0 <= s0 + 200) return;
        std::vector<cf32> seg((size_t)(e0 - s0));
        for (uint64_t i = s0; i < e0; i++) seg[(size_t)(i - s0)] = C.ring[(size_t)(i % kRing)];
        const size_t core0 = (size_t)(p.start > s0 ? p.start - s0 : 0), core1 = (size_t)(std::min(p.end, e0) - s0);
        const double startSec = C.epoch + (double)p.start / kAisWorkRate;
        AisCollisionResult cr;
        if (sat) {
            AisCollisionConfig cc;
            cc.chanTaps = &h2;
            cr = aisDecodeCollided(seg.data(), seg.size(), core0, core1, phy, hints[c], cc);
        } else if (collisionCancel) {
            // coast mode: the same successive cancellation as the satellite mode, but only a little Doppler-free frequency search (two transmitters
            // differ by up to about 1 kHz) and two layers, so a clean burst costs one extra fit and noise bursts a few tries
            AisCollisionConfig cc;
            cc.chanTaps = &h2;
            cc.maxLayers = 2; cc.maxCandidates = 3; cc.searchHz = 1000; cc.gridHz = 500; cc.minSepHz = 250;
            cr = aisDecodeCollided(seg.data(), seg.size(), core0, core1, phy, {}, cc);
        } else {
            const AisBurstResult r = aisDecodeBurst(seg.data(), seg.size(), core0, core1, phy);
            cr.bad = r.bad; cr.cfoHz = r.cfoHz;
            for (const auto& f : r.frames) { AisFound a; a.payload = f; a.cfoHz = r.cfoHz; cr.frames.push_back(std::move(a)); }
        }
        if (cr.frames.empty()) {
            if (cr.bad > 0) { t.blocksBad++; t.channelBad[c]++; }
            return;
        }
        // signal to noise ratio in 48 kHz: the burst power above the noise floor against the noise floor scaled from the filter's bandwidth
        double pw = 0;
        const size_t a = std::min(core0 + kW, seg.size()), b = core1 > kW ? core1 - kW : 0;
        size_t cnt = 0;
        for (size_t i = a; i < b; i++) { pw += std::norm(seg[i]); cnt++; }
        double snr = 0;
        const double n48 = C.nf * kAisWorkRate / enbw;
        if (cnt > 10 && C.nfKnown) {
            const double sig = pw / (double)cnt - C.nf;
            snr = sig > 0 ? 10 * std::log10(sig / n48) : -10;
        }
        for (const auto& f : cr.frames) {
            const double s = f.power > 0 && C.nfKnown && n48 > 0 ? 10 * std::log10(f.power / n48) : snr;   // a burst that was fitted: its own power
            onFrame(c, f.payload, s, f.cfoHz, startSec, f.layer);
        }
    }

    void chanSample(int c, cf32 x) {
        Chan& C = ch[c];
        const uint64_t idx = C.total++;
        C.ring[(size_t)(idx % kRing)] = x;
        const double p = std::norm(x);
        double& dl = C.delay[idx % kW];
        C.sumW += p - dl;
        dl = p;
        const double s = std::max(0.0, C.sumW) / kW;
        // noise blocks
        C.blkSum += p; C.blkN++;
        if (C.active) C.blkTaint = true;
        if (C.blkN == kBlock) {
            const double mean = C.blkSum / (double)kBlock;
            if (!C.blkTaint) {
                C.blocks.push_back(mean);
                if (C.blocks.size() > kBlocks) C.blocks.erase(C.blocks.begin());
                if (C.blocks.size() >= 3) {
                    std::vector<double> sorted = C.blocks;
                    std::sort(sorted.begin(), sorted.end());
                    C.nf = std::max(sorted[sorted.size() / 5] * 1.03, 1e-10);
                    C.nfKnown = true;
                }
            }
            C.levelEma = C.levelEma == 0 ? mean : 0.9 * C.levelEma + 0.1 * mean;
            C.blkSum = 0; C.blkN = 0; C.blkTaint = false;
        }
        if (!C.active) {
            if (C.nfKnown && idx >= (uint64_t)kW && s > kOn * C.nf) {
                C.active = true; C.start = idx - kW / 2; C.peak = s; C.low = 0;
                C.blkTaint = true;
            }
        } else {
            C.peak = std::max(C.peak, s);
            const double endThr = std::max(kEndAbs * C.nf, kEndRel * C.peak);
            if (s < endThr) C.low++; else C.low = 0;
            if (C.low >= 12 || idx - C.start > kMaxBurst) {
                const uint64_t end = idx - (uint64_t)C.low;
                if (end > C.start + 100) C.pend.push_back({C.start, end, idx + kPost});
                C.active = false;
            }
        }
        while (!C.pend.empty() && C.pend.front().decodeAt <= idx) {
            const Pending pd = C.pend.front();
            C.pend.pop_front();
            decodeBurst(c, pd);
        }
    }

    // ---- publishing
    void publish() {
        const double tnow = now();
        lastNow = tnow;
        t.channelMask = (fr[0].on ? 3 : 0) | (fr[1].on ? 12 : 0);
        t.satellite = sat;
        if (sat) refreshHints(tnow);
        for (int c = 0; c < 4; c++) {
            t.dopplerHz[c] = ch[c].dop.valid() ? (float)ch[c].dop.estimate(tnow) : 0.f;
            t.dopplerRateHzS[c] = (float)ch[c].dop.rateHzS();
            while (!goodTimes[c].empty() && goodTimes[c].front() < tnow - 60) goodTimes[c].pop_front();
            const double span = std::max(5.0, std::min(60.0, tnow));
            t.burstsPerMin[c] = (float)((double)goodTimes[c].size() * 60.0 / span);
            const double scale = kAisWorkRate / enbw;
            t.levelDbfs[c] = ch[c].levelEma > 0 ? (float)(10 * std::log10(ch[c].levelEma * scale)) : -120.f;
            t.noiseDbfs[c] = ch[c].nfKnown ? (float)(10 * std::log10(ch[c].nf * scale)) : -120.f;
        }
        t.timeSec = tnow;
        t.vesselCount = (uint32_t)vessels.size();
        t.dataValid = lastGood > -1e8 && tnow - lastGood < 10.0;
        t.state = t.dataValid ? 2 : (lastBurst > -1e8 && tnow - lastBurst < 10.0 ? 1 : 0);
        t.cfoHz = cfoEma;
        t.snrDb = (float)snrEma;
        if (t.state != lastState) {
            logLine(t.state == 2 ? "AIS: decoding messages" : t.state == 1 ? "AIS: signal, no good message" : "AIS: lost the signal");
            lastState = t.state;
        }
        AisTelemetry o = t;
        fillLists(o, tnow);
        o.alerts.assign(alertLog.begin(), alertLog.end());
        std::lock_guard<std::mutex> lk(telMu);
        o.seq = ++seq;
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        bool bad = false;
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real(), im = x[i].imag();
            if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; }
        }
        if (bad) {
            clean.assign(x, x + n);
            for (auto& v : clean) if (!(std::fabs(v.real()) < 1e4f && std::fabs(v.imag()) < 1e4f)) v = cf32(0, 0);
            x = clean.data();
        }
        for (int p = 0; p < 2; p++) {
            Front& F = fr[p];
            if (!F.on) continue;
            const cf32* cur = x;
            size_t cn = n;
            if (F.midHz != 0) {
                // the middle of the pair's two channels goes to 0 Hz
                F.mixBuf.resize(n);
                const double w = -2 * kPi * F.midHz / fs;
                const cf32 stp((float)std::cos(w), (float)std::sin(w));
                cf32 ph((float)std::cos(F.offPhase), (float)std::sin(F.offPhase));
                for (size_t i = 0; i < n; i++) {
                    F.mixBuf[i] = x[i] * ph;
                    ph *= stp;
                    if ((i & 1023) == 1023) ph /= std::abs(ph);
                }
                F.offPhase = std::fmod(F.offPhase + w * (double)n, 2 * kPi);
                cur = F.mixBuf.data();
            }
            for (size_t s = 0; s < F.stages.size(); s++) {
                std::vector<cf32>& dst = (s & 1) ? F.bufB : F.bufA;
                dst.clear();
                F.stages[s].process(cur, cn, dst);
                cur = dst.data(); cn = dst.size();
            }
            if (!F.rsPass) {
                F.o2.clear();
                F.rs.process(cur, cn, F.o2);
                cur = F.o2.data(); cn = F.o2.size();
            }
            for (int k = 0; k < 2; k++) {
                const int c = 2 * p + k;
                tmp.resize(cn);
                for (size_t i = 0; i < cn; i++) tmp[i] = cur[i] * mixTab[k][(F.n96 + i) % 96];
                o1.clear();
                ch[c].dec.process(tmp.data(), cn, o1);
                o3.clear();
                ch[c].filt.process(o1.data(), o1.size(), o3);
                for (const cf32& v : o3) chanSample(c, v);
            }
            F.n96 += cn;
        }
        inSamples += n;
        const uint64_t pubEvery = (uint64_t)std::llround(fs * (double)kPublishSamples / kAisWorkRate);
        if (inSamples - pubMark >= pubEvery) {
            pubMark = inSamples;
            publish();
        }
    }
};

AisReceiver::AisReceiver() : p_(std::make_unique<Impl>()) {}
AisReceiver::~AisReceiver() = default;

void AisReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->configure(inputRateHz);
}
void AisReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->offsetHz = hz;
    p_->replace();
    p_->fr[0].offPhase = p_->fr[1].offPhase = 0;
}
void AisReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->centreHz = hz;
    p_->replace();
}
void AisReceiver::setChannelMask(int mask) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->planMask = mask & 15;
    p_->replace();
}
void AisReceiver::setSatelliteMode(bool on) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->sat == on) return;
    p_->sat = on;
    if (p_->isReady) p_->designChannels();
    for (auto& h : p_->hints) h.clear();
}
void AisReceiver::setCollisionCancel(bool on) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->collisionCancel = on;
}
void AisReceiver::setAlertConfig(const AisAlertConfig& c) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->alertEng.configure(c);
}
void AisReceiver::setSatelliteTle(const Tle* tle) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->satSgp.reset();
    if (!tle) return;
    auto s = std::make_unique<Sgp4>();
    if (s->init(*tle)) p_->satSgp = std::move(s);
}
void AisReceiver::setTimeBase(double unixSec) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->timeBase = unixSec;
    p_->timeBaseFixed = unixSec > 0;
}
bool AisReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return p_->isReady;
}
void AisReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->isReady) p_->resetState();
}
void AisReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->feed(x, n);
}
bool AisReceiver::telemetry(AisTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    sess::observeAis(out);
    return true;
}
void AisReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->vessels.clear(); p_->nmea.clear(); p_->recent.clear(); p_->alertLog.clear(); p_->alertEng.clear();
    p_->republish();
}
std::string AisReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return results::pack(p_->lastNow, p_->vessels, p_->nmea);
}
bool AisReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::unordered_map<uint32_t, Rec> vessels;
    std::deque<std::string> nmea;
    if (!results::unpack(blob, t0, vessels, nmea)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    for (auto& kv : vessels) kv.second.lastHeard += p_->lastNow - t0;
    p_->vessels = std::move(vessels); p_->nmea = std::move(nmea);
    p_->republish();
    return true;
}
void AisReceiver::setPhyConfig(const AisPhyConfig& c) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->phy = c;
}
void AisReceiver::setFrameCallback(std::function<void(char, const ais::Bits&, double, double, double)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->frameCb = std::move(cb);
}
void AisReceiver::setForwardFeeds(bool on) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->forwardFeeds = on;
}
void AisReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}

ModeTuning aisTuning() {
    ModeTuning t;
    t.stdMode = 16; t.id = "ais"; t.name = "AIS";
    t.minMhz = 156; t.maxMhz = 163; t.defMhz = 162;     // the default centre puts AIS 1 and AIS 2 at -25 and +25 kHz
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.1;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 0;                                 // DC falls between the two channels
    return t;
}

} // namespace dect2
