// Inmarsat Aero receiver: a band search finds P-channel carriers (up to 6), each gets a front end (mixer + CIC decimator to about 48 kHz)
// and demodulators for 600, 1200 and 10500 bit/s until one of them holds frame sync; the frames go to the SU layer (aero_su.h).
#include "dect2/session/mode_adapters.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/results/results_io.h"
#include "dect2/aero/aero_demod.h"
#include "dect2/aero/aero_phy.h"
#include "dect2/aero/aero_pos.h"
#include "dect2/aero/aero_su.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kMaxChannels = 12;   // a busy P group (600 / 1200 bit/s) plus the 10500 bit/s carriers; a full table gives way to a wide carrier (below)
constexpr size_t kMaxMessages = 200, kMaxLogons = 100, kMaxAircraft = 100, kMaxTypes = 64, kMaxTrack = 24;
const int kRates[3] = {600, 1200, 10500};

// mixer + 4-stage CIC decimator in integers (wraps cleanly), input scaled to 16 bits
class Front {
public:
    void setup(double fsIn, double freqHz, int decim) {
        fsIn_ = fsIn; D_ = decim;
        setFreq(freqHz);
        osc_ = {1.0, 0.0};
        std::fill(std::begin(iI_), std::end(iI_), 0); std::fill(std::begin(iQ_), std::end(iQ_), 0);
        std::fill(std::begin(cI_), std::end(cI_), 0); std::fill(std::begin(cQ_), std::end(cQ_), 0);
        cnt_ = 0;
        gain_ = 1.0 / (std::pow((double)D_, 4) * 1048576.0);
    }
    void setFreq(double hz) {
        freq_ = hz;
        const double w = -2 * kPi * hz / fsIn_;
        rot_ = {std::cos(w), std::sin(w)};
    }
    double freq() const { return freq_; }
    // returns the number of outputs written to out
    size_t process(const cf32* x, size_t n, cf32* out) {
        size_t m = 0;
        float oR = (float)osc_.real(), oI = (float)osc_.imag();
        const float rR = (float)rot_.real(), rI = (float)rot_.imag();
        constexpr float kScale = 1048576.f;          // 2^20: truncation then leaves a bias far below any carrier
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real() * oR - x[i].imag() * oI;
            const float im = x[i].real() * oI + x[i].imag() * oR;
            const float nr = oR * rR - oI * rI;
            oI = oR * rI + oI * rR;
            oR = nr;
            int64_t a = (int64_t)(re * kScale), b = (int64_t)(im * kScale);
            for (int s = 0; s < 4; s++) { iI_[s] = (int64_t)((uint64_t)iI_[s] + (uint64_t)a); a = iI_[s]; iQ_[s] = (int64_t)((uint64_t)iQ_[s] + (uint64_t)b); b = iQ_[s]; }
            if (++cnt_ < D_) continue;
            cnt_ = 0;
            for (int s = 0; s < 4; s++) {
                const int64_t ta = (int64_t)((uint64_t)a - (uint64_t)cI_[s]); cI_[s] = a; a = ta;
                const int64_t tb = (int64_t)((uint64_t)b - (uint64_t)cQ_[s]); cQ_[s] = b; b = tb;
            }
            out[m++] = cf32((float)((double)a * gain_), (float)((double)b * gain_));
            // keep the oscillator on the unit circle (float rounding), once per output sample
            const float g = 1.5f - 0.5f * (oR * oR + oI * oI);
            oR *= g; oI *= g;
        }
        osc_ = {oR, oI};
        return m;
    }
private:
    double fsIn_ = 1, freq_ = 0, gain_ = 1;
    int D_ = 1, cnt_ = 0;
    std::complex<double> osc_{1, 0}, rot_{1, 0};
    int64_t iI_[4] = {}, iQ_[4] = {}, cI_[4] = {}, cQ_[4] = {};
};

struct Region { double freq, width, peakDb; };
} // namespace

struct AeroReceiver::Impl {
    std::mutex mu;                     // the processing state (feed, configure, reset)
    std::mutex snapMu;                 // the published report
    AeroTelemetry snap;
    std::atomic<uint64_t> seqA{0};
    std::function<void(const std::string&)> log;
    std::function<void(double, const AeroFrameEvent&)> frameCb;

