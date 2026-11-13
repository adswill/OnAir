// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R receiver (gsmr_rx.h): blocks of input, 200 kHz carriers found in the spectrum, base stations synchronised and their BCCH decoded.
#include "dect2/common/thread_name.h"
#include "dect2/wirecap/wire.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/gsmr/gsmr_phy.h"
#include "dect2/gsmr/gsmr_si.h"
#include "dect2/gsmr/gsmr_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {

// ---------------------------------------------------------------- channel raster and operators

double gsmrArfcnToHz(int n) {
    if (n >= 0 && n <= 124) return (935.0 + 0.2 * n) * 1e6;
    if (n >= 955 && n <= 1023) return (935.0 + 0.2 * (n - 1024)) * 1e6;
    if (n >= 512 && n <= 885) return (1805.2 + 0.2 * (n - 512)) * 1e6;   // DCS 1800
    if (n >= 128 && n <= 251) return (869.2 + 0.2 * (n - 128)) * 1e6;    // GSM 850
    return 0;
}

double gsmrRasterHz(double hz) {   // 3GPP TS 45.005 2: the channel the frequency is on, in its own band
    const int n = gsmrHzToArfcn(hz);
    if (n < 0) return 0;
    if (hz >= 1930.1e6 && hz <= 1989.9e6) return (1930.2 + 0.2 * (n - 512)) * 1e6;   // PCS 1900
    return gsmrArfcnToHz(n);
}

const char* gsmrBandName(double hz) {
    const double m = hz / 1e6;
    if (m >= 869.1 && m <= 893.9) return "GSM 850";
    if (m >= 921.1 && m <= 924.9) return "GSM-R";
    if (m >= 925.1 && m <= 959.9) return "GSM 900";
    if (m >= 1805.1 && m <= 1879.9) return "DCS 1800";
    if (m >= 1930.1 && m <= 1989.9) return "PCS 1900";
    return "";
}

int gsmrHzToArfcn(double hz) {
    const double m = hz / 1e6;
    if (m >= 1805.1 && m <= 1879.9) return 512 + (int)std::lround((m - 1805.2) / 0.2);   // DCS 1800
    if (m >= 1930.1 && m <= 1989.9) return 512 + (int)std::lround((m - 1930.2) / 0.2);   // PCS 1900 (the same numbers as DCS 1800: the band tells them apart)
    if (m >= 869.1 && m <= 893.9) return 128 + (int)std::lround((m - 869.2) / 0.2);      // GSM 850
    if (m < 921.1 || m > 959.9) return -1;
    const long k = std::lround((m - 935.0) / 0.2);
    if (k >= 0) return (int)k;
    return (int)(1024 + k);
}

bool gsmrIsRailway(int n) { return n >= 955 && n <= 973; }

const char* gsmrOperatorName(int mcc, int mnc) {
    struct O { int mcc, mnc; const char* name; };
    static const O t[] = {
        {1, 1, "Test network"},
        {262, 10, "DB Netz (GSM-R)"}, {208, 14, "SNCF Reseau (GSM-R)"}, {228, 6, "SBB (GSM-R)"},
        {232, 91, "OBB (GSM-R)"}, {234, 12, "Network Rail (GSM-R)"}, {222, 30, "RFI (GSM-R)"},
    };
    for (const auto& o : t) if (o.mcc == mcc && o.mnc == mnc) return o.name;
    return "";
}

const char* gsmrCountry(int mcc) {
    struct C { int mcc; const char* name; };
    static const C t[] = {{1, "test"}, {262, "Germany"}, {208, "France"}, {228, "Switzerland"}, {232, "Austria"}, {234, "United Kingdom"},
                          {222, "Italy"}, {204, "Netherlands"}, {206, "Belgium"}, {214, "Spain"}, {260, "Poland"}, {230, "Czechia"},
                          {240, "Sweden"}, {242, "Norway"}, {238, "Denmark"}, {244, "Finland"}, {250, "Russia"}, {255, "Ukraine"}, {246, "Lithuania"},
                          {247, "Latvia"}, {248, "Estonia"}, {268, "Portugal"}, {202, "Greece"}, {226, "Romania"}, {284, "Bulgaria"}, {216, "Hungary"}};
    for (const auto& c : t) if (c.mcc == mcc) return c.name;
    return "";
}

