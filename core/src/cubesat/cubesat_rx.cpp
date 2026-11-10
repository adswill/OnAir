// CubeSat telemetry receiver: see cubesat_rx.h. The radio side is the packet receiver's (packet_demod.h), the frame layers are
// packet_ax25.h and cubesat_proto.h.
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/cubesat/cubesat_grsat.h"
#include "dect2/cubesat/cubesat_more.h"
#include "dect2/common/mathconst.h"
#include "dect2/net/feeds.h"
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_demod.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include "dect2/common/bits.h"
#include "dect2/common/str.h"

namespace dect2 {

using namespace cubesat;

namespace {

using pktdemod::kMidRate;
constexpr int64_t kDupWindow = (int64_t)(0.05 * kMidRate);
constexpr int kAsmMaxErr = 2;

int popcount32(uint32_t v) { return dect2::bits::popcount(v); }

uint64_t hashBytes(const std::vector<uint8_t>& d) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : d) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

std::string upper(std::string s) { return dect2::str::upper(std::move(s)); }
std::string baseCall(const std::string& s) { const size_t d = s.find('-'); return upper(d == std::string::npos ? s : s.substr(0, d)); }

// Hunts for the AX100 (0x930B51DE) and CCSDS (0x1ACFFC1D) sync words in a bit stream, either polarity, then collects the frame:
// AX100: the Golay field and as many bytes as it gives; CCSDS: the fixed block length.
struct AsmFramer {
    enum St { Hunt, Golay, Body };
    St st = Hunt;
    uint32_t sh = 0;
    int nb = 0;
    bool inv = false, ccsds = false;
    uint32_t acc = 0; int accBits = 0;
    int flags = 0;
    size_t need = 0;
    std::vector<uint8_t> body;
    int ccsdsLen = 255;
    // kind 0 AX100, 1 CCSDS; golayFailed: the sync word came but the length field did not decode
    std::function<void(bool ccsds, std::vector<uint8_t>& body, int flags, bool golayFailed)> done;
    void reset() { st = Hunt; sh = 0; nb = 0; acc = 0; accBits = 0; body.clear(); }
    bool inFrame() const { return st != Hunt; }
    void bit(int b) {
        if (st == Hunt) {
            sh = (sh << 1) | (uint32_t)(b & 1);
            if (++nb < 32) return;
            for (int k = 0; k < 2; k++) {
                const uint32_t a = k ? kAsmCcsds : kAsmAx100;
                const int e = popcount32(sh ^ a), ei = popcount32(~sh ^ a);
                if (e <= kAsmMaxErr || ei <= kAsmMaxErr) {
                    inv = ei < e; ccsds = k == 1;
                    acc = 0; accBits = 0; body.clear();
                    if (ccsds) { st = Body; need = (size_t)ccsdsLen; flags = 0; }
                    else st = Golay;
                    return;
                }
            }
            return;
        }
        const int v = (b ^ (inv ? 1 : 0)) & 1;
        acc = (acc << 1) | (uint32_t)v;
        accBits++;
        if (st == Golay) {
            if (accBits < 24) return;
            const int d = golayDecode(acc & 0xFFFFFF);
            acc = 0; accBits = 0;
            if (d < 0 || (d & 0xFF) < 5) { std::vector<uint8_t> none; if (done) done(false, none, 0, true); st = Hunt; nb = 0; return; }
            flags = d & 0xF00; need = (size_t)(d & 0xFF);
            st = Body;
            return;
        }
        if (accBits < 8) return;
        body.push_back((uint8_t)acc);
        acc = 0; accBits = 0;
        if (body.size() >= need) {
            if (done) done(ccsds, body, flags, false);
            st = Hunt; nb = 0; sh = 0;
        }
    }
};

// Audio FSK on the FM discriminator (GOMspace U482C radios: tones at afCarrier +- the deviation): the audio mixed down by the carrier,
// filtered, FM-discriminated again -> a frequency in Hz the FSK slicer takes.
struct AudioFsk {
    double fc = 3600;
    double ph = 0;
    cf32 prev{1.f, 0.f};
    dmr::Fir<cf32> lp;
    std::vector<cf32> mix, flt;
    void init(double carrier, double baud) {
        fc = carrier;
        const double pass = std::min(carrier * 0.9, 0.75 * baud + 600), stop = std::min(1.6 * carrier, pass + 0.75 * baud + 1500);
        lp.design(dmr::lowpassTaps(pass, stop, kMidRate, 40, 9), 1);
        reset();
    }
    void reset() { ph = 0; prev = cf32(1.f, 0.f); lp.reset(); }
    void run(const float* x, size_t n, std::vector<float>& out) {
        mix.resize(n);
        const double w = 2 * dect2::math::kPi * fc / kMidRate;
        for (size_t i = 0; i < n; i++) {
            mix[i] = cf32((float)(x[i] * std::cos(ph)), (float)(-x[i] * std::sin(ph)));
            ph += w; if (ph > 2 * dect2::math::kPi) ph -= 2 * dect2::math::kPi;
        }
        flt.clear();
        lp.process(mix.data(), n, flt);
        out.resize(flt.size());
        const float k = (float)(kMidRate / (2 * dect2::math::kPi));
        for (size_t i = 0; i < flt.size(); i++) {
            const cf32 d = flt[i] * std::conj(prev);
            prev = flt[i];
            out[i] = std::atan2(d.imag(), d.real()) * k;
        }
    }
};

// One extra demodulator: a baud rate (and, for AFSK, an audio carrier) the fixed 9600 / 4800 slicers do not cover. Feeds the sync word
// framings (AX100 Mode 5, CCSDS, and every cubesat_more.h framing) and, for G3RUH / NRZI AX.25, an HDLC slicer.
struct ExtraPath {
    bool on = false, hdlc = false, audio = false;
    int baud = 0;
    std::string why;
    AudioFsk af;
    dmr::Fir<float> lp;
    std::vector<float> a, f;
    pktdemod::FskSlicer g3, plain;
    AsmFramer framer;
    MoreHunter more;
};

} // namespace