    double rate = 0, offsetHz = 0, searchLimitHz = 0;
    uint64_t samples = 0;
    double sinceReport = 0;
    // band search
    int nfft = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win, acc, accPrev;
    std::vector<cf32> fbuf;
    int fpos = 0, slices = 0, slicesDone = 0;
    uint64_t periodLen = 0, periodPos = 0, sliceGap = 0;
    int period = 0;
    std::vector<std::pair<double, int>> pending;   // carriers seen once (frequency, search number)
    // channels
    struct Channel {
        int id = 0;
        double freq = 0;               // input coordinates (Hz)
        double fsCh = 0;
        Front front;
        std::unique_ptr<AeroDemod> dem[3];
        int active = -1;               // index into kRates once a rate holds sync
        int lastSeen = 0, created = 0;
        double lastSync = 0, lastGood = 0;
        float levelDb = 0, ebn0 = 0;
        uint64_t frames = 0, uwMisses = 0, uwBitErrors = 0, susOk = 0, susBad = 0;
        float ber = 0;
        bool announced = false;
        AeroSuDecoder suDec;
    };
    std::vector<std::unique_ptr<Channel>> chans;
    int nextId = 1;
    std::vector<cf32> tmp;
    std::vector<int> dropIds;
    // decoded tables
    std::map<int, AeroSuTypeCount> types;
    std::vector<AeroLogonEntry> logons;
    std::vector<AeroMessage> messages;
    std::vector<AeroAircraft> aircraft;
    std::vector<AeroSysEntry> sys;
    uint64_t messagesTotal = 0, logonsTotal = 0, framesTotal = 0, susOk = 0, susBad = 0, positionsTotal = 0;
    bool suLayer = false;
    double lastNow = 0;                // stream time of the last report: the kept tables move back by it when the clock starts again

    void shiftTimes(double dt) {
        for (auto& l : logons) l.time += dt;
        for (auto& m : messages) m.time += dt;
        for (auto& a : aircraft) { a.lastHeard += dt; if (a.hasPos) a.posTime += dt; for (auto& p : a.track) p.time += dt; }
    }
    // the decoded tables of a report
    void fillLists(AeroTelemetry& t) const {
        t.suTypes.clear();
        for (auto& kv : types) { if (t.suTypes.size() < kMaxTypes) t.suTypes.push_back(kv.second); }
        t.logons = logons;
        t.messages = messages;
        t.aircraft = aircraft;
        std::sort(t.aircraft.begin(), t.aircraft.end(), [](const AeroAircraft& a, const AeroAircraft& b) { return a.lastHeard > b.lastHeard; });
        t.sys = sys;
        t.messagesTotal = messagesTotal; t.logonsTotal = logonsTotal; t.positionsTotal = positionsTotal;
    }
    void republish() {   // mu held: the tables alone (after a reset, a load or a clear)
        std::lock_guard<std::mutex> lk(snapMu);
        AeroTelemetry t;
        t.inputRate = rate;
        t.seq = snap.seq + 1;
        fillLists(t);
        snap = std::move(t);
    }

    void say(const std::string& s) { if (log) log(s); }
    double now() const { return rate > 0 ? (double)samples / rate : 0; }