// ---------------------------------------------------------------- receiver

ModeTuning gsmrTuning() {
    ModeTuning t;
    t.stdMode = 41; t.id = "gsmr"; t.name = "GSM-R";
    t.minMhz = 860; t.maxMhz = 1990; t.defMhz = 922.8;
    t.sampleRate = 5000000;      // the whole railway band (921 - 925 MHz) in one capture
    t.basebandHz = 5000000;
    t.bandwidthMhz = 4.8;
    t.minSampleRate = 1000000;   // five channels or so
    return t;
}

namespace {

constexpr double kBlockSec = 2.2;

int64_t wallNow() { return dect2::unixNow(); }

struct Carrier { double off; float levelDb; float powerDb; };

struct Decoded {
    bool synced = false;
    int bsic = -1;
    uint32_t fn = 0;
    double cfoHz = 0;       // the carrier's offset from the nominal channel centre
    float snrDb = -99;
    int bad = 0;
    std::vector<gsmr::Bcch> msgs;
    gsmr::CcchCount ccch;   // the CCCH blocks of the capture by message kind (no contents are kept)
};

} // namespace

struct GsmrReceiver::Impl {
    mutable std::mutex mu;
    double rateSet = 0, sigOff = 0, centerHz = 0;
    std::function<void(const std::string&)> log;
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> idle{true};
    std::atomic<bool> clearReq{false};
    // capture (analysis thread)
    std::vector<cf32> acc;
    size_t want = 0;
    uint64_t accGen = 0;
    double accCenter = 0, accOff = 0;
    uint64_t total = 0;
    double levelAcc = 0; uint64_t levelN = 0;
    double lastPubSec = -1;
    // worker
    std::thread th;
    std::condition_variable cv;
    bool stop = false, jobReady = false;
    std::vector<cf32> job;
    double jobCenter = 0, jobOff = 0, jobRate = 0, jobSec = 0;
    uint64_t jobGen = 0;
    // results (under mu)
    GsmrTelemetry tel, pub;

