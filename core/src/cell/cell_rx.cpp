// Cell towers receiver (cell_rx.h): blocks of input, carriers found in the spectrum, cells searched and their broadcast decoded.
#include "dect2/common/thread_name.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/cell/cell_asn.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/cell/cell_lte.h"
#include "dect2/cell/cell_tel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include "dect2/common/timecompat.h"

namespace dect2 {

// ---------------------------------------------------------------- bands and operators

const std::vector<LteBand>& lteBands() {
    // 36.101 Table 5.7.3-1 (downlink EARFCN), every FDD / TDD / SDL band; the order is the preference when a frequency lies in several
    // bands (Europe first)
    static const std::vector<LteBand> t = {
        {1, 2110, 2170, 0, false, "IMT 2100"},        {3, 1805, 1880, 1200, false, "DCS 1800"},   {7, 2620, 2690, 2750, false, "IMT-E 2600"},
        {8, 925, 960, 3450, false, "E-GSM 900"},      {20, 791, 821, 6150, false, "EU 800 DD"},   {28, 758, 803, 9210, false, "APT 700"},
        {32, 1452, 1496, 9920, false, "L-band SDL"},  {38, 2570, 2620, 37750, true, "TDD 2600"},  {40, 2300, 2400, 38650, true, "TDD 2300"},
        {41, 2496, 2690, 39650, true, "TDD 2500"},    {42, 3400, 3600, 41590, true, "TDD 3500"},  {43, 3600, 3800, 43590, true, "TDD 3700"},
        {2, 1930, 1990, 600, false, "PCS 1900"},
        {4, 2110, 2155, 1950, false, "AWS-1"},        {5, 869, 894, 2400, false, "CLR 850"},      {12, 729, 746, 5010, false, "Lower 700"},
        {13, 746, 756, 5180, false, "Upper 700 C"},   {17, 734, 746, 5730, false, "Lower 700 B/C"}, {25, 1930, 1995, 8040, false, "E-PCS 1900"},
        {26, 859, 894, 8690, false, "E-CLR 850"},     {66, 2110, 2200, 66436, false, "AWS-3"},     {71, 617, 652, 68586, false, "600 MHz"},
        {6, 875, 885, 2650, false, "UMTS 800"},       {9, 1844.9, 1879.9, 3800, false, "UMTS 1700"}, {10, 2110, 2170, 4150, false, "E-AWS"},
        {11, 1475.9, 1495.9, 4750, false, "Lower 1500"}, {14, 758, 768, 5280, false, "Upper 700 D"}, {18, 860, 875, 5850, false, "Lower 800"},
        {19, 875, 890, 6000, false, "Upper 800"},     {21, 1495.9, 1510.9, 6450, false, "Upper 1500"}, {22, 3510, 3590, 6600, false, "3500 FDD"},
        {23, 2180, 2200, 7500, false, "S-band"},      {24, 1525, 1559, 7700, false, "L-band 1600"}, {27, 852, 869, 9040, false, "SMR 800"},
        {29, 717, 728, 9660, false, "700 SDL"},       {30, 2350, 2360, 9770, false, "WCS 2300"},   {31, 462.5, 467.5, 9870, false, "450 MHz"},
        {33, 1900, 1920, 36000, true, "TDD 2000"},    {34, 2010, 2025, 36200, true, "TDD 2000"},   {35, 1850, 1910, 36350, true, "TDD PCS"},
        {36, 1930, 1990, 36950, true, "TDD PCS"},     {37, 1910, 1930, 37550, true, "TDD PCS"},    {39, 1880, 1920, 38250, true, "TDD 1900"},
        {44, 703, 803, 45590, true, "TDD 700"},       {45, 1447, 1467, 46590, true, "TDD 1500"},   {46, 5150, 5925, 46790, true, "LAA 5 GHz"},
        {47, 5855, 5925, 54540, true, "V2X 5.9 GHz"}, {48, 3550, 3700, 55240, true, "CBRS"},       {49, 3550, 3700, 56740, true, "CBRS LAA"},
        {50, 1432, 1517, 58240, true, "TDD 1500"},    {51, 1427, 1432, 59090, true, "TDD 1500-"},  {52, 3300, 3400, 59140, true, "TDD 3300"},
        {53, 2483.5, 2495, 60140, true, "TDD 2.4 GHz"},
        {65, 2110, 2200, 65536, false, "E-IMT 2100"}, {67, 738, 758, 67336, false, "EU 700 SDL"}, {68, 753, 783, 67536, false, "ME 700"},
        {69, 2570, 2620, 67836, false, "2600 SDL"},   {70, 1995, 2020, 68336, false, "AWS-4"},     {72, 461, 466, 68936, false, "PMR 450"},
        {73, 460, 465, 68986, false, "450 APAC"},     {74, 1475, 1518, 69036, false, "L-band"},    {75, 1432, 1517, 69466, false, "L-band SDL"},
        {76, 1427, 1432, 70316, false, "L-band SDL-"}, {85, 728, 746, 70366, false, "Lower 700 A"}, {87, 420, 425, 70546, false, "410 MHz"},
        {88, 422, 427, 70596, false, "410 MHz"},
    };
    return t;
}

const LteBand* lteBandInfo(int band) {
    for (const auto& b : lteBands()) if (b.band == band) return &b;
    return nullptr;
}

static int bandSpan(const LteBand& b) { return (int)std::lround((b.dlHighMhz - b.dlLowMhz) * 10); }

double lteEarfcnToHz(int n) {
    for (const auto& b : lteBands()) if (n >= b.nOffsDl && n < b.nOffsDl + bandSpan(b)) return (b.dlLowMhz + 0.1 * (n - b.nOffsDl)) * 1e6;
    return 0;
}

int lteBandOfEarfcn(int n) {
    for (const auto& b : lteBands()) if (n >= b.nOffsDl && n < b.nOffsDl + bandSpan(b)) return b.band;
    return 0;
}

int lteHzToEarfcn(double hz, int prefer) {
    const double mhz = hz / 1e6;
    auto of = [&](const LteBand& b) -> int {
        const long k = std::lround((mhz - b.dlLowMhz) * 10);
        if (k < 0 || k >= bandSpan(b) || std::fabs(mhz - (b.dlLowMhz + 0.1 * (double)k)) > 0.02) return -1;
        return b.nOffsDl + (int)k;
    };
    if (const LteBand* p = lteBandInfo(prefer)) { const int e = of(*p); if (e >= 0) return e; }
    for (const auto& b : lteBands()) { const int e = of(b); if (e >= 0) return e; }
    return -1;
}

int lteNearestEarfcn(double hz, int prefer) {
    const double mhz = hz / 1e6;
    auto of = [&](const LteBand& b) -> int {
        const long k = std::lround((mhz - b.dlLowMhz) * 10);
        if (k < 0 || k >= bandSpan(b)) return -1;
        return b.nOffsDl + (int)k;
    };
    if (const LteBand* p = lteBandInfo(prefer)) { const int e = of(*p); if (e >= 0) return e; }
    for (const auto& b : lteBands()) { const int e = of(b); if (e >= 0) return e; }
    return -1;
}

std::vector<int> lteBandsOfHz(double hz) {
    std::vector<int> v;
    const double mhz = hz / 1e6;
    for (const auto& b : lteBands()) {
        const long k = std::lround((mhz - b.dlLowMhz) * 10);
        if (k >= 0 && k < bandSpan(b)) v.push_back(b.band);
    }
    return v;
}

LteBandChoice lteBandFor(double hz, int sib1Band) {
    LteBandChoice c;
    const std::vector<int> all = lteBandsOfHz(hz);
    if (sib1Band >= 1 && sib1Band != 64) {   // 65..256: freqBandIndicator-v9e0, read from SIB1's extension
        c.band = sib1Band; c.confirmed = true;
        for (int b : all) if (b != sib1Band) c.also.push_back(b);
        return c;
    }
    std::vector<int> pool;
    for (int b : all) if (sib1Band != 64 || b > 64) pool.push_back(b);
    if (pool.empty()) return c;
    c.band = pool[0];
    c.confirmed = sib1Band == 64 && pool.size() == 1;   // the only band above 64 that holds it
    for (size_t i = 1; i < pool.size(); i++) c.also.push_back(pool[i]);
    return c;
}

const char* lteOperatorName(int mcc, int mnc) {
    struct O { int mcc, mnc; const char* name; };
    static const O t[] = {
        {1, 1, "Test network"},
        {246, 1, "Telia"}, {246, 2, "Bite"}, {246, 3, "Tele2"},
        {247, 1, "LMT"}, {247, 2, "Tele2"}, {247, 5, "Bite"},
        {248, 1, "Telia"}, {248, 2, "Elisa"}, {248, 3, "Tele2"},
        {260, 1, "Plus"}, {260, 2, "T-Mobile"}, {260, 3, "Orange"}, {260, 6, "Play"},
        {244, 5, "Elisa"}, {244, 12, "DNA"}, {244, 91, "Telia"},
        {240, 1, "Telia"}, {240, 2, "Tre"}, {240, 7, "Tele2"},
        {242, 1, "Telenor"}, {242, 2, "Telia"},
        {238, 1, "TDC"}, {238, 2, "Telenor"},
        {262, 1, "Telekom"}, {262, 2, "Vodafone"}, {262, 3, "O2"},
        {234, 10, "O2"}, {234, 15, "Vodafone"}, {234, 20, "Three"}, {234, 30, "EE"},
        {208, 1, "Orange"}, {208, 10, "SFR"}, {208, 15, "Free"}, {208, 20, "Bouygues"},
        {424, 2, "e& (Etisalat)"}, {424, 3, "du"},
        {250, 1, "MTS"}, {250, 2, "MegaFon"}, {250, 20, "Tele2"}, {250, 99, "Beeline"},
        {257, 1, "A1"}, {257, 2, "MTS"}, {257, 4, "life:)"},
        {255, 1, "Vodafone"}, {255, 3, "Kyivstar"}, {255, 6, "lifecell"},
        {310, 260, "T-Mobile"}, {310, 410, "AT&T"}, {311, 480, "Verizon"}, {313, 100, "FirstNet"},
    };
    for (const auto& o : t) if (o.mcc == mcc && o.mnc == mnc) return o.name;
    return "";
}

const char* lteCountry(int mcc) {
    struct C { int mcc; const char* name; };
    static const C t[] = {{1, "test"}, {246, "Lithuania"}, {247, "Latvia"}, {248, "Estonia"}, {260, "Poland"}, {244, "Finland"}, {240, "Sweden"},
                          {242, "Norway"}, {238, "Denmark"}, {262, "Germany"}, {234, "United Kingdom"}, {208, "France"}, {424, "UAE"},
                          {250, "Russia"}, {257, "Belarus"}, {255, "Ukraine"}, {310, "USA"}, {311, "USA"}};
    for (const auto& c : t) if (c.mcc == mcc) return c.name;
    return "";
}

// ---------------------------------------------------------------- receiver

ModeTuning cellTuning() {
    ModeTuning t;
    t.stdMode = 34; t.id = "cell"; t.name = "Cell towers";
    t.minMhz = 450; t.maxMhz = 3800; t.defMhz = 806;
    t.sampleRate = 20000000;     // a whole 10 MHz carrier (and a 15 MHz one) with room for the filter; the HackRF Pro tunes exactly at 20 Msps
    t.basebandHz = 15000000;
    t.bandwidthMhz = 10;
    t.minSampleRate = 1920000;   // the central 1.08 MHz: cell search and MIB; the system information needs the whole carrier
    return t;
}

namespace {

constexpr double kBlockSec = 0.2;

int64_t wallNow() { return dect2::unixNow(); }

struct Cand { double off; double maxCfo; bool snapped; double raw; };

} // namespace

struct CellReceiver::Impl {
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
    double lastCenterSeen = -1;
    // worker
    std::thread th;
    std::condition_variable cv;
    bool stop = false, jobReady = false;
    std::vector<cf32> job;
    double jobCenter = 0, jobOff = 0, jobRate = 0, jobSec = 0;
    uint64_t jobGen = 0;
    // results (under mu)
    CellTelemetry tel, pub;