struct CubesatReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0;
    bool isReady = false;
    pktdemod::FmFront fe;
    dmr::Fir<float> audio48;
    pktdemod::Afsk1200 afsk[2];
    // G3RUH HDLC: 0 = 9600, 1 = 4800; plain slicers for the sync words: 0 = 9600, 1 = 4800
    dmr::Fir<float> lpG[2];
    pktdemod::FskSlicer g3[2], plain[2];
    AsmFramer framer[2];
    std::vector<float> a48, fg[2];
    // 3.9: the cubesat_more.h framings on the 9600 / 4800 slicers, the GOMspace audio FSK (4800 baud on 3600 Hz), and the tracked satellite's own
    MoreHunter more[2];
    ExtraPath afskPath;
    std::vector<std::unique_ptr<ExtraPath>> trackPaths;   // one per baud rate / audio carrier of the tracked satellite the fixed ones miss
    const GrTx* trackTx = nullptr;
    MoreConfig moreCfg;

    // naming and options (set from the interface thread under procMu)
    std::vector<SatDef> sats = defaultSats();
    double dialHz = 0;
    int trackedNorad = 0; std::string trackedName;
    bool forwardKiss = true, wallClock = false;
    int ccsdsLen = 255;

    int64_t nextReport = 0;
    uint64_t seq = 0;
    struct Recent { uint64_t h; int64_t t; };
    std::deque<Recent> recent;
    struct Fail { int64_t t; };
    std::deque<Fail> fails;
    std::deque<CubeFrame> frames;   // newest first
    CubeTelemetry t, pub;
    int lastState = 0;

    void say(const std::string& s) { if (log) log(s); }
    double now() const { return (double)fe.n96 / kMidRate; }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        fe.chanPass = 12000.0; fe.chanStop = 20000.0; fe.clipHz = 10000.f; fe.afcMaxHz = 15000.0;
        if (!fe.configure(fs, cubesatTuning().minSampleRate)) return;
        audio48.design(dmr::lowpassTaps(3300.0, 8000.0, kMidRate, 50, 9), 2);
        lpG[0].design(dmr::lowpassTaps(5200.0, 11000.0, kMidRate, 40, 9), 1);
        lpG[1].design(dmr::lowpassTaps(2800.0, 6000.0, kMidRate, 40, 9), 1);
        afsk[0].init(40, false, 1.0f, 0.70f, 0.80f);
        afsk[1].init(40, true, 0.7f, 0.75f, 0.85f);
        for (int i = 0; i < 2; i++) {
            const double baud = i ? 4800.0 : 9600.0;
            g3[i].baud = baud; g3[i].descramble = true; g3[i].init(0.70f, 0.80f);
            plain[i].baud = baud; plain[i].descramble = false; plain[i].init(0.70f, 0.85f);
            plain[i].bitSink = [this, i](int b) { framer[i].bit(b); more[i].push(b ? 1.f : -1.f); plain[i].locked = framer[i].inFrame() || more[i].busy(); };
            more[i].onFrame = [this, i](const MoreFrame& m) { onMore(m, i ? 4800 : 9600, plain[i].pos); };
            framer[i].ccsdsLen = ccsdsLen;
            framer[i].done = [this, i](bool cc, std::vector<uint8_t>& body, int flags, bool golayFailed) { onAsm(i, cc, body, flags, golayFailed); };
        }
        for (int i = 0; i < 2; i++) afsk[i].hdlc.setCallback([this, i](const std::vector<uint8_t>& d, bool ok) { onHdlc(Ax25Afsk1200, 1200, i, d, ok, afsk[i].pos * 2); });
        for (int i = 0; i < 2; i++) g3[i].hdlc.setCallback([this, i](const std::vector<uint8_t>& d, bool ok) { onHdlc(Ax25G3ruh, i ? 4800 : 9600, i, d, ok, g3[i].pos); });
        setupPath(afskPath, 4800, 3600.0, false, "GOMspace AFSK 4800");
        setupTracked();
        isReady = true;
        resetState();
        say("CubeSat: listening for AX.25 (1200 AFSK, 9600 / 4800 G3RUH), AX100 Mode 5 / 6, CCSDS, U482C, CCSDS concatenated, USP, NGHam and Mobitex frames");
    }

    void resetState() {
        const double off = fe.offsetHz;
        fe.reset(); fe.offsetHz = off;
        audio48.reset(); lpG[0].reset(); lpG[1].reset();
        for (auto& a : afsk) a.reset();
        for (int i = 0; i < 2; i++) { g3[i].reset(); plain[i].reset(); framer[i].reset(); framer[i].ccsdsLen = ccsdsLen; more[i].reset(); }
        std::vector<ExtraPath*> eps{&afskPath};
        for (auto& tp : trackPaths) eps.push_back(tp.get());
        for (ExtraPath* ep : eps) { ep->af.reset(); ep->lp.reset(); ep->g3.reset(); ep->plain.reset(); ep->framer.reset(); ep->more.reset(); }
        nextReport = 0;
        recent.clear(); fails.clear();
        for (auto& f : frames) f.timeSec -= t.timeSec;   // the frames stay (clearResults): the clock starts again
        CubeTelemetry n;
        n.inputRate = fs;
        n.blocksOk = t.blocksOk; n.blocksBad = t.blocksBad; n.rsFixed = t.rsFixed; n.kissSent = t.kissSent;
        for (int i = 0; i < kNumFramings; i++) n.perFraming[i] = t.perFraming[i];
        t = n;
        lastState = 0;
    }

    // ---- naming
    void name(CubeFrame& f) {
        if (!f.from.empty()) {
            const std::string c = baseCall(f.from);
            for (const SatDef& s : sats) if (!s.call.empty() && baseCall(s.call) == c) { f.sat = s.name; f.norad = s.norad; return; }
        }
        if (trackedNorad > 0) {
            for (const SatDef& s : sats) if (s.norad == trackedNorad) { f.sat = s.name; f.norad = s.norad; return; }
            f.sat = trackedName; f.norad = trackedNorad;
            return;
        }
        if (dialHz <= 0) return;
        const SatDef* best = nullptr;
        double bestD = 30e3;
        for (const SatDef& s : sats) {
            if (s.framing != f.framing) continue;     // by frequency only a satellite listed with this framing
            const double d = std::fabs(s.mhz * 1e6 - dialHz);
            if (d < bestD) { bestD = d; best = &s; }
        }
        if (best) { f.sat = best->name; f.norad = best->norad; }
    }

    bool dup(const std::vector<uint8_t>& d, int64_t tt) {
        const uint64_t h = hashBytes(d);
        while (!recent.empty() && recent.front().t < tt - 4 * kDupWindow) recent.pop_front();
        for (const Recent& r : recent) if (r.h == h && std::llabs(r.t - tt) < kDupWindow) return true;
        recent.push_back({h, tt});
        return false;
    }

    void fail(int64_t tt) {
        if (fe.carrierRecent(0.2) && tt - fe.carrierSince > (int64_t)(0.05 * kMidRate)) fails.push_back({tt});
    }
    void flushFails() {
        while (!fails.empty() && fails.front().t < fe.n96 - (int64_t)(1.3 * kMidRate)) {
            fails.pop_front();
            t.blocksBad++;
            if (t.blocksBad <= 20) say("CubeSat: a frame failed its checks");
        }
    }

    void accept(CubeFrame f, int64_t tt) {
        // a good frame explains failures of the other demodulators around it
        for (auto it = fails.begin(); it != fails.end();) { if (std::llabs(it->t - tt) < (int64_t)(1.2 * kMidRate)) it = fails.erase(it); else ++it; }
        if (dup(f.data, tt)) return;
        f.timeSec = (double)tt / kMidRate;
        f.wallTime = wallClock ? (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() : 0;
        f.snrDb = (float)fe.snrEma;
        name(f);
        t.blocksOk++;
        t.perFraming[f.framing]++;
        if (f.rsErrors > 0) t.rsFixed++;
        t.lastFrameSec = f.timeSec;
        if (forwardKiss && Feeds::get().wantKiss() && !f.data.empty()) { Feeds::get().kiss(f.data.data(), f.data.size()); f.forwarded = true; t.kissSent++; }
        if (t.blocksOk <= 30) say(std::string("CubeSat: ") + framingShort(f.framing) + " frame" + (f.sat.empty() ? "" : " from " + f.sat) +
                                  (f.from.empty() ? "" : " (" + f.from + ")") + ", " + std::to_string(f.data.size()) + " bytes" +
                                  (f.rsErrors > 0 ? ", " + std::to_string(f.rsErrors) + " byte errors put right" : ""));
        frames.push_front(std::move(f));
        while (frames.size() > kCubeFramesMax) frames.pop_back();
    }

    void onHdlc(int framing, int baud, int variant, const std::vector<uint8_t>& d, bool ok, int64_t tt) {
        if (ok) {
            ax25::Frame a;
            if (!ax25::parseFrame(d.data(), d.size(), a)) { if (variant == 0) fail(tt); return; }
            CubeFrame f;
            f.framing = framing; f.baud = baud;
            f.from = a.from.str(); f.to = a.to.str(); f.path = a.pathStr(); f.type = a.typeName(); f.pid = a.pid;
            f.data = d;
            f.payload.assign(a.info.begin(), a.info.end());
            accept(std::move(f), tt);
            return;
        }
        if (framing == Ax25G3ruh) {     // AX100 Mode 6: an RS block in the HDLC frame
            std::vector<uint8_t> csp;
            int rsErr = -1;
            if (ax100Mode6Decode(d, csp, rsErr)) {
                CubeFrame f;
                f.framing = Ax100Mode6; f.baud = baud; f.rsErrors = rsErr;
                f.hasCsp = parseCsp(csp, f.csp);
                f.data = csp;
                if (csp.size() > 4) f.payload.assign(csp.begin() + 4, csp.end());
                accept(std::move(f), tt);
                return;
            }
        }
        if (variant == 0 && d.size() >= 20) fail(tt);
    }

    void onAsm(int i, bool cc, std::vector<uint8_t>& body, int flags, bool golayFailed) {
        const int64_t tt = plain[i].pos;
        if (golayFailed) { fail(tt); return; }
        CubeFrame f;
        f.baud = i ? 4800 : 9600;
        int rsErr = -1;
        if (cc) {
            std::vector<uint8_t> data;
            if (!ccsdsDecode(body.data(), body.size(), data, rsErr)) { fail(tt); return; }
            f.framing = CcsdsRs; f.rsErrors = rsErr; f.data = data; f.payload = data;
        } else {
            std::vector<uint8_t> csp;
            if (!ax100Mode5Decode(body.data(), body.size(), flags, csp, rsErr)) { fail(tt); return; }
            f.framing = Ax100Mode5; f.rsErrors = (flags & kAx100Rs) ? rsErr : -1;
            f.hasCsp = parseCsp(csp, f.csp);
            f.data = csp;
            if (csp.size() > 4) f.payload.assign(csp.begin() + 4, csp.end());
        }
        accept(std::move(f), tt);
    }

    // ---- 3.9 framings
    void setupPath(ExtraPath& ep, int baud, double afCarrier, bool hdlc, const std::string& why) {
        ep.on = baud > 0; ep.baud = baud; ep.audio = afCarrier > 0; ep.hdlc = hdlc; ep.why = why;
        if (!ep.on) return;
        if (ep.audio) ep.af.init(afCarrier, baud);
        const double pass = std::max(600.0, 0.58 * baud), stop = std::max(pass + 400.0, std::min(1.25 * baud, 46000.0));
        ep.lp.design(dmr::lowpassTaps(pass, stop, kMidRate, 40, 9), 1);
        ep.g3.baud = baud; ep.g3.descramble = true; ep.g3.init(0.70f, 0.80f);
        ep.plain.baud = baud; ep.plain.descramble = false; ep.plain.init(0.70f, 0.85f);
        ep.framer.ccsdsLen = ccsdsLen;
        ep.more.configure(moreCfg);
        ExtraPath* p = &ep;
        ep.plain.bitSink = [p](int b) { p->framer.bit(b); p->more.push(b ? 1.f : -1.f); p->plain.locked = p->framer.inFrame() || p->more.busy(); };
        ep.more.onFrame = [this, p](const MoreFrame& m) { onMore(m, p->baud, p->plain.pos); };
        ep.framer.done = [this, p](bool cc, std::vector<uint8_t>& body, int flags, bool gf) { onAsmAt(p->plain.pos, p->baud, cc, body, flags, gf); };
        ep.g3.hdlc.setCallback([this, p](const std::vector<uint8_t>& d, bool ok) { onHdlc(Ax25G3ruh, p->baud, 1, d, ok, p->g3.pos); });
    }
    // the satellite the pass planner follows: its transmitter from the gr-satellites table chooses the framing parameters and, when the fixed
    // demodulators do not cover its baud rate, an extra one
    void setupTracked() {
        trackTx = trackedNorad > 0 ? grsatChoose(trackedNorad, dialHz) : nullptr;
        MoreConfig c;
        if (trackTx) {
            const int fr = grsatFraming(*trackTx);
            if (fr == CcsdsConcat) c.concatParams = grsatConcat(*trackTx);
            if (fr == Mobitex) c.mobitexVariant = grsatMobitexVariant(*trackTx);
        }
        moreCfg = c;
        for (auto& m : more) m.configure(c);
        afskPath.more.configure(c);
        trackPaths.clear();
        if (!trackTx || !isReadyFs()) return;
        // every decodable transmitter of the satellite on this dial (SPUTNIX satellites send 1200 .. 9600 baud on one frequency)
        for (const GrTx* t : grsatFor(trackedNorad)) {
            if (!grsatReceiverDecodes(*t) || (dialHz > 0 && std::fabs((double)t->hz - dialHz) > 50e3)) continue;
            const bool afsk = std::string(t->modulation) == "AFSK";
            const int fr = grsatFraming(*t);
            const int b = t->baud;
            const double af = afsk ? t->afCarrier : 0;
            if (afsk && (fr == Ax25Afsk1200 || (b == 4800 && t->afCarrier == 3600))) continue;   // the Bell 202 / GOMspace paths
            if (!afsk && (b == 9600 || b == 4800)) continue;                                       // the fixed slicers
            bool have = false;
            for (auto& tp : trackPaths) have |= tp->baud == b && (tp->audio ? tp->af.fc : 0) == af;
            if (have || trackPaths.size() >= 4) continue;
            trackPaths.push_back(std::make_unique<ExtraPath>());
            ExtraPath& ep = *trackPaths.back();
            setupPath(ep, b, af, !afsk && (fr == Ax25G3ruh), t->sat);
            if (t->framing == GrFraming::Ax25) ep.g3.descramble = false;   // plain NRZI AX.25 on FSK
        }
    }
    bool isReadyFs() const { return fs > 0; }
    void runPath(ExtraPath& ep, const std::vector<float>& disc) {
        if (!ep.on) return;
        const std::vector<float>* src = &disc;
        if (ep.audio) { ep.af.run(disc.data(), disc.size(), ep.a); src = &ep.a; }
        ep.f.clear();
        ep.lp.process(src->data(), src->size(), ep.f);
        if (ep.f.empty()) return;
        if (ep.hdlc) ep.g3.process(ep.f.data(), ep.f.size());
        ep.plain.process(ep.f.data(), ep.f.size());
    }
    void onMore(const MoreFrame& m, int baud, int64_t tt) {
        CubeFrame f;
        f.framing = m.framing; f.baud = baud; f.rsErrors = m.rsErrors; f.data = m.data; f.payload = m.data;
        if (m.framing == Usp) {
            ax25::Frame a;
            std::vector<uint8_t> withFcs = m.data;
            if (ax25::parseFrame(withFcs.data(), withFcs.size(), a)) {
                f.from = a.from.str(); f.to = a.to.str(); f.path = a.pathStr(); f.type = a.typeName(); f.pid = a.pid;
                f.payload.assign(a.info.begin(), a.info.end());
            }
        } else if (m.framing == U482c) {
            f.hasCsp = parseCsp(m.data, f.csp);
            if (m.data.size() > 4) f.payload.assign(m.data.begin() + 4, m.data.end());
        } else if (m.framing == Mobitex) {
            f.from = m.callsign;
            f.type = m.name;
        }
        accept(std::move(f), tt);
    }
    void onAsmAt(int64_t tt, int baud, bool cc, std::vector<uint8_t>& body, int flags, bool golayFailed) {
        if (golayFailed) { fail(tt); return; }
        CubeFrame f;
        f.baud = baud;
        int rsErr = -1;
        if (cc) {
            std::vector<uint8_t> data;
            if (!ccsdsDecode(body.data(), body.size(), data, rsErr)) { fail(tt); return; }
            f.framing = CcsdsRs; f.rsErrors = rsErr; f.data = data; f.payload = data;
        } else {
            std::vector<uint8_t> csp;
            if (!ax100Mode5Decode(body.data(), body.size(), flags, csp, rsErr)) { fail(tt); return; }
            f.framing = Ax100Mode5; f.rsErrors = (flags & kAx100Rs) ? rsErr : -1;
            f.hasCsp = parseCsp(csp, f.csp);
            f.data = csp;
            if (csp.size() > 4) f.payload.assign(csp.begin() + 4, csp.end());
        }
        accept(std::move(f), tt);
    }

    void publish() {
        flushFails();
        const double tnow = now();
        t.timeSec = tnow;
        t.carrier = fe.carrierRecent(0.2);
        t.dataValid = tnow - t.lastFrameSec < 30.0;
        t.state = t.dataValid ? 2 : (fe.carrierRecent(5) ? 1 : 0);
        t.cfoHz = fe.cfoHz();
        t.snrDb = (float)fe.snrEma;
        t.levelDb = fe.takeLevelDb();
        t.dialMhz = dialHz / 1e6;
        t.tracked = trackedName; t.trackedNorad = trackedNorad;
        t.trackedSetup = "";
        if (trackTx) {
            t.trackedSetup = grsatDescribe(*trackTx);
            for (size_t k = 0; k < trackPaths.size(); k++) t.trackedSetup += (k ? ", " : "; own demodulators: ") + std::to_string(trackPaths[k]->baud) + " baud" + (trackPaths[k]->audio ? " AFSK" : "");
        }
        if (t.state != lastState) {
            say(t.state == 2 ? "CubeSat: decoding frames" : t.state == 1 ? "CubeSat: a carrier, no good frame lately" : "CubeSat: no signal");
            lastState = t.state;
        }
        CubeTelemetry o = t;
        o.seq = ++seq;
        o.frames.assign(frames.begin(), frames.end());
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        const std::vector<float>& disc = fe.process(x, n);
        if (!disc.empty()) {
            a48.clear();
            audio48.process(disc.data(), disc.size(), a48);
            if (!a48.empty()) for (auto& a : afsk) a.process(a48.data(), a48.size());
            for (int i = 0; i < 2; i++) {
                fg[i].clear();
                lpG[i].process(disc.data(), disc.size(), fg[i]);
                if (fg[i].empty()) continue;
                g3[i].process(fg[i].data(), fg[i].size());
                plain[i].process(fg[i].data(), fg[i].size());
            }
            runPath(afskPath, disc);
            for (auto& tp : trackPaths) runPath(*tp, disc);
        }
        fe.commit();
        while (fe.n96 >= nextReport) {
            nextReport += (int64_t)(0.25 * kMidRate);
            publish();
        }
    }
};