    Impl() { th = dect2::namedThread("onair-gsmr", [this] { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void event(const std::string& s, double sec) {
        GsmrEvent e; e.wallTime = wallNow(); e.sec = sec; e.text = s;
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kGsmrLogMax) tel.events.resize(kGsmrLogMax);
        if (log) log("gsmr: " + s);
    }

    void publish() { tel.seq++; pub = tel; }

    void run() {
        for (;;) {
            std::vector<cf32> x;
            double center, off, rate, sec; uint64_t g;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || jobReady; });
                if (stop) return;
                x.swap(job); jobReady = false;
                center = jobCenter; off = jobOff; rate = jobRate; sec = jobSec; g = jobGen;
            }
            process(x, center, off, rate, sec, g);
            idle = true;
        }
    }

    // 200 kHz humps in the spectrum: offsets of their centres from 0 Hz
    std::vector<Carrier> carriers(const std::vector<cf32>& x, double fs) {
        std::vector<Carrier> out;
        const int nfft = cell::pow2At((int)(fs / 10e3));
        if (nfft < 64 || x.size() < (size_t)nfft * 8) return out;
        const std::vector<float> p = cell::welch(x.data(), x.size(), nfft, 300);
        const double bin = fs / nfft;
        const int half = std::max(1, (int)std::lround(100e3 / bin));   // a window 200 kHz wide
        std::vector<double> w((size_t)nfft, 0.0);
        {
            double s = 0;
            for (int i = 0; i < nfft && i <= half; i++) s += p[(size_t)i];
            for (int i = 0; i < nfft; i++) {
                w[(size_t)i] = s;
                const int add = i + half + 1, sub = i - half;
                if (add < nfft) s += p[(size_t)add];
                if (sub >= 0) s -= p[(size_t)sub];
            }
        }
        std::vector<double> srt = w;
        std::nth_element(srt.begin(), srt.begin() + nfft / 4, srt.end());
        const double floorW = std::max(1e-30, srt[(size_t)(nfft / 4)]);
        const int edge = std::max(half + 1, (int)(nfft * 0.04));
        std::vector<Carrier> cand;
        for (int i = edge; i < nfft - edge; i++) {
            const double v = w[(size_t)i];
            if (v < floorW * 5.0) continue;   // 7 dB over the floor
            bool top = true;
            for (int k = -half; k <= half && top; k++) { if (k && w[(size_t)(i + k)] > v) top = false; else if (k < 0 && w[(size_t)(i + k)] == v) top = false; }
            if (!top) continue;
            Carrier c;
            c.off = (i - nfft / 2.0) * bin;
            c.levelDb = (float)(10 * std::log10(v / floorW));
            c.powerDb = (float)(10 * std::log10(v / (0.375 * nfft) + 1e-30));   // dBFS: the Hann window's power loss, the bins' normalisation
            cand.push_back(c);
        }
        std::sort(cand.begin(), cand.end(), [](const Carrier& a, const Carrier& b) { return a.levelDb > b.levelDb; });
        if (cand.size() > 12) cand.resize(12);
        return cand;
    }

    // Synchronise to one carrier and decode every BCCH block in the capture
    Decoded decodeCarrier(const std::vector<cf32>& x, double fs, double off, uint64_t g) {
        using namespace gsmr;
        Decoded d;
        // 1. a first look at the start of the capture finds the frequency correction bursts and the carrier's exact frequency
        size_t nA = std::min(x.size(), (size_t)(fs * 0.5));
        std::vector<cf32> a = channelize(x.data(), x.size(), fs, off, 0, nA);
        std::vector<FcchHit> h = findFcch(a.data(), a.size());
        if (h.empty()) return d;
        if (gen.load() != g) return d;
        std::vector<double> errs;
        for (const auto& f : h) errs.push_back(f.freqErrHz);
        std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
        double refined = off + errs[errs.size() / 2];
        // 2. the whole capture, centred on the carrier
        std::vector<cf32> s = channelize(x.data(), x.size(), fs, refined, 0, x.size());
        h = findFcch(s.data(), s.size());
        if (h.empty() || gen.load() != g) return d;
        double resid = 0;
        { std::vector<double> e; for (const auto& f : h) e.push_back(f.freqErrHz); std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end()); resid = e[e.size() / 2]; }
        refined += resid;
        d.cfoHz = refined - off;   // relative to the spectrum peak; the caller turns it into the offset from the channel centre
        if (std::fabs(resid) > 150) {   // a better centre: do it again for the bursts to sit at their nominal frequency
            s = channelize(x.data(), x.size(), fs, refined, 0, x.size());
            h = findFcch(s.data(), s.size());
            if (h.empty()) return d;
        }
        d.cfoHz = refined;   // offset of the true carrier from 0 Hz in the input
        std::sort(h.begin(), h.end(), [](const FcchHit& p, const FcchHit& q) { return p.coherence > q.coherence; });
        // 3. the SCH that follows an FCCH by one frame
        double schPos = 0;
        bool have = false;
        for (const auto& f : h) {
            const double fcchStart = f.center - 74.0 * kSps;
            const BurstRx b = demodBurst(s.data(), s.size(), fcchStart + kFrameSamples, 12, kSync, 64, 42);
            if (!b.ok) continue;
            float l78[78];
            for (int i = 0; i < 39; i++) { l78[i] = b.llr[3 + i]; l78[39 + i] = b.llr[106 + i]; }
            int bsic; uint32_t fn;
            if (!schDecode(l78, bsic, fn)) continue;
            d.synced = true; d.bsic = bsic; d.fn = fn; d.snrDb = b.snrDb;
            schPos = b.pos;
            have = true;
            break;
        }
        if (!have) return d;
        // 4. every BCCH block: frames with FN mod 51 = 2 .. 5, TSC = BCC
        const int tsc = d.bsic & 7;
        double drift = 0;
        const long fnFirst = (long)d.fn - (long)std::floor(schPos / kFrameSamples) - 1;   // a frame that starts before the capture
        const long fnLast = (long)d.fn + (long)std::floor(((double)s.size() - schPos) / kFrameSamples) + 1;
        for (long f0 = fnFirst; f0 <= fnLast; f0++) {
            const int t51 = (int)(((f0 % 51) + 51) % 51);
            // BCCH at 2 - 5; CCCH blocks at 6, 12, 16, 22, 26, 32, 36, 42, 46 (TS 45.002 7 Table 3, non-combined; combined uses 6, 12, 16 only)
            const bool ccchBlk = t51 == 6 || t51 == 12 || t51 == 16 || t51 == 22 || t51 == 26 || t51 == 32 || t51 == 36 || t51 == 42 || t51 == 46;
            if (t51 != 2 && !ccchBlk) continue;
            const double first = schPos + (double)(f0 - (long)d.fn) * kFrameSamples;
            if (first < -200 || first + 3.0 * kFrameSamples + 700 > (double)s.size()) continue;
            if (gen.load() != g) return d;
            BurstRx br[4];
            const float* dat[4];
            std::vector<float> storage(4 * 114);
            bool all = true;
            for (int b = 0; b < 4; b++) {
                const double pred = first + (double)b * kFrameSamples + drift;
                br[b] = demodBurst(s.data(), s.size(), pred, b == 0 ? 10 : 6, kTsc[tsc], 26, 61);
                float* v = &storage[(size_t)b * 114];
                if (!br[b].ok) { all = false; for (int i = 0; i < 114; i++) v[i] = 0; }
                else {
                    drift = br[b].pos - (first + (double)b * kFrameSamples);
                    for (int i = 0; i < 57; i++) { v[i] = br[b].llr[3 + i]; v[57 + i] = br[b].llr[88 + i]; }
                }
                dat[b] = v;
            }
            float l456[456];
            xcchDeinterleave(dat, l456);
            uint8_t m[23];
            if (ccchBlk) {   // only the message kind is counted; the contents (paging identities) are neither read nor kept
                if (xcchDecode(l456, m)) d.ccch.add(ccchKind(m)); else if (all) d.ccch.bad++;
                continue;
            }
            if (xcchDecode(l456, m)) {
                if (wire::on(wire::GsmR)) wire::Hub::get().publish(wire::GsmR, wire::makeGsmtap(wire::gsmtap::kTypeUm, wire::gsmtap::kChanBcch, m, sizeof m));
                Bcch bc;
                if (parseBcch(m, bc)) d.msgs.push_back(bc);   // a good frame that is not system information (filler, a message of another kind) is not an error
            } else if (all) d.bad++;
        }
        return d;
    }

    void process(const std::vector<cf32>& x, double center, double dialOff, double fs, double sec, uint64_t g) {
        (void)dialOff;
        const std::vector<Carrier> car = carriers(x, fs);
        std::vector<std::pair<Carrier, Decoded>> found;
        std::vector<GsmrCarrierSeen> seen;
        for (const auto& c : car) {
            if (gen.load() != g) return;   // retuned meanwhile: these samples belong to another frequency
            Decoded d = decodeCarrier(x, fs, c.off, g);
            GsmrCarrierSeen cs; cs.freqHz = (center > 0 ? center : 0.0) + (d.synced ? d.cfoHz : c.off); cs.levelDb = c.levelDb; cs.synced = d.synced;
            seen.push_back(cs);
            if (d.synced) found.push_back({c, std::move(d)});
        }
        std::lock_guard<std::mutex> lk(mu);
        if (gen.load() != g) return;
        if (clearReq.exchange(false)) { tel.cells.clear(); tel.events.clear(); tel.blocksOk = tel.blocksBad = 0; }
        tel.blocks++;
        tel.carriers = seen;
        tel.lastBlockCenterHz = center;
        for (auto& fr : found) merge(fr.first, fr.second, center, sec);
        std::stable_sort(tel.cells.begin(), tel.cells.end(), [](const GsmrCell& a, const GsmrCell& b) { return a.lastSec > b.lastSec; });
        if (tel.cells.size() > kGsmrMax) tel.cells.resize(kGsmrMax);
        refreshState(sec);
        publish();
    }

    void refreshState(double sec) {
        const GsmrCell* best = nullptr;
        bool recent = false;
        for (const auto& c : tel.cells) {
            if (sec - c.lastSec < 30) recent = true;
            if (sec - c.lastSec < 30 && (!best || c.levelDb > best->levelDb)) best = &c;
        }
        tel.dataValid = recent;
        tel.state = (tel.timeSec > 0 || tel.blocks) ? (recent ? 2 : 1) : 0;
        if (best) { tel.cfoHz = best->cfoHz; tel.snrDb = best->snrDb; }
    }

    void merge(const Carrier& cr, const Decoded& d, double center, double sec) {
        const double actual = (center > 0 ? center : 0.0) + d.cfoHz;
        const int arfcn = center > 0 ? gsmrHzToArfcn(actual) : -1;
        const double nominal = arfcn >= 0 ? gsmrRasterHz(actual) : actual;
        GsmrCell* t = nullptr;
        for (auto& c : tel.cells) if (c.arfcn == arfcn && c.bsic == d.bsic && (arfcn >= 0 || std::fabs(c.freqHz - nominal) < 60e3)) { t = &c; break; }
        const bool isNew = !t;
        if (!t) {
            tel.cells.push_back(GsmrCell());
            t = &tel.cells.back();
            t->arfcn = arfcn; t->freqHz = nominal; t->bsic = d.bsic; t->ncc = d.bsic >> 3; t->bcc = d.bsic & 7;
            t->railway = gsmrIsRailway(arfcn); t->firstWall = wallNow();
        }
        t->fn = (int)d.fn;
        t->levelDb = cr.powerDb; t->snrDb = d.snrDb;
        t->cfoHz = center > 0 ? actual - nominal : d.cfoHz;
        t->lastWall = wallNow(); t->lastSec = sec; t->seen++;
        char b[256];
        if (isNew) {
            if (arfcn >= 0) snprintf(b, sizeof b, "ARFCN %d at %.1f MHz%s, BSIC %d-%d", arfcn, nominal / 1e6, t->railway ? " (GSM-R band)" : "", t->ncc, t->bcc);
            else snprintf(b, sizeof b, "carrier at %+.1f kHz, BSIC %d-%d", nominal / 1e3, t->ncc, t->bcc);
            event(b, sec);
        }
        t->siBad += (uint64_t)d.bad;
        t->ccch.merge(d.ccch);
        tel.blocksBad += (uint64_t)d.bad;
        for (const auto& m : d.msgs) {
            tel.blocksOk++;
            t->siMsgs++;
            if (std::find(t->typesSeen.begin(), t->typesSeen.end(), m.type) == t->typesSeen.end()) { t->typesSeen.push_back(m.type); std::sort(t->typesSeen.begin(), t->typesSeen.end()); }
            if (m.hasLai) {
                const bool first = !t->hasLai || t->lac != m.si3.lai.lac || t->mnc != m.si3.lai.mnc;
                t->hasLai = true;
                t->mcc = m.si3.lai.mcc; t->mnc = m.si3.lai.mnc; t->mncDigits = m.si3.lai.mncDigits; t->lac = m.si3.lai.lac;
                t->op = gsmrOperatorName(t->mcc, t->mnc);
                if (first && !m.hasSi3) {
                    snprintf(b, sizeof b, "ARFCN %d: %03d-%0*d %s, LAC %u", t->arfcn, t->mcc, t->mncDigits, t->mnc, t->op.c_str(), t->lac);
                    event(b, sec);
                }
            }
            const gsmr::Si3& s = m.si3;
            if (m.hasSi3) {
                const bool first = !t->hasSi3 || t->cellId != s.cellId;
                t->hasSi3 = true;
                t->cellId = s.cellId; t->attach = s.attach; t->agBlks = s.agBlks; t->ccchConf = s.ccchConf; t->pagMfrms = s.pagMfrms; t->t3212 = s.t3212;
                t->rlTimeout = s.rlTimeout; t->dtx = s.dtx; t->pwrc = s.pwrc;
                if (first) {
                    snprintf(b, sizeof b, "ARFCN %d: %03d-%0*d %s, LAC %u, cell %u", t->arfcn, t->mcc, t->mncDigits, t->mnc, t->op.c_str(), t->lac, t->cellId);
                    event(b, sec);
                }
            }
            if (m.type == gsmr::kSi3 || m.type == gsmr::kSi4) {
                t->reselHystDb = s.reselHystDb; t->msTxPwrMax = s.msTxPwrMax; t->rxLevAccessMin = s.rxLevAccessMin; t->neci = s.neci; t->acs = s.acs;
            }
            if (m.type == gsmr::kSi1 || m.type == gsmr::kSi2 || m.type == gsmr::kSi3 || m.type == gsmr::kSi4) {
                t->maxRetrans = s.maxRetrans; t->txInteger = s.txInteger; t->cellBar = s.cellBar; t->acBarred = s.acBarred;
            }
            if (m.type == gsmr::kSi1 && !m.listUnparsed) { t->hasSi1 = true; t->cellAlloc = m.arfcns; }
            if (m.type == gsmr::kSi2 && !m.listUnparsed) { t->hasSi2 = true; t->neigh = m.arfcns; t->nccPermitted = m.nccPermitted; }
            if (m.type == gsmr::kSi2bis && !m.listUnparsed && m.hasList) t->neighBis = m.arfcns;
        }
    }
};