    Impl() { th = dect2::namedThread("onair-cell", [this] { run(); }); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    void event(const std::string& s, double sec) {
        CellEvent e; e.wallTime = wallNow(); e.sec = sec; e.text = s;
        tel.events.insert(tel.events.begin(), e);
        if (tel.events.size() > kCellLogMax) tel.events.resize(kCellLogMax);
        if (log) log("cell: " + s);
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

    // LTE-shaped blocks in the spectrum: offsets of their centres from 0 Hz
    std::vector<CellCarrierSeen> carriers(const std::vector<cf32>& x, double fs) {
        std::vector<CellCarrierSeen> out;
        const int nfft = cell::pow2At((int)(fs / 25e3));
        if (nfft < 64 || x.size() < (size_t)nfft * 8) return out;
        std::vector<float> p = cell::welch(x.data(), x.size(), nfft, 240);
        std::vector<float> d((size_t)nfft);
        for (int i = 0; i < nfft; i++) {
            double s = 0;
            for (int k = -2; k <= 2; k++) s += p[(size_t)std::max(0, std::min(nfft - 1, i + k))];
            d[(size_t)i] = (float)(10 * std::log10(s / 5 + 1e-30));
        }
        std::vector<float> srt = d;
        std::nth_element(srt.begin(), srt.begin() + nfft / 6, srt.end());
        const float floorDb = srt[(size_t)(nfft / 6)], thr = floorDb + 6;
        const double bin = fs / nfft;
        int i = 0;
        const int edge = (int)(nfft * 0.03);
        while (i < nfft) {
            if (d[(size_t)i] < thr) { i++; continue; }
            int j = i;
            int gap = 0;
            while (j + 1 < nfft) {
                if (d[(size_t)(j + 1)] >= thr) { gap = 0; j++; }
                else if (gap < 2 && j + 2 < nfft && d[(size_t)(j + 2)] >= thr) { gap++; j++; }
                else break;
            }
            const double w = (j - i + 1) * bin;
            if (w >= 0.9e6 && w <= 19.5e6 && i > edge && j < nfft - 1 - edge) {
                CellCarrierSeen c;
                c.freqHz = ((i + j) / 2.0 - nfft / 2.0) * bin;   // welch is fftshifted: bin 0 = -fs/2
                c.widthHz = w;
                float pk = -300; for (int k = i; k <= j; k++) pk = std::max(pk, d[(size_t)k]);
                c.levelDb = pk - floorDb;
                out.push_back(c);
            }
            i = j + 1;
        }
        return out;
    }

    void process(const std::vector<cf32>& x, double center, double dialOff, double fs, double sec, uint64_t g) {
        const std::vector<CellCarrierSeen> car = carriers(x, fs);
        std::vector<Cand> cands;
        auto addCand = [&](double off, bool snapIt) {
            if (center > 0 && snapIt) {
                const double f = center + off, snapped = std::round(f / 100e3) * 100e3;
                Cand c{snapped - center, 25000, true, off};
                for (const auto& o : cands) if (std::fabs(o.off - c.off) < 50e3) return;
                cands.push_back(c);
            } else {
                Cand c{off, 60000, false, off};
                for (const auto& o : cands) if (std::fabs(o.off - c.off) < 50e3) return;
                cands.push_back(c);
            }
        };
        addCand(dialOff, true);
        // carriers side by side (the operators' blocks fill a band edge to edge) make one wide block, or none, in the spectrum: look for a
        // PSS on every 100 kHz raster point inside the sample band, and keep the strongest point of each 1.4 MHz
        if (center > 0) {
            const double lim = fs / 2 * 0.92 - 0.7e6;
            std::vector<double> offs;
            for (long k = (long)std::ceil((center - lim) / 100e3); k * 100e3 <= center + lim; k++) offs.push_back(k * 100e3 - center);
            std::vector<cell::LtePssProbe> pr = cell::ltePssProbe(x.data(), x.size(), fs, offs, 25000, 10.0f);
            std::sort(pr.begin(), pr.end(), [](const cell::LtePssProbe& a, const cell::LtePssProbe& b) { return a.db > b.db; });
            std::vector<double> kept;
            for (const auto& p : pr) {
                bool nearBy = false;
                for (double k : kept) if (std::fabs(k - p.offsetHz) < 1.4e6) nearBy = true;
                if (nearBy) continue;
                kept.push_back(p.offsetHz);
                addCand(p.offsetHz, true);
            }
            // the spectrum's blocks too, where no PSS point stood out near them (the probe looks at 20 ms only)
            for (const auto& c : car) {
                bool nearBy = false;
                for (double k : kept) if (std::fabs(k - c.freqHz) < 1.4e6) nearBy = true;
                if (!nearBy) addCand(c.freqHz, true);
            }
        } else for (const auto& c : car) addCand(c.freqHz, true);
        std::vector<std::pair<Cand, std::vector<cell::LteCellResult>>> found;
        for (size_t k = 0; k < cands.size(); k++) {
            if (gen.load() != g) return;   // retuned meanwhile: these samples belong to another frequency
            cell::LteSearchOpts o;
            o.maxCfoHz = cands[k].maxCfo;
            auto r = cell::lteSearchDecode(x.data(), x.size(), fs, cands[k].off, o);
            if (r.empty() && cands[k].snapped && std::fabs(cands[k].raw - cands[k].off) > 20e3) {   // the spectrum's centre was off the raster: search around it
                o.maxCfoHz = 60000;
                r = cell::lteSearchDecode(x.data(), x.size(), fs, cands[k].raw, o);
                if (!r.empty()) cands[k].off = cands[k].raw;
            }
            if (!r.empty()) found.push_back({cands[k], std::move(r)});
        }
        std::lock_guard<std::mutex> lk(mu);
        if (gen.load() != g) return;
        if (clearReq.exchange(false)) { tel.cells.clear(); tel.events.clear(); tel.blocksOk = tel.blocksBad = 0; }
        tel.blocks++;
        tel.carriers = car;
        for (auto& cc : tel.carriers) cc.freqHz += center;
        tel.lastBlockCenterHz = center;
        for (auto& fr : found) for (auto& r : fr.second) merge(fr.first, r, center, sec);
        std::stable_sort(tel.cells.begin(), tel.cells.end(), [](const CellTower& a, const CellTower& b) { return a.lastSec > b.lastSec; });
        if (tel.cells.size() > kCellMax) tel.cells.resize(kCellMax);
        refreshState(sec);
        publish();
    }

    void refreshState(double sec) {
        const CellTower* best = nullptr;
        bool recent = false;
        for (const auto& c : tel.cells) {
            if (sec - c.lastSec < 10) recent = true;
            if (sec - c.lastSec < 10 && (!best || c.rsrpDb > best->rsrpDb)) best = &c;
        }
        tel.dataValid = recent;
        tel.state = (tel.timeSec > 0 || tel.blocks) ? (recent ? 2 : 1) : 0;
        if (best) { tel.cfoHz = best->cfoHz; tel.snrDb = best->sinrDb; }
    }

    void merge(const Cand& cd, const cell::LteCellResult& r, double center, double sec) {
        const double actual = (center > 0 ? center : 0.0) + cd.off + r.found.cfoHz;
        // the nearest channel of the band table (bands whose edge is on a 50 kHz point have a raster of their own), else the 100 kHz grid
        const int ear = center > 0 ? lteNearestEarfcn(actual) : -1;
        const double nominal = center > 0 ? (ear >= 0 ? lteEarfcnToHz(ear) : std::round(actual / 100e3) * 100e3) : cd.off;
        if (center > 0 && ear < 0 && !r.mib.ok) return;   // off every LTE band raster and without a MIB: not a cell
        CellTower* t = nullptr;
        for (auto& c : tel.cells) if (c.pci == r.found.pci && std::fabs(c.freqHz - nominal) < 60e3) { t = &c; break; }
        const bool isNew = !t;
        if (!t) {
            tel.cells.push_back(CellTower());
            t = &tel.cells.back();
            t->freqHz = nominal; t->earfcn = ear; t->pci = r.found.pci; t->firstWall = wallNow();
            if (ear >= 0) { const LteBandChoice bc = lteBandFor(nominal, 0); t->band = bc.band; t->bandConfirmed = false; t->bandsAlso = bc.also; }
        }
        t->tdd = r.found.tdd; t->extCp = r.found.extCp;
        if (r.mib.ok) { t->hasMib = true; t->nRb = r.mib.nRb; t->ports = r.mib.ports; t->phichDur = r.mib.phichDur; t->phichRes = r.mib.phichRes; t->sfn = r.mib.sfn; }
        t->rsrpDb = r.meas.rsrpDb; t->rsrqDb = r.meas.rsrqDb; t->sinrDb = r.meas.sinrDb; t->pssDb = r.found.pssDb;
        t->cfoHz = actual - (center > 0 ? nominal : cd.off);
        t->clockPpm = r.found.clockPpm;
        t->lastWall = wallNow(); t->lastSec = sec; t->seen++;
        char b[256];
        if (isNew) {
            if (!r.mib.ok) {
                if (center > 0) snprintf(b, sizeof b, "PCI %d at %.1f MHz (EARFCN %d), synchronisation signals only", t->pci, nominal / 1e6, ear);
                else snprintf(b, sizeof b, "PCI %d at %+.1f kHz, synchronisation signals only", t->pci, nominal / 1e3);
            } else if (center > 0) snprintf(b, sizeof b, "PCI %d at %.1f MHz (EARFCN %d), %.0f MHz, %d port%s", t->pci, nominal / 1e6, ear, t->nRb * 0.2, t->ports, t->ports == 1 ? "" : "s");
            else snprintf(b, sizeof b, "PCI %d at %+.1f kHz, %.0f MHz, %d port%s", t->pci, nominal / 1e3, t->nRb * 0.2, t->ports, t->ports == 1 ? "" : "s");
            event(b, sec);
        }
        tel.blocksBad += (uint64_t)r.tbFail;
        for (const auto& m : r.si) {
            cell::LteBcch bc;
            if (!cell::lteParseBcch(m.bits, bc)) continue;
            tel.blocksOk++;
            t->siMsgs++;
            auto seenSib = [&](int n) { if (std::find(t->sibsSeen.begin(), t->sibsSeen.end(), n) == t->sibsSeen.end()) { t->sibsSeen.push_back(n); std::sort(t->sibsSeen.begin(), t->sibsSeen.end()); } };
            if (bc.isSib1) {
                const cell::LteSib1& s = bc.sib1;
                const bool first = !t->hasSib1 || t->cellId != s.cellId || t->tac != s.tac;
                t->hasSib1 = true;
                seenSib(1);
                t->plmns.clear();
                for (const auto& p : s.plmns) t->plmns.push_back(cell::plmnText(p));
                if (!s.plmns.empty()) { t->mcc = s.plmns[0].mcc; t->mnc = s.plmns[0].mnc; t->mncDigits = s.plmns[0].mncDigits; t->reservedOp = s.plmns[0].reserved; }
                t->op = lteOperatorName(t->mcc, t->mnc);
                t->tac = s.tac; t->cellId = s.cellId; t->enb = s.cellId >> 8; t->sector = (int)(s.cellId & 255);
                t->barred = s.barred; t->qRxLevMinDbm = s.qRxLevMinDbm; t->pMaxDbm = s.pMax;
                if (s.band > 0) {
                    const LteBandChoice bc = lteBandFor(t->freqHz, s.band);   // 64: a band above 64 (66, 71...), from the frequency
                    if (bc.band > 0) { t->band = bc.band; t->bandConfirmed = bc.confirmed; t->bandsAlso = bc.also; t->mfbi = false; }
                    // MFBI: the cell names the other bands it is in; those replace the guess from the frequency
                    std::vector<int> mb;
                    for (int x : s.multiBands) if (x != t->band && x != 64 && std::find(mb.begin(), mb.end(), x) == mb.end()) mb.push_back(x);
                    if (!mb.empty()) { t->bandsAlso = mb; t->mfbi = true; }
                    if (center > 0 && t->band > 0) { const int e = lteNearestEarfcn(t->freqHz, t->band); if (e >= 0) t->earfcn = e; }
                }
                t->sched.clear();
                for (const auto& e : s.sched) { CellSched cs; cs.periodRf = e.periodRf; cs.sibs = e.sibs; t->sched.push_back(cs); }
                t->siWindowMs = s.siWindowMs; t->valueTag = s.valueTag;
                if (first) {
                    snprintf(b, sizeof b, "PCI %d: %s%s%s, TAC %u, eNB %u sector %d, band %d", t->pci, t->plmns.empty() ? "?" : t->plmns[0].c_str(),
                             t->op.empty() ? "" : " ", t->op.c_str(), t->tac, t->enb, t->sector, t->band);
                    event(b, sec);
                }
                continue;
            }
            for (int n : bc.sibsSeen) seenSib(n);
            if (bc.has2) {
                const cell::LteSib2& s = bc.sib2;
                t->hasSib2 = true;
                t->refSignalPowerDbm = s.refSignalPowerDbm; t->pb = s.pb; t->ulEarfcn = s.ulEarfcn; t->ulBwRb = s.ulBwRb; t->raPreambles = s.numRaPreambles;
                t->modPeriodCoeff = s.modPeriodCoeff; t->pagingCycleRf = s.pagingCycleRf; t->acBarring = s.acBarring;
                t->t300 = s.t300; t->t310 = s.t310; t->n310 = s.n310; t->timeAlignMs = s.timeAlignMs;
            }
            if (bc.has3) {
                const cell::LteSib3& s = bc.sib3;
                t->hasSib3 = true;
                t->qHystDb = s.qHystDb; t->prio = s.priority; t->sIntraDb = s.sIntraSearch; t->sNonIntraDb = s.sNonIntraSearch;
                t->threshServingLowDb = s.threshServingLow; t->tReselSec = s.tReselSec;
            }
            if (bc.has4) {
                t->hasSib4 = true;
                t->intra.clear();
                for (const auto& nb : bc.sib4.neigh) t->intra.push_back({nb.pci, nb.qOffsetDb});
            }
            if (bc.has5) {
                t->hasSib5 = true;
                t->inter.clear();
                for (const auto& f : bc.sib5.freqs) {
                    CellInterFreq q;
                    q.earfcn = f.earfcn; q.bwRb = f.bwRb; q.priority = f.priority; q.qRxLevMinDbm = f.qRxLevMinDbm; q.threshHighDb = f.threshHigh; q.threshLowDb = f.threshLow;
                    for (const auto& nb : f.neigh) q.cells.push_back({nb.pci, nb.qOffsetDb});
                    t->inter.push_back(q);
                }
            }
        }
    }
};

CellReceiver::CellReceiver() : p_(new Impl) {}
CellReceiver::~CellReceiver() = default;

void CellReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rateSet = rate;
    p_->tel.inputRate = rate;
    p_->want = (size_t)(rate * kBlockSec);
    p_->gen++;
}

void CellReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->sigOff = hz; p_->tel.dialOffsetHz = hz; p_->gen++; }
void CellReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (std::fabs(hz - p_->centerHz) > 1) { p_->gen++; p_->tel.retunes++; }
    p_->centerHz = hz; p_->tel.centerHz = hz;
}
bool CellReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= cellTuning().minSampleRate - 1; }
void CellReceiver::reset() { p_->gen++; }
void CellReceiver::clearCells() { p_->clearReq = true; std::lock_guard<std::mutex> lk(p_->mu); p_->tel.cells.clear(); p_->tel.events.clear(); p_->tel.blocksOk = p_->tel.blocksBad = 0; p_->clearReq = false; p_->publish(); }
void CellReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

bool CellReceiver::busy() const { std::lock_guard<std::mutex> lk(p_->mu); return !p_->idle.load() || p_->jobReady; }

// the cells and the event log already stay through reset() and configure() (the sample clock runs on): only the user's Clear empties them
void CellReceiver::clearResults() { clearCells(); }
std::string CellReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.timeSec, p_->tel.cells, p_->tel.events);
}
bool CellReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    CellTelemetry l;
    if (!results::unpack(blob, t0, l.cells, l.events)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->tel.timeSec - t0;    // onto this run's clock
    for (auto& c : l.cells) c.lastSec += dt;
    for (auto& e : l.events) e.sec += dt;
    p_->tel.cells = std::move(l.cells); p_->tel.events = std::move(l.events);
    p_->publish();
    return true;
}
bool CellReceiver::telemetry(CellTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void CellReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    double rate, center, off;
    { std::lock_guard<std::mutex> lk(m.mu); rate = m.rateSet; center = m.centerHz; off = m.sigOff; }
    if (rate < cellTuning().minSampleRate - 1 || m.want == 0) return;
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