CubesatReceiver::CubesatReceiver() : p_(std::make_unique<Impl>()) {}
CubesatReceiver::~CubesatReceiver() = default;

void CubesatReceiver::configure(double inputRateHz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->configure(inputRateHz); }
void CubesatReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->fe.setOffset(hz); }
void CubesatReceiver::setDialHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    const bool changed = p_->dialHz != hz;
    p_->dialHz = hz;
    if (changed && p_->trackedNorad > 0) p_->setupTracked();   // the transmitter nearest the new dial
}
void CubesatReceiver::setSatellites(const std::vector<SatDef>& s) { std::lock_guard<std::mutex> lk(p_->procMu); p_->sats = s; }
void CubesatReceiver::setTracked(int norad, const std::string& name) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->trackedNorad = norad; p_->trackedName = norad > 0 ? name : "";
    p_->setupTracked();
    if (p_->trackTx) p_->say("CubeSat: set up for " + p_->trackedName + ": " + grsatDescribe(*p_->trackTx));
}
void CubesatReceiver::setForwardKiss(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->forwardKiss = on; }
void CubesatReceiver::setCcsdsLength(int bytes) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->ccsdsLen = std::max(kRsParity + 1, std::min(255, bytes));
    for (auto& f : p_->framer) f.ccsdsLen = p_->ccsdsLen;
}
void CubesatReceiver::setWallClock(bool on) { std::lock_guard<std::mutex> lk(p_->procMu); p_->wallClock = on; }
bool CubesatReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void CubesatReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->isReady) p_->resetState(); }
void CubesatReceiver::clearFrames() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->frames.clear();
    const uint64_t s = p_->seq;
    const double rate = p_->fs;
    p_->t = CubeTelemetry();
    p_->t.inputRate = rate;
    p_->seq = s;
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.frames.clear();
    p_->pub.seq = ++p_->seq;
}
void CubesatReceiver::clearResults() { clearFrames(); }
std::string CubesatReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return results::pack(p_->t.timeSec, p_->frames);
}
bool CubesatReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::deque<CubeFrame> frames;
    if (!results::unpack(blob, t0, frames)) return false;
    std::lock_guard<std::mutex> lk(p_->procMu);
    for (auto& f : frames) f.timeSec += p_->t.timeSec - t0;
    p_->frames = std::move(frames);
    std::lock_guard<std::mutex> tl(p_->telMu);
    p_->pub.frames.assign(p_->frames.begin(), p_->frames.end());
    p_->pub.seq = ++p_->seq;
    return true;
}
void CubesatReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
bool CubesatReceiver::telemetry(CubeTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void CubesatReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }

ModeTuning cubesatTuning() {
    ModeTuning t;
    t.stdMode = 63; t.id = "cubesat"; t.name = "CubeSat telemetry";
    t.minMhz = 130; t.maxMhz = 1000; t.defMhz = 145.825;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.03;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as APRS / Packet)
    return t;
}

} // namespace dect2