GsmrReceiver::GsmrReceiver() : p_(new Impl) {}
GsmrReceiver::~GsmrReceiver() = default;

void GsmrReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = rate;
    p_->tel.inputRate = rate;
    p_->want = (size_t)(rate * kBlockSec);
    p_->gen++;
}

void GsmrReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->sigOff = hz; p_->tel.dialOffsetHz = hz; p_->gen++; }
void GsmrReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (std::fabs(hz - p_->centerHz) > 1) { p_->gen++; p_->tel.retunes++; }
    p_->centerHz = hz; p_->tel.centerHz = hz;
}
bool GsmrReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= gsmrTuning().minSampleRate - 1; }
void GsmrReceiver::reset() { p_->gen++; }
void GsmrReceiver::clearCells() { std::lock_guard<std::mutex> lk(p_->mu); p_->tel.cells.clear(); p_->tel.events.clear(); p_->tel.blocksOk = p_->tel.blocksBad = 0; p_->publish(); }
void GsmrReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool GsmrReceiver::busy() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->idle.load() || p_->jobReady; }

// the cells and the event log already stay through reset() and configure() (the sample clock runs on): only the user's Clear empties them
void GsmrReceiver::clearResults() { clearCells(); }
std::string GsmrReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool GsmrReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    GsmrTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;    // onto this run's clock
    for (auto& c : l.cells) c.lastSec += dt;
    for (auto& e : l.events) e.sec += dt;
    p_->tel.cells = std::move(l.cells); p_->tel.events = std::move(l.events);
    p_->publish();
    return true;
}
bool GsmrReceiver::telemetry(GsmrTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void GsmrReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    double rate, center, off;
    { std::lock_guard<std::mutex> lk(m.mu); rate = m.rateSet; center = m.centerHz; off = m.sigOff; }
    if (rate < gsmrTuning().minSampleRate - 1 || m.want == 0) return;
    for (size_t i = 0; i < n; i++) { const cf32 v = x[i]; if (std::isfinite(v.real()) && std::isfinite(v.imag())) m.levelAcc += std::norm(v); }
    m.levelN += n;
    m.total += n;
    const uint64_t g = m.gen.load();
    if (g != m.accGen) { m.acc.clear(); m.accGen = g; }
    if (m.idle.load()) {
        if (m.acc.empty()) { m.accCenter = center; m.accOff = off; m.acc.reserve(m.want); }
        const size_t take = std::min(n, m.want - m.acc.size());
        for (size_t i = 0; i < take; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            m.acc.push_back(v);
        }
        if (m.acc.size() >= m.want) {
            std::lock_guard<std::mutex> lk(m.mu);
            m.job.swap(m.acc); m.acc.clear();
            m.jobCenter = m.accCenter; m.jobOff = m.accOff; m.jobRate = rate; m.jobSec = (double)m.total / rate; m.jobGen = g;
            m.jobReady = true; m.idle = false;
            m.cv.notify_one();
        }
    }
    const double now = (double)m.total / rate;
    if (now - m.lastPubSec >= 0.5) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.tel.timeSec = now;
        m.tel.levelDb = m.levelN ? (float)(10 * std::log10(m.levelAcc / (double)m.levelN + 1e-20)) : -200.f;
        m.levelAcc = 0; m.levelN = 0;
        m.lastPubSec = now;
        m.refreshState(now);
        if (m.tel.state == 0) m.tel.state = 1;
        m.publish();
    }
}

} // namespace dect2