    void setup() {
        chans.clear();
        shiftTimes(-lastNow);          // the decoded tables stay (clearResults)
        lastNow = 0;
        framesTotal = susOk = susBad = 0;
        samples = 0; sinceReport = 0; period = 0;
        pending.clear();
        if (rate <= 0) return;
        nfft = 1024;
        while (nfft < 131072 && rate / nfft > 250) nfft <<= 1;
        fft = std::make_unique<Fft>(nfft);
        win.resize(nfft);
        for (int i = 0; i < nfft; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / nfft));
        acc.assign(nfft, 0.f); accPrev.assign(nfft, 0.f);
        fbuf.resize(nfft);
        slices = 16;
        periodLen = (uint64_t)rate;                     // one search a second
        sliceGap = periodLen / (uint64_t)slices;
        if (sliceGap < (uint64_t)nfft) sliceGap = (uint64_t)nfft;
        periodPos = 0; fpos = 0; slicesDone = 0;
    }

    // ---- band search ----
    void searchFeed(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            const uint64_t inSlice = periodPos % sliceGap;
            if (slicesDone < slices && inSlice < (uint64_t)nfft) {
                const size_t take = std::min((size_t)((uint64_t)nfft - inSlice), n - i);
                for (size_t k = 0; k < take; k++) fbuf[fpos + k] = x[i + k] * win[fpos + k];
                fpos += (int)take; i += take; periodPos += take;
                if (fpos == nfft) {
                    fft->forward(fbuf.data());
                    for (int k = 0; k < nfft; k++) acc[k] += std::norm(fbuf[k]);
                    fpos = 0; slicesDone++;
                }
            } else {
                const uint64_t toNext = slicesDone < slices ? sliceGap - inSlice : periodLen - periodPos;
                const size_t skip = (size_t)std::min<uint64_t>(toNext, n - i);
                i += skip; periodPos += skip;
            }
            if (periodPos >= periodLen) {
                if (slicesDone == slices) searchEvaluate();
                periodPos = 0; fpos = 0; slicesDone = 0;
            }
        }
    }

    // table full: a new wide carrier takes the place of the weakest channel that holds no sync (never one that decodes)
    bool freeSlot() {
        int worst = -1;
        for (size_t i = 0; i < chans.size(); i++) {
            const Channel& c = *chans[i];
            if (synced(c) || now() - c.lastSync < 10) continue;
            if (worst < 0 || c.levelDb < chans[(size_t)worst]->levelDb) worst = (int)i;
        }
        if (worst < 0) return false;
        chans.erase(chans.begin() + worst);
        return true;
    }

    void searchEvaluate() {
        period++;
        std::vector<float> p(nfft);
        for (int k = 0; k < nfft; k++) p[k] = acc[k] + accPrev[k];
        accPrev = acc;
        std::fill(acc.begin(), acc.end(), 0.f);
        const double bin = rate / nfft;
        // in frequency order, smoothed over 3 bins
        std::vector<float> s(nfft);
        for (int j = 0; j < nfft; j++) {
            const int k = (j + nfft / 2) % nfft;        // j = 0 is -fs/2
            const int km = (k - 1 + nfft) % nfft, kp = (k + 1) % nfft;
            s[j] = (p[km] + p[k] + p[kp]) / 3;
        }
        auto fOf = [&](int j) { return (j - nfft / 2) * bin; };
        std::vector<float> use;
        std::vector<char> ok(nfft, 0);
        for (int j = 0; j < nfft; j++) {
            const double f = fOf(j);
            if (std::fabs(f) > 0.45 * rate) continue;
            if (searchLimitHz > 0 && std::fabs(f) > searchLimitHz) continue;   // a channelised window: outside its pass-band there is no noise to take a floor from
            if (offsetHz != 0 && std::fabs(f) < 2000) continue;   // the radio's DC spike (a radio is tuned away from the channels)
            ok[j] = 1;
            use.push_back(s[j]);
        }
        if (use.size() < 16) return;
        std::nth_element(use.begin(), use.begin() + use.size() / 2, use.end());
        const float floorP = std::max(use[use.size() / 2], 1e-30f);
        const float thr = floorP * 2.0f;                // 3 dB over the floor after 32 averaged spectra
        std::vector<Region> regs;
        int j = 0;
        while (j < nfft) {
            if (!ok[j] || s[j] <= thr) { j++; continue; }
            int a = j, b = j, gap = 0;
            int k = j + 1;
            while (k < nfft && ok[k] && (s[k] > thr || gap < 2)) {
                if (s[k] > thr) { b = k; gap = 0; } else gap++;
                k++;
            }
            double wsum = 0, fsum = 0, peak = 0;
            for (int q = a; q <= b; q++) {
                const double w = std::max(0.0, (double)s[q] - floorP);
                wsum += w; fsum += w * fOf(q); peak = std::max(peak, (double)s[q]);
            }
            const double width = (b - a + 1) * bin;
            if (wsum > 0 && width < 40000) regs.push_back({fsum / wsum, width, 10 * std::log10(peak / floorP)});
            j = b + 1;
        }
        // a wide (10500 bit/s) carrier spreads its power over about 10 kHz, so its peak is far below a 600 / 1200 bit/s carrier of the same power:
        // it gets a bonus in the order, or the strong narrow carriers (and noise ghosts) would fill every slot first
        auto score = [](const Region& x) { return x.peakDb + (x.width >= 6000 ? 8.0 : 0.0); };
        std::sort(regs.begin(), regs.end(), [&](const Region& x, const Region& y) { return score(x) > score(y); });
        std::vector<std::pair<double, int>> newPending;
        for (const Region& r : regs) {
            Channel* m = nullptr;
            for (auto& c : chans) {
                const double cf = chanFreq(*c);
                // a 10500 bit/s carrier is about 10 kHz wide: at low levels its spectrum can break into pieces
                const double reach = c->active >= 0 && kRates[c->active] == 10500 ? 7000.0 : std::max(1500.0, r.width / 2);
                if (std::fabs(r.freq - cf) < reach) { m = c.get(); break; }
            }
            if (m) {
                m->lastSeen = period;
                m->levelDb = (float)r.peakDb;
                if (!synced(*m) && now() - m->lastSync > 3 && std::fabs(r.freq - m->freq) > 400) retune(*m, r.freq);
            } else if ((int)chans.size() < kMaxChannels || r.width >= 6000) {
                // a new carrier must show in two searches in a row: noise alone crosses the threshold now and then, never twice in one place
                bool seen = false;
                for (const auto& q : pending) seen |= q.second == period - 1 && std::fabs(q.first - r.freq) < std::max(1500.0, r.width / 2);
                newPending.push_back({r.freq, period});
                if (!seen) continue;
                if ((int)chans.size() >= kMaxChannels && !freeSlot()) continue;
                auto c = std::make_unique<Channel>();
                c->id = nextId++;
                c->created = c->lastSeen = period;
                c->levelDb = (float)r.peakDb;
                c->lastSync = now();
                chans.push_back(std::move(c));
                retune(*chans.back(), r.freq);
            }
        }
        pending.swap(newPending);
        // channels that went away, and carriers that never gave a frame
        for (size_t i = 0; i < chans.size();) {
            Channel& c = *chans[i];
            const bool gone = period - c.lastSeen > 8 && !synced(c);
            const bool useless = !synced(c) && now() - c.lastSync > 40;
            if (gone || useless) {
                if (c.announced) say("Aero: channel at " + khz(chanFreq(c)) + " lost");
                chans.erase(chans.begin() + (long)i);
            } else {
                i++;
            }
        }
    }

    std::string khz(double fInput) const {
        char b[64];
        std::snprintf(b, sizeof b, "%+.3f kHz", (fInput - offsetHz) / 1000);
        return b;
    }

    bool synced(const Channel& c) const {
        for (int k = 0; k < 3; k++) if (c.dem[k] && c.dem[k]->synced()) return true;
        return false;
    }
    double chanFreq(const Channel& c) const {
        if (c.active >= 0 && c.dem[c.active]) return c.freq + c.dem[c.active]->freqHz();
        for (int k = 0; k < 3; k++) if (c.dem[k] && c.dem[k]->acquired()) return c.freq + c.dem[k]->freqHz();
        return c.freq;
    }

    void retune(Channel& c, double f) {
        c.freq = f;
        const int D = std::max(1, (int)std::floor(rate / 48000.0));
        c.fsCh = rate / D;
        c.front.setup(rate, f, D);
        c.active = -1;
        for (int k = 0; k < 3; k++) makeDemod(c, k);
    }
    void makeDemod(Channel& c, int k) {
        c.dem[k] = std::make_unique<AeroDemod>(kRates[k], c.fsCh, kRates[k] == 10500 ? 1500.0 : 700.0);
        Channel* cp = &c;
        c.dem[k]->setCallback([this, cp, k](AeroFrameEvent& e) { onFrame(*cp, k, e); });
    }

    // ---- frames -> SUs ----
    void onFrame(Channel& c, int k, AeroFrameEvent& e) {
        c.frames++;
        c.uwBitErrors += e.uwErrors > 0 ? (uint64_t)e.uwErrors : 0;
        if (e.uwErrors < 0) c.uwMisses++;
        c.susOk += (uint64_t)e.susOk; c.susBad += (uint64_t)e.susBad;
        c.ber = e.channelBer;
        c.ebn0 = e.ebn0Db;
        susOk += (uint64_t)e.susOk; susBad += (uint64_t)e.susBad;
        framesTotal++;
        c.lastSync = now();
        if (e.susOk) c.lastGood = now();
        if (frameCb) frameCb(c.freq + e.freqHz - offsetHz, e);
        if (c.active != k && e.susOk >= (int)(e.bytes.size() / 24)) {      // at least half the SUs good: this is the rate
            c.active = k;
            for (int q = 0; q < 3; q++) if (q != k) c.dem[q].reset();
            if (!c.announced) {
                c.announced = true;
                char b[160];
                std::snprintf(b, sizeof b, "Aero: P channel at %s, %d bit/s, Eb/N0 %.1f dB", khz(chanFreq(c)).c_str(), e.bitRate, e.ebn0Db);
                say(b);
            }
        }
        const size_t bb = aeroBlockBytes(e.bitRate);
        if (!bb || e.bytes.size() % bb) { suLayer = false; return; }
        suLayer = true;
        std::vector<AeroAcars> acars;
        std::vector<AeroLogon> lgs;
        const int chIdx = channelIndex(c);
        for (size_t off = 0; off < e.bytes.size(); off += bb) {
            const std::vector<AeroSu> sus = aeroSplitSus(e.bytes.data() + off, bb, e.bitRate);
            for (const AeroSu& su : sus) {
                auto& t = types[su.type];
                t.type = su.type;
                if (t.name.empty()) t.name = su.typeName;
                t.count++;
                if (su.crcOk) c.suDec.feed(su, now(), acars, lgs);
                if (su.crcOk) noteSystem(su);
            }
        }
        const int64_t wall = (int64_t)std::time(nullptr);
        for (const AeroLogon& l : lgs) {
            AeroLogonEntry le;
            le.time = now(); le.wallTime = wall; le.channel = chIdx;
            le.aesId = l.aesId; le.gesId = l.gesId; le.logon = l.logon;
            logons.push_back(le);
            if (logons.size() > kMaxLogons) logons.erase(logons.begin());
            logonsTotal++;
            AeroAircraft& a = plane(l.aesId, "");
            a.loggedOn = l.logon;
            a.lastHeard = now(); a.lastWall = wall;
            char b[96];
            std::snprintf(b, sizeof b, "Aero: %s AES %06X GES %d", l.logon ? "logon" : "logoff", l.aesId, l.gesId);
            say(b);
        }
        for (const AeroAcars& m : acars) {
            AeroMessage am;
            am.time = now(); am.wallTime = wall; am.channel = chIdx; am.bitRate = e.bitRate;
            am.aesId = m.aesId; am.gesId = m.gesId; am.uplink = m.uplink;
            am.mode = m.mode; am.registration = m.registration; am.flight = m.flight; am.label = m.label; am.labelText = m.labelText;
            am.blockId = m.blockId; am.msgNo = m.msgNo; am.text = m.text.substr(0, 240); am.crcOk = m.crcOk;
            AeroPosition pos;
            if (m.crcOk) aeroPositionFromMessage(m.label, m.text, m.uplink, pos, &am.decoded);
            if (pos.valid && !aeroSanitizePosition(pos.lat, pos.lon)) pos.valid = false;   // out of range or no fix: not a position
            if (pos.valid) { am.hasPos = true; am.lat = pos.lat; am.lon = pos.lon; }
            const double heardOff = chanFreq(c) - offsetHz;
            am.heardOffsetHz = heardOff;
            if (automation::wants(automation::FMsg)) {   // "Save decoded messages", Integrations, automations (automation/engine.h)
                char aes[16];
                std::snprintf(aes, sizeof aes, "%06X", am.aesId);
                automation::Event ex;
                ex.str("registration", am.registration).str("flight", am.flight).str("label", am.label).str("label_text", am.labelText).str("aes", aes)
                  .flag("uplink", am.uplink).flag("crc_ok", am.crcOk).num("bit_rate", am.bitRate).num("ges_id", am.gesId).num("heard_offset_hz", am.heardOffsetHz);
                if (!am.decoded.empty()) ex.str("decoded", am.decoded);
                if (am.hasPos) ex.num("lat", am.lat).num("lon", am.lon);
                automation::emitMessage("aero", "Inmarsat Aero", 0, am.registration.empty() ? std::string(aes) : am.registration, am.text, e.ebn0Db, std::move(ex));
            }
            messages.push_back(am);
            if (messages.size() > kMaxMessages) messages.erase(messages.begin());
            messagesTotal++;
            AeroAircraft& a = plane(m.aesId, m.registration);
            if (!m.registration.empty()) a.registration = m.registration;
            if (!m.flight.empty()) a.flight = m.flight;
            a.lastLabel = m.label;
            a.lastText = m.text.substr(0, 80);
            a.messages++;
            a.lastHeard = now(); a.lastWall = wall;
            a.heardBitRate = e.bitRate; a.heardOffsetHz = heardOff;
            if (pos.valid) addPosition(a, pos, wall);
            std::string line = "Aero: ACARS " + (m.registration.empty() ? std::string("?") : m.registration) + " " + m.label + (m.flight.empty() ? "" : " " + m.flight) + " ";
            line += m.text.substr(0, 60);
            for (auto& ch : line) if ((unsigned char)ch < 32) ch = ' ';
            say(line);
        }
    }

    // the system table: satellite id (with its longitude) and the P/R channel control: kept once per kind and identity, counted
    void noteSystem(const AeroSu& su) {
        const AeroSysInfo si = aeroParseSystemSu(su);
        if (si.kind != 2 && si.kind != 3) return;
        for (auto& e : sys) {
            if (e.kind == si.kind && e.satId == si.satId && e.gesId == si.gesId && std::fabs(e.pMHz - si.freqMHz[0]) < 1e-6) { e.count++; e.wallTime = (int64_t)std::time(nullptr); return; }
        }
        if (sys.size() >= 24) return;
        AeroSysEntry e;
        e.kind = si.kind; e.satId = si.satId; e.lonDeg = si.lonDeg; e.gesId = si.gesId; e.bitRate = si.bitRate; e.pMHz = si.freqMHz[0];
        e.spotBeam = si.spotBeam; e.count = 1; e.wallTime = (int64_t)std::time(nullptr);
        sys.push_back(e);
    }

    void addPosition(AeroAircraft& a, const AeroPosition& p, int64_t wall) {
        positionsTotal++;
        a.positions++;
        if (a.flight.empty() && !p.flightId.empty()) a.flight = p.flightId;
        // with no track in the report, the bearing from the last position (when it moved more than about 1 km)
        double trk = p.trackDeg;
        bool hasTrk = p.hasTrack;
        if (!hasTrk && a.hasPos) {
            const double p1 = a.lat * kPi / 180, p2 = p.lat * kPi / 180, dl = (p.lon - a.lon) * kPi / 180;
            const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
            if (std::hypot(p.lat - a.lat, (p.lon - a.lon) * std::cos(p2)) > 0.01) {
                hasTrk = true;
                trk = std::fmod(std::atan2(y, x) * 180 / kPi + 360, 360);
            }
        }
        if (p.icao) a.icao = p.icao;
        a.hasPos = true; a.lat = p.lat; a.lon = p.lon;
        a.hasAlt = p.hasAlt; a.altFt = p.altFt;
        a.hasTrack = hasTrk; a.trackDeg = trk;
        a.hasSpeed = p.hasSpeed; a.speedKt = p.speedKt;
        a.posSource = p.source; a.posKind = p.kind;
        a.posTime = now(); a.posWall = wall;
        a.reportSecPastHour = p.secPastHour; a.reportSecOfDay = p.secOfDay;
        a.route = p.route;
        if (a.track.empty() || a.track.back().lat != p.lat || a.track.back().lon != p.lon) {
            a.track.push_back({p.lat, p.lon, p.altFt, now()});
            if (a.track.size() > kMaxTrack) a.track.erase(a.track.begin());
        }
        char b[160];
        std::snprintf(b, sizeof b, "Aero: position %s %.4f %.4f", a.registration.empty() ? "?" : a.registration.c_str(), p.lat, p.lon);
        std::string line = b;
        if (p.hasAlt) line += " " + std::to_string(p.altFt) + " ft";
        say(line + " (" + p.kind + ")");
    }

    int channelIndex(const Channel& c) const {
        for (size_t i = 0; i < chans.size(); i++) if (chans[i].get() == &c) return (int)i;
        return 0;
    }
    AeroAircraft& plane(uint32_t aes, const std::string& reg) {
        for (auto& a : aircraft) if ((aes && a.aesId == aes) || (!aes && !reg.empty() && a.registration == reg)) return a;
        if (aircraft.size() >= kMaxAircraft) {
            auto oldest = std::min_element(aircraft.begin(), aircraft.end(), [](const AeroAircraft& x, const AeroAircraft& y) { return x.lastHeard < y.lastHeard; });
            aircraft.erase(oldest);
        }
        AeroAircraft a;
        a.aesId = aes; a.registration = reg;
        aircraft.push_back(a);
        return aircraft.back();
    }

    // ---- processing ----
    void process(const cf32* x, size_t n) {
        searchFeed(x, n);
        for (auto& cp : chans) {
            Channel& c = *cp;
            if (tmp.size() < n) tmp.resize(n);
            const size_t m = c.front.process(x, n, tmp.data());
            if (!m) continue;
            for (int k = 0; k < 3; k++) if (c.dem[k]) c.dem[k]->feed(tmp.data(), m);
            // two channels on one carrier (a wide carrier seen as two pieces): keep the older one
            if (synced(c))
                for (auto& o : chans)
                    if (o.get() != &c && o->id < c.id && synced(*o) && std::fabs(chanFreq(*o) - chanFreq(c)) < 1500) dropIds.push_back(c.id);
            // the rate that held sync lost it for a long time: try all three again
            if (c.active >= 0 && !synced(c) && now() - c.lastSync > 20) {
                c.active = -1;
                for (int k = 0; k < 3; k++) if (!c.dem[k]) makeDemod(c, k);
            }
        }
        samples += n;
        for (int id : dropIds)
            for (size_t i = 0; i < chans.size(); i++)
                if (chans[i]->id == id) { chans.erase(chans.begin() + (long)i); break; }
        dropIds.clear();
    }

    void report() {
        AeroTelemetry t;
        t.inputRate = rate;
        t.timeSec = now();
        lastNow = t.timeSec;
        double bestDist = 1e18;
        int nSync = 0, nLock = 0;
        for (auto& cp : chans) {
            Channel& c = *cp;
            AeroChannelInfo ci;
            ci.offsetHz = chanFreq(c) - offsetHz;
            const AeroDemod* d = c.active >= 0 ? c.dem[c.active].get() : nullptr;
            if (!d) for (int k = 0; k < 3; k++) if (c.dem[k] && c.dem[k]->synced()) d = c.dem[k].get();
            if (!d) for (int k = 0; k < 3; k++) if (c.dem[k] && c.dem[k]->acquired()) d = c.dem[k].get();
            ci.bitRate = c.active >= 0 ? kRates[c.active] : (d && d->synced() ? d->bitRate() : 0);
            ci.state = 0;
            if (d && d->acquired()) ci.state = 1;
            if (d && d->synced()) ci.state = 2;
            if (d && d->dataLock()) ci.state = 3;
            ci.ebn0Db = d && d->acquired() ? d->ebn0Db() : 0.f;
            ci.levelDb = c.levelDb;
            ci.cfoHz = d && d->acquired() ? d->freqHz() : 0;
            ci.frames = c.frames; ci.uwMisses = c.uwMisses; ci.uwBitErrors = c.uwBitErrors;
            ci.susOk = c.susOk; ci.susBad = c.susBad; ci.channelBer = c.ber; ci.lastHeard = c.lastGood;
            nSync += ci.state >= 2;
            if (ci.state == 3) {
                nLock++;
                if (std::fabs(ci.offsetHz) < bestDist) { bestDist = std::fabs(ci.offsetHz); t.cfoHz = ci.offsetHz; t.snrDb = ci.ebn0Db; }
            }
            t.channels.push_back(ci);
        }
        t.state = chans.empty() ? 0 : nLock ? 2 : 1;
        t.blocksOk = susOk; t.blocksBad = susBad;
        t.dataValid = nLock > 0 && (messagesTotal > 0 || logonsTotal > 0 || !suLayer);
        fillLists(t);
        t.frames = framesTotal;
        t.suLayer = suLayer;
        (void)nSync;
        std::lock_guard<std::mutex> lk(snapMu);
        t.seq = snap.seq + 1;
        snap = std::move(t);
    }
};

AeroReceiver::AeroReceiver() : p_(std::make_unique<Impl>()) {}
AeroReceiver::~AeroReceiver() = default;

void AeroReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->setup();
}
void AeroReceiver::setSearchLimit(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->searchLimitHz = hz;
}
void AeroReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    p_->chans.clear();
}
bool AeroReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= aeroTuning().minSampleRate - 1;
}
void AeroReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->setup();
    p_->republish();                     // the report number never restarts; the tables stay
}
void AeroReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->sys.clear(); p_->types.clear(); p_->logons.clear(); p_->messages.clear(); p_->aircraft.clear();
    p_->messagesTotal = p_->logonsTotal = p_->positionsTotal = 0;
    p_->republish();
}
std::string AeroReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl& I = *p_;
    return results::pack(I.lastNow, I.types, I.logons, I.messages, I.aircraft, I.messagesTotal, I.logonsTotal, I.positionsTotal);
}
bool AeroReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::map<int, AeroSuTypeCount> types;
    std::vector<AeroLogonEntry> logons;
    std::vector<AeroMessage> messages;
    std::vector<AeroAircraft> aircraft;
    uint64_t mt = 0, lt = 0, pt = 0;
    if (!results::unpack(blob, t0, types, logons, messages, aircraft, mt, lt, pt)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    I.types = std::move(types); I.logons = std::move(logons); I.messages = std::move(messages); I.aircraft = std::move(aircraft);
    I.messagesTotal = mt; I.logonsTotal = lt; I.positionsTotal = pt;
    I.shiftTimes(I.lastNow - t0);
    I.republish();
    return true;
}
void AeroReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->rate < aeroTuning().minSampleRate - 1) return;
    const double per = 0.25 * p_->rate;
    while (n) {
        // stop at report boundaries so a report reflects the stream up to that point
        const size_t until = (size_t)std::max(1.0, std::ceil(per - p_->sinceReport));
        const size_t m = std::min(n, until);
        p_->process(x, m);
        p_->sinceReport += (double)m;
        x += m; n -= m;
        if (p_->sinceReport >= per) { p_->sinceReport -= per; p_->report(); }
    }
}
bool AeroReceiver::telemetry(AeroTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->snapMu);
    if (p_->snap.seq <= lastSeq) return false;
    out = p_->snap;
    sess::observeAero(out);
    return true;
}
void AeroReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void AeroReceiver::setFrameCallback(std::function<void(double, const AeroFrameEvent&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->frameCb = std::move(cb); }

ModeTuning aeroTuning() {
    ModeTuning t;
    t.stdMode = 20; t.id = "aero"; t.name = "Inmarsat Aero";
    t.minMhz = 1525; t.maxMhz = 1559; t.defMhz = 1545.57;   // between the 600/1200 group (~1545.1) and 10500 bit/s (~1546.0)
    t.sampleRate = 4000000;              // Auto: a 4 Msps radio sees both P groups; a slower radio runs as fast as it can
    t.basebandHz = 3500000;
    t.bandwidthMhz = 0.02;
    t.minSampleRate = 240000;
    t.tuneOffsetHz = 50000;
    return t;
}

} // namespace dect2
