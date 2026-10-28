// DVB-S/S2 receiver, see dvbs_rx.h.
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_s2.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/dvbs/dvbs_cid.h"
#include "dvbs_carrier.h"
#include "dvbs_front.h"
#include "dvbs_s1.h"
#include "dvbs_compute.h"
#include "dvbs_s2hunt.h"
#include "dvbs_s2rx.h"
#include "dvbs_sf.h"
#include "dect2/tv/ts.h"
#include "dvbs_spec.h"
#include "dvbs_symthread.h"
#include "dvbs_vlsnr.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace dect2 {
using namespace dvbs;

double dvbsMaxSymbolRate(double fs, double rollOff) { return fs / (1.3 * (1.0 + rollOff)); }
double dvbsMinAutoSymbolRate(double fs) { return std::max(60e3, fs * 40.0 / 4096.0 * 2.5); }   // about 40 spectrum bins across the carrier

ModeTuning dvbsTuning() {
    ModeTuning t;
    t.stdMode = 8; t.id = "dvbs"; t.name = "DVB-S/S2";
    // The L-band IF of an LNB is 950 to 2150 MHz (739 MHz for the QO-100 narrow band transponder), but radio amateurs also send DVB-S/S2
    // directly on 50, 70, 144, 437 MHz and 1.2 / 2.4 GHz, often narrow band (issue #30): the radio's own range is the only limit.
    t.minMhz = 10; t.maxMhz = 6000; t.defMhz = 1500.0;
    t.sampleRate = 10000000.0; t.basebandHz = 8000000.0; t.bandwidthMhz = 8;
    t.minSampleRate = 2000000.0;
    return t;
}

namespace {
using clk = std::chrono::steady_clock;
constexpr size_t kHuntWindow = 49152;       // (DVB-S)        // symbols a DVB-S attempt looks at: 16384 for the carrier loop to settle, 32768 to find the code
constexpr size_t kHuntKeep = 294912;        // symbols kept for the frame search of DVB-S2: up to eight long frames, the more the weaker a signal it finds
constexpr size_t kS2Window = 262144;
constexpr size_t kCellsMax = 2048;

double snapRollOff(double a) {
    static const double v[6] = {0.35, 0.25, 0.20, 0.15, 0.10, 0.05};
    double best = v[0];
    for (double x : v) if (std::fabs(x - a) < std::fabs(best - a)) best = x;
    return best;
}
// The symbol rate from the spectral line that the squared envelope of a linearly modulated carrier has at the symbol rate (its cyclostationarity).
// The spectrum's -3 dB width (the first estimate, rs0) is bent by an echo or a tilted cable response, which ripples the spectrum: the line is not.
// x: kCyclicN samples at fs; centre: where the carrier is. Looks between 0.6 and 1.25 times rs0 (clipping and
// spectral regrowth widen the -3 dB width far more than they narrow it); returns 0 when no clear line stands out (small
// roll-offs give a weak one).
constexpr int kCyclicN = 1 << 18;
double cyclicRate(const std::vector<cf32>& x, double fs, double centre, double rs0) {
    if (x.size() < (size_t)kCyclicN) return 0;
    std::vector<cf32> v(x.end() - kCyclicN, x.end());
    Fft fft(kCyclicN);
    fft.forward(v.data());
    // keep the carrier only (up to 1.25 rs0 with a 0.35 roll-off), so that the noise beside it does not bury the line
    const double half = 0.5 * 1.25 * rs0 * 1.35;
    for (int k = 0; k < kCyclicN; k++) {
        // distance to the centre around the circle: a carrier near the band edge continues on the other side
        double df = std::fmod(std::fabs(k * fs / kCyclicN - centre), fs);
        df = std::min(df, fs - df);
        if (df > half) v[(size_t)k] = cf32(0, 0);
    }
    fft.inverse(v.data());
    double mean = 0;
    for (const cf32& z : v) mean += std::norm(z);
    mean /= kCyclicN;
    for (cf32& z : v) z = cf32((float)(std::norm(z) - mean), 0.f);
    fft.forward(v.data());
    // A rate above fs / 2 shows as its alias fs - rate: the squared envelope is real, so bin N - k holds the same as bin k, and the bins up to N
    // stand for the rates up to fs. A rate and its alias look the same; of the two the one nearer rs0 is taken.
    const int k0 = std::max(2, (int)std::floor(0.6 * rs0 * kCyclicN / fs)), k1 = std::min(kCyclicN - 2, (int)std::ceil(1.25 * rs0 * kCyclicN / fs));
    if (k1 <= k0 + 8) return 0;
    const double kr = rs0 * kCyclicN / fs;
    std::vector<float> mag((size_t)(k1 - k0 + 1));
    int best = k0;
    for (int k = k0; k <= k1; k++) {
        mag[(size_t)(k - k0)] = std::abs(v[(size_t)k]);
        const float m = mag[(size_t)(k - k0)], mb = mag[(size_t)(best - k0)];
        if (m > mb * 1.0001f || (m >= mb * 0.9999f && std::fabs(k - kr) < std::fabs(best - kr))) best = k;
    }
    std::vector<float> sorted = mag;
    std::nth_element(sorted.begin(), sorted.begin() + (std::ptrdiff_t)(sorted.size() / 2), sorted.end());
    const float median = sorted[sorted.size() / 2];
    if (!(mag[(size_t)(best - k0)] > 12.f * median) || best <= k0 || best >= k1) return 0;
    // parabolic interpolation of the peak
    const double a = mag[(size_t)(best - k0 - 1)], b = mag[(size_t)(best - k0)], c = mag[(size_t)(best - k0 + 1)];
    const double d = 0.5 * (a - c) / std::min(-1e-30, a - 2 * b + c);
    return (best + std::max(-0.5, std::min(0.5, d))) * fs / kCyclicN;
}

} // namespace

struct DvbsReceiver::Impl {
    enum Mode { kSearch = 0, kHunt = 1, kS1 = 2, kS2 = 3 };

    // ---- settings from other threads
    std::atomic<double> manualRs{0}, rollHint{0}, pickHz{0};
    std::atomic<bool> pickOn{false};
    std::mutex carMu, dumpMu, cidMu;
    std::atomic<bool> cidOn{false};
    std::atomic<double> hostCfo{0};
    std::unique_ptr<dvbcid::Tap> cidTap;
    std::unique_ptr<dvbcid::Rx> cidRx;
    double cidChipRate = 0;
    std::vector<cf32> cidBuf;
    std::vector<dvbcid::Frame> cidFrames;
    DvbsCid cidTel;
    FILE* dumpF = nullptr;
    std::string dumpPath;
    uint64_t dumpCount = 0;
    std::vector<DvbsCarrier> carriers;
    std::atomic<int> stdHint{0}, isiSel{-1}, plCode{-1}, computeMode{2};
    ComputeChoice s1Compute{&ComputeChoice::viterbiUsable, false};                  // DVB-S: where the Viterbi decoder runs
    std::atomic<bool> s1OnGpu{false};
    std::atomic<bool> blocking{false};
    std::atomic<bool> resetReq{false}, restartReq{false};
    std::mutex cbMu;
    std::function<void(const std::string&)> logCb;
    std::function<void(const uint8_t*, size_t, double)> pktCb;
    // IP data: GSE from the BBFRAMEs (here), MPE and ULE from the transport stream (feedTs); one sink for the statistics and the PCAP file
    std::mutex ipMu;
    struct IsiIp { dvbsip::GseIp gse; dvbsip::HemDeframer hem; uint64_t fields = 0; };
    std::map<int, IsiIp> isiIp;   // every input stream of the carrier has its own GSE reading (a multistream hub: user IP on one ISI, signalling on another)
    dvbsip::IpSink ipSink;
    uint64_t gseFields = 0, gseIpPackets = 0;
    dvbsip::IpMedia media;
    struct McastTs { TsDemux demux; uint64_t packets = 0, scrambled = 0; };
    std::map<std::string, std::unique_ptr<McastTs>> mcastTs;   // MPEG-TS found in UDP: at most four flows are analysed
    void initIp() {
        ipSink.onUdp = [this](const dvbsip::IpInfo& info, const uint8_t*, const uint8_t* payload, size_t n, double now) {
            size_t off = 0;
            if (!media.udp(info, payload, n, now, off)) return;
            char key[96];
            snprintf(key, sizeof key, "%s:%d", dvbsip::addrString(info.dst, false).c_str(), info.dport);
            auto it = mcastTs.find(key);
            if (it == mcastTs.end()) { if (mcastTs.size() >= 4) return; it = mcastTs.emplace(key, std::make_unique<McastTs>()).first; }
            for (size_t k = off; k + 188 <= n; k += 188) {
                it->second->demux.feed(payload + k);
                it->second->packets++;
                if ((payload[k + 3] >> 6) != 0) it->second->scrambled++;
            }
        };
    }
    dvbsip::MpeExtractor mpe;
    dvbsip::UleExtractor ule;
    std::string pcapPath;

    // ---- state of the receiver thread
    double fs = 0;
    Mode mode = kSearch;
    std::atomic<uint64_t> inputCount{0};
    uint64_t reportAt = 0;
    SpectrumEstimator spec;
    SpectrumResult specRes;
    std::vector<float> psdDb;
    int searchTries = 0;
    std::vector<cf32> raw;                      // the latest input while searching, for cyclicRate()
    std::vector<cf32> clean;                    // the input with non-finite samples replaced
    DvbsFront front;
    std::vector<cf32> sym, y;
    uint64_t symTotal = 0;
    bool timingNarrow = false, recentred = false, timingFine = false;
    double rsUsed = 0, rollUsed = 0.35;
    int rollSource = 0;
    // hunting
    std::vector<cf32> huntBuf;
    uint64_t huntSinceTry = 0;
    double rsNominal = 0;                       // the rate the spectrum (or the user) gave; hunts that fail try rates a little either side of it
    int huntFails = 0, ditherIdx = 0, huntCycles = 0;     // failed attempts at one rate, the rate in the dither list, complete lists tried
    // DVB-S: the carrier loop and the decoder run on a thread of their own, fed with the symbols of the front end
    QpskPll pll;
    S1Decoder s1;
    SymbolThread s1th;
    std::vector<cf32> s1y;
    std::vector<uint8_t> s1pk;
    uint64_t s1Symbols = 0, s1Mark = 0;      // symbols the thread has processed, and at the last packet delivery
    std::atomic<bool> s1Lost{false};
    std::atomic<double> rateNow{0};          // symbol rate of the front end, kept for the threads
    struct S1Snap {
        bool started = false, synced = false, inverted = false;
        int rate = 0, consecutiveBad = 0;
        float lock = 0, snrDb = 0;
        double freq = 0, byteErrors = 0;
        uint64_t rsClean = 0, rsCorrected = 0, rsFailed = 0;
    } s1Snap;
    std::mutex s1Mu, cellMu;
    uint64_t s2Symbols = 0;
    std::atomic<bool> tsLockFlag{false};
    // DVB-S2
    S2Rx s2;
    // packets and counters
    std::vector<uint8_t> pk;
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> packetsBad{0};
    std::atomic<uint64_t> lockSinceInput{0};
    std::vector<cf32> cells;
    size_t cellPos = 0;
    std::vector<float> merHist;
    uint64_t rateMark = 0, ratePackets = 0;
    double netBitrate = 0;
    // telemetry
    std::mutex tmu;
    DvbsTelemetry tel;
    uint64_t seq = 0;
    uint64_t seqBase = 0;

    void log(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(cbMu); cb = logCb; }
        if (cb) cb(s);
    }
    void logf(const char* f, double a = 0, double b = 0, const char* c = "", const char* d = "") {
        char buf[200];
        snprintf(buf, sizeof buf, f, a, b, c, d);
        log(buf);
    }

    void resetState() {
        mode = kSearch;
        inputCount = 0; reportAt = 0;
        spec.reset();
        specRes = SpectrumResult();
        searchTries = 0;
        sym.clear(); y.clear();
        symTotal = 0; timingNarrow = false; recentred = false; timingFine = false;
        huntBuf.clear(); huntSinceTry = 0;
        s1th.stop();
        s1.reset(); s1Symbols = 0; s1Mark = 0; s1Lost = false;
        s2.stop();
        packets = 0; packetsBad = 0;
        lockSinceInput = 0; tsLockFlag = false;
        { std::lock_guard<std::mutex> lk(cellMu); cells.clear(); cellPos = 0; }
        merHist.clear();
        rateMark = 0; ratePackets = 0; netBitrate = 0;
        psdDb.clear();
        { std::lock_guard<std::mutex> lk(cidMu); cidTap.reset(); cidRx.reset(); cidChipRate = 0; cidTel = DvbsCid(); }
    }

    // the carrier identification signal: the input mixed to the host carrier's centre and resampled to 8 samples per chip
    void cidFeed(const cf32* x, size_t n) {
        if (!cidOn.load() || mode == kSearch) return;
        const double chip = dvbcid::chipRate(front.config().symbolRate);
        std::lock_guard<std::mutex> lk(cidMu);
        if (!cidRx || cidChipRate != chip) {
            cidTap = std::make_unique<dvbcid::Tap>(fs, chip);
            cidRx = std::make_unique<dvbcid::Rx>(chip, true);
            cidChipRate = chip;
            cidTel = DvbsCid();
        }
        if (!cidTap->usable()) return;
        cidBuf.clear();
        cidTap->process(x, n, hostCfo.load(), cidBuf);
        if (cidBuf.empty()) return;
        cidFrames.clear();
        cidRx->push(cidBuf.data(), cidBuf.size(), cidFrames);
        const dvbcid::Stats s = cidRx->stats();
        DvbsCid& c = cidTel;
        c.on = true; c.acquired = s.acquired; c.offsetHz = s.offsetHz; c.snrDb = s.snrDb; c.bits = s.bits; c.frames = s.frames; c.badFrames = s.badFrames; c.clockPpm = s.clockPpm;
        if (s.report.haveGuid) { c.haveGuid = true; c.guid = dvbcid::guidText(s.report.guid); c.mac = dvbcid::guidMac(s.report.guid); }
        double la = 0, lo = 0;
        if (s.report.latitude(la) && s.report.longitude(lo)) { c.havePosition = true; c.latitude = la; c.longitude = lo; }
        c.telephone = s.report.telephone(); c.text = s.report.userText();
        for (const auto& f : cidFrames) log("carrier ID " + dvbcid::guidText(f.guid));
    }

    void surveyUpdate() {
        std::vector<SpectrumResult> all = spec.analyseAll(16);
        std::vector<DvbsCarrier> v;
        float top = -1e9f;
        for (const auto& r : all) top = std::max(top, r.plateauDb);
        for (const auto& r : all) {
            // leftovers of the blanking next to a real carrier, and noise humps, are not carriers
            if (r.snrDb < 3.f || r.fitRms > 2.5f) continue;
            bool shadow = false;
            for (const auto& o : all)
                if (&o != &r && o.plateauDb > r.plateauDb + 6.f && std::fabs(o.centerHz - r.centerHz) < 0.75 * (o.rateHz + r.rateHz)) shadow = true;
            if (shadow) continue;
            DvbsCarrier c;
            c.centerHz = r.centerHz; c.rateHz = r.rateHz; c.rollOff = r.rollOff; c.snrDb = r.snrDb; c.levelDb = r.plateauDb - top;
            c.selected = specRes.valid && std::fabs(r.centerHz - specRes.centerHz) < 1.0;
            v.push_back(c);
        }
        std::lock_guard<std::mutex> lk(carMu);
        carriers.swap(v);
    }

    // ------------------------------------------------------------------------ search: spectrum -> front end
    void doSearch() {
        if (spec.segments() < 48) return;
        spec.setPick(pickOn.load(), pickHz.load());
        specRes = spec.analyse();
        surveyUpdate();
        spec.display(psdDb, 512);
        const double manual = manualRs.load();
        const bool carrier = specRes.valid && specRes.zscore >= 8.f && specRes.fitRms < 2.5f;
        double rs = manual > 0 ? manual : specRes.rateHz;
        if (manual <= 0 && carrier) {
            const double rc = cyclicRate(raw, fs, specRes.centerHz, rs);
            if (rc > 0) {
                if (std::fabs(rc / rs - 1) > 0.01) logf("symbol rate %.3f Msym/s from the spectrum's width, %.4f from its cyclic line", rs / 1e6, rc / 1e6);
                rs = rc;
            }
        }
        if (!carrier || rs < dvbsMinAutoSymbolRate(fs) * (manual > 0 ? 0.0 : 1.0) || rs > dvbsMaxSymbolRate(fs, 0.35) * 1.02) {
            spec.reset();
            searchTries++;
            return;
        }
        startFront(specRes, rs);
    }

    void startFront(const SpectrumResult& r, double rs) {
        double roll = 0.35;
        rollSource = 0;
        if (rollHint.load() > 0) { roll = rollHint.load(); rollSource = 1; }
        else if (r.rollOff > 0 && r.snrDb > 15.0f) { roll = snapRollOff(r.rollOff); rollSource = 1; }
        DvbsFront::Config c;
        c.fs = fs; c.symbolRate = rs; c.centerHz = r.centerHz; c.rollOff = roll;
        front.configure(c);
        front.timer().setLoop(0.02, 0.707);
        rsUsed = rs; rollUsed = roll;
        symTotal = 0; timingNarrow = false; recentred = false; timingFine = false;
        huntBuf.clear(); huntSinceTry = 0;
        sfStrip.reset(); sfWasLocked = sfWasPilots = false;
        rsNominal = rs; huntFails = 0; ditherIdx = 0; huntCycles = 0;
        mode = kHunt;
        logf("carrier found: %.3f Msym/s, %+.0f kHz from the centre", rs / 1e6, r.centerHz / 1e3);
    }

    // ------------------------------------------------------------------------ the symbol path
    // DVB-S2X super-frames (Annex E): their own symbols are taken out here so that the hunt and the receiver see plain PLFRAMEs
    SfStripper sfStrip;
    bool sfWasLocked = false, sfWasPilots = false;
    std::vector<cf32> sfOut;
    bool sfPilotsNow() const { const SfView v = sfStrip.view(); return v.carriesPlframes() && v.pilots; }

    void runFront(const cf32* x, size_t n) {
        sym.clear();
        front.process(x, n, sym);
        if (!sym.empty() && (mode == kHunt || mode == kS2 || mode == kS1) && stdHint.load() != 1) {   // DVB-S (kS1) keeps passing through it: the stream the hunt saw continues without a gap
            sfOut.clear(); sfStrip.push(sym.data(), sym.size(), sfOut); sym.swap(sfOut);
            // the super-frame grid was found (or its pilots) while the frames were being followed: their symbols were not taken out until now, so the receiver starts again
            const bool lk = sfStrip.view().carriesPlframes(), pl = sfPilotsNow();
            if (mode == kS2 && ((lk && !sfWasLocked) || (pl && !sfWasPilots))) { s2.stop(); tsLockFlag = false; mode = kHunt; huntBuf.clear(); huntSinceTry = 0; }
            sfWasLocked = lk; sfWasPilots = pl;
        }
        if (sym.empty()) return;
        {
            std::lock_guard<std::mutex> lk(dumpMu);
            if (dumpF && dumpCount < 64ull * 1024 * 1024) { fwrite(sym.data(), sizeof(cf32), sym.size(), dumpF); dumpCount += sym.size(); }
        }
        symTotal += sym.size();
        rateNow = front.symbolRateNow();
        if (!timingNarrow && symTotal > 40000) { front.timer().setLoop(0.004, 0.707); timingNarrow = true; }
        if (!recentred && symTotal > 200000) { front.recentre(); recentred = true; }
        // once the rate offset is taken out the loop only has the timing jitter left to fight: a narrower loop has less of it near threshold
        if (!timingFine && recentred && symTotal > 300000) { front.timer().setLoop(0.0018, 0.707); timingFine = true; }
        switch (mode) {
        case kHunt: huntPush(); break;
        case kS1: s1Push(); break;
        case kS2: s2Push(); break;
        default: break;
        }
    }

    void addCells(const cf32* z, size_t n) {
        std::lock_guard<std::mutex> lk(cellMu);
        if (cells.size() < kCellsMax) cells.resize(kCellsMax, cf32());
        for (size_t i = 0; i < n; i += 4) { cells[cellPos] = z[i]; cellPos = (cellPos + 1) % kCellsMax; }   // every fourth symbol keeps the display current
    }

    void huntPush() {
        huntBuf.insert(huntBuf.end(), sym.begin(), sym.end());
        if (huntBuf.size() > 2 * kHuntKeep) huntBuf.erase(huntBuf.begin(), huntBuf.end() - (std::ptrdiff_t)kHuntKeep);
        huntSinceTry += sym.size();
        // a signal that is not there is looked for less often: the interval doubles after each complete round of rates, up to four times as long
        const uint64_t interval = std::max<uint64_t>(65536, (uint64_t)(0.15 * rsUsed)) * (uint64_t)std::min(4, 1 << std::min(huntCycles, 2));
        if (huntBuf.size() >= kHuntWindow && huntSinceTry >= interval && symTotal > 20000) {
            huntSinceTry = 0;
            if (tryDvbs2() || tryDvbs()) { huntFails = 0; return; }
            // The spectrum gives the symbol rate to a percent or two at best, and in noise the timing loop cannot pull in much more than that:
            // after a few attempts the rate is moved a little and the front end starts again
            if (++huntFails >= 3 && huntBuf.size() >= 150000) {
                static const double d[6] = {0.012, -0.012, 0.024, -0.024, 0.036, -0.036};
                const double f = ditherIdx < 6 ? d[ditherIdx] : 0.0;
                ditherIdx = (ditherIdx + 1) % 7;
                if (ditherIdx == 0) huntCycles++;
                huntFails = 0;
                DvbsFront::Config c = front.config();
                c.symbolRate = rsNominal * (1.0 + f);
                front.configure(c);
                front.timer().setLoop(0.02, 0.707);
                rsUsed = c.symbolRate;
                symTotal = 0; timingNarrow = false; recentred = false; timingFine = false;
                huntBuf.clear(); huntSinceTry = 0;
            }
        }
    }

    bool tryDvbs2() {
        if (stdHint.load() == 1 || huntBuf.size() < 100000) return false;
        const size_t n = std::min(huntBuf.size(), kS2Window);
        const cf32* w = huntBuf.data() + huntBuf.size() - n;
        const S2HuntResult h = s2Hunt(w, n, sfPilotsNow());
        if (!h.ok) return false;
        s2.setSymbolRate(front.symbolRateNow());
        s2.setPlCode(plCode.load());
        s2.setIsi(isiSel.load());
        s2.setSuperframePilots(sfPilotsNow());
        s2.setBlocking(blocking.load());
        s2.setDataFieldCallback([this](const uint8_t* bb, size_t n, int mod, int rate, bool sh, int tsGs) {
            std::vector<dvbsip::IpPacket> pk;
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (n < 10) return;
            const bool sis = (bb[0] >> 5) & 1;
            const int isi = sis ? 0 : bb[1];
            std::lock_guard<std::mutex> lk(ipMu);
            IsiIp& st = isiIp[isi];
            st.gse.setModcod(mod * 1000 + rate * 2 + (sh ? 1 : 0));
            st.fields++; gseFields++;
            if (tsGs == 2) {   // GSE-HEM: the packets are sliced across BBFRAMEs, SYNCD says where the first one starts
                std::vector<uint8_t> whole;
                st.hem.dataField(bb + 10, n - 10, (uint16_t)(bb[7] << 8 | bb[8]), whole);
                if (!whole.empty()) st.gse.dataField(whole.data(), whole.size(), pk);
            } else st.gse.bbframe(bb, n, pk);
            for (const auto& q : pk) { gseIpPackets++; ipSink.push(q.data.data(), q.data.size(), now); }
        });
        s2.setPacketCallback([this](const uint8_t* p, size_t np, double secs) {
            std::function<void(const uint8_t*, size_t, double)> cb;
            { std::lock_guard<std::mutex> lk(cbMu); cb = pktCb; }
            packets.fetch_add(np);
            if (!tsLockFlag.exchange(true)) { lockSinceInput = inputCount.load(); log("transport stream lock"); }
            if (cb) cb(p, np, secs);
        });
        s2.setLogCallback([this](const std::string& m) { log(m); });
        s2.start(h, w, n);
        front.timer().setLoop(0.001, 0.707);       // the frames carry the timing now: a narrow loop wanders less
        s2Symbols = 0;
        mode = kS2;
        int mod, rate;
        if (h.modcod != 0 && s2ModcodSplit(h.modcod, mod, rate))
            log(std::string(s2ModcodIsS2x(h.modcod) ? "DVB-S2X, " : "DVB-S2, ") + s2ModNameFor(mod, rate) + " " + s2RateName(rate) + (h.shortFrame ? " short" : "") +
                (h.pilots ? " pilots" : "") + (h.inverted ? ", spectrum inverted" : ""));
        else log("DVB-S2, dummy frames");
        return true;
    }

    void s2Push() {
        s2Symbols += sym.size();
        s2.push(sym.data(), sym.size());
        if (s2.lost()) {
            s2.stop();
            tsLockFlag = false;
            mode = kHunt; huntBuf.clear(); huntSinceTry = 0;
        }
    }

    bool tryDvbs() {
        if (stdHint.load() == 2) return false;
        const cf32* w = huntBuf.data() + huntBuf.size() - kHuntWindow;
        const QpskEstimate e = qpskEstimate(w, 16384);
        if (!e.ok) return false;
        std::vector<cf32> yy(kHuntWindow);
        QpskPll p;
        p.start(e.freq, e.phase, 0.02);
        p.process(w, 16384, yy.data());
        p.setBandwidth(0.005);
        p.process(w + 16384, kHuntWindow - 16384, yy.data() + 16384);
        const S1Hypothesis h = s1Search(yy.data() + 16384, kHuntWindow - 16384);
        if (!h.ok) return false;
        pll = p;
        s1.setCompute(&s1Compute);
        s1Compute.autoAllowed = front.symbolRateNow() >= 8e6;      // 8 Msym/s and up: a window of a million steps is under a tenth of a second
        s1.start(h, kHuntWindow - 16384);
        s1Symbols = 0; s1Mark = 0; s1Lost = false;
        snapS1();
        // blocking (a file): a short queue, so the front end cannot run seconds ahead of the decoder; the symbols queued when the decoder
        // loses sync are thrown away, and how many that were depended on the thread scheduling
        s1th.start([this](const cf32* z, size_t m) { s1Work(z, m); }, blocking.load() ? 1u << 16 : 1u << 22, blocking.load());
        mode = kS1;
        static const char* rn[5] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
        log(std::string("DVB-S, QPSK ") + rn[h.rate] + ((h.variant & 2) ? ", spectrum inverted" : ""));
        return true;
    }

    // runs on the DVB-S thread
    void s1Work(const cf32* z, size_t n) {
        if (s1Lost.load()) return;           // sync lost: the receiver thread stops this one and hunts again
        s1y.resize(n);
        pll.process(z, n, s1y.data());
        addCells(s1y.data(), n);
        s1Symbols += n;
        if (s1.started()) {
            s1.push(s1y.data(), n);
            s1OnGpu = s1.lastOnGpu();
            s1pk.clear();
            s1.takePackets(s1pk);
            deliver();
        }
        if (!s1.started()) {                 // the decoder lost its hypothesis: look again
            log("DVB-S: sync lost");
            tsLockFlag = false;
            s1Lost = true;
        } else if (!s1.synced() && s1Symbols > 600000) {
            log("DVB-S: no packet sync, searching again");
            s1.reset();
            s1Lost = true;
        }
        snapS1();
    }

    void snapS1() {
        std::lock_guard<std::mutex> lk(s1Mu);
        s1Snap.started = s1.started(); s1Snap.synced = s1.synced(); s1Snap.inverted = (s1.hypothesis().variant & 2) != 0;
        s1Snap.rate = s1.rate(); s1Snap.consecutiveBad = s1.consecutiveBad();
        s1Snap.lock = pll.lock(); s1Snap.snrDb = pll.snrDb(); s1Snap.freq = pll.freq();
        s1Snap.byteErrors = s1.byteErrorRate();
        s1Snap.rsClean = s1.rsClean; s1Snap.rsCorrected = s1.rsCorrected; s1Snap.rsFailed = s1.rsFailed;
    }

    void s1Push() {
        s1th.push(sym.data(), sym.size());
        if (s1Lost.load()) {
            s1th.stop();
            s1Lost = false;
            mode = kHunt; huntBuf.clear(); huntSinceTry = 0;
        }
    }

    void deliver() {
        if (s1pk.empty()) return;
        const size_t np = s1pk.size() / 188;
        uint64_t bad = 0;
        for (size_t i = 0; i < np; i++) if (s1pk[i * 188 + 1] & 0x80) bad++;
        packets += np; packetsBad += bad;
        const double secs = (double)(s1Symbols - s1Mark) / std::max(1.0, rateNow.load());
        s1Mark = s1Symbols;
        std::function<void(const uint8_t*, size_t, double)> cb;
        { std::lock_guard<std::mutex> lk(cbMu); cb = pktCb; }
        if (cb) cb(s1pk.data(), np, secs);
        if (np > bad && !tsLockFlag.exchange(true)) { lockSinceInput = inputCount.load(); log("transport stream lock"); }
    }

    // ------------------------------------------------------------------------ IP data
    static std::string modcodText(int code) {
        const int mod = code / 1000, rest = code % 1000, rate = rest / 2;
        return std::string(s2ModNameFor(mod, rate)) + " " + s2RateName(rate) + ((rest & 1) ? " short" : "");
    }
    void fillIp(DvbsIpTel& o) {
        using namespace dvbsip;
        std::lock_guard<std::mutex> lk(ipMu);
        o.gseFields = gseFields;
        o.packets = ipSink.stats.packets; o.bytes = ipSink.stats.bytes; o.ipv4 = ipSink.stats.ipv4; o.ipv6 = ipSink.stats.ipv6;
        o.fragments = ipSink.frag.fragments; o.reassembled = ipSink.frag.reassembled;
        o.mpePackets = mpe.stats.datagrams; o.ulePackets = ule.stats.ipDatagrams;
        o.active = o.packets > 0;
        std::string src;
        std::map<uint16_t, uint64_t> others;
        uint64_t otherBytesJudged = 0;
        bool encrypted = false;
        double entropy = -1;
        std::vector<std::pair<std::pair<int, Label>, const LabelStats*>> lv;
        for (const auto& kv : isiIp) {
            const GseIp& g = kv.second.gse;
            const std::string tag = isiIp.size() > 1 ? " ISI " + std::to_string(kv.first) : std::string();
            const int sc = g.source();
            std::string one;
            if (sc == 1) one = std::string("GSE") + tag + " (" + g.reports()[(size_t)g.chosen()].variant.label() + ")";
            else if (sc == 2) one = "GSE" + tag + " (blind search for IPv4)";
            else if (kv.second.fields > 0 && !g.otherProtocols().empty()) one = "GSE" + tag + ", no IP";
            if (!one.empty()) src += std::string(src.empty() ? "" : ", ") + one + (kv.second.hem.stats.fields ? " HEM" : "");
            for (const auto& q : g.otherProtocols()) others[q.first] += q.second;
            if (g.otherEntropy() >= 0) { entropy = std::max(entropy, g.otherEntropy()); otherBytesJudged++; }
            encrypted = encrypted || g.otherEncrypted();
            for (const auto& l : g.labels()) lv.push_back({{kv.first, l.first}, &l.second});
        }
        if (mpe.stats.datagrams) src += std::string(src.empty() ? "" : ", ") + "MPE";
        if (ule.stats.ipDatagrams) src += std::string(src.empty() ? "" : ", ") + "ULE";
        o.source = src;
        auto top = [](const std::map<uint16_t, uint64_t>& m, std::vector<std::pair<std::string, uint64_t>>& out, size_t n) {
            std::vector<std::pair<uint16_t, uint64_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < v.size() && i < n; i++) out.push_back({std::to_string(v[i].first), v[i].second});
        };
        top(ipSink.stats.udpPorts, o.udpPorts, 12); top(ipSink.stats.tcpPorts, o.tcpPorts, 12);
        std::vector<std::pair<uint8_t, uint64_t>> pr(ipSink.stats.protocols.begin(), ipSink.stats.protocols.end());
        std::sort(pr.begin(), pr.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& q : pr) o.protocols.push_back({std::string(protocolName(q.first)) + (std::string(protocolName(q.first)) == "other" ? " " + std::to_string(q.first) : std::string()), q.second});
        for (const Flow& f : ipSink.stats.topFlows(40)) {
            DvbsIpFlow d;
            d.src = addrString(f.key.src, f.key.v6); d.dst = addrString(f.key.dst, f.key.v6); d.protocol = protocolName(f.key.protocol);
            if (f.key.sport || f.key.dport) { d.sport = f.key.sport; d.dport = f.key.dport; }
            d.packets = f.packets; d.bytes = f.bytes; d.encrypted = f.encrypted();
            o.flows.push_back(d);
        }
        o.flowCount = ipSink.stats.flowCount(); o.encryptedFlows = ipSink.stats.encryptedFlows();
        for (const auto& q : others) { char b[16]; snprintf(b, sizeof b, "%04X", q.first); o.otherPdus.push_back({b, q.second}); }
        std::sort(o.otherPdus.begin(), o.otherPdus.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        o.otherEntropy = entropy; o.otherEncrypted = encrypted;
        o.labelCount = lv.size();
        std::sort(lv.begin(), lv.end(), [](const auto& a, const auto& b) { return a.second->bytes > b.second->bytes; });
        for (size_t i = 0; i < lv.size() && i < 40; i++) {
            DvbsIpLabel d;
            d.label = (isiIp.size() > 1 ? "ISI " + std::to_string(lv[i].first.first) + " " : std::string()) + lv[i].first.second.text();
            d.pdus = lv[i].second->pdus; d.bytes = lv[i].second->bytes; d.ip = lv[i].second->ip;
            for (size_t k = 0; k < lv[i].second->addrs.size(); k++) d.addrs.push_back(addrString(lv[i].second->addrs[k], lv[i].second->addrV6[k]));
            uint64_t tot = 0;
            for (const auto& m : lv[i].second->modcods) tot += m.second;
            std::vector<std::pair<int, uint64_t>> mv(lv[i].second->modcods.begin(), lv[i].second->modcods.end());
            std::sort(mv.begin(), mv.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t k = 0; k < mv.size() && k < 4; k++) { char b[64]; snprintf(b, sizeof b, "%s%.0f %%", k ? ", " : "", 100.0 * (double)mv[k].second / (double)std::max<uint64_t>(1, tot)); d.modcods += modcodText(mv[k].first) + " " + b; }
            o.labels.push_back(std::move(d));
        }
        for (const auto& m : media.flows()) {
            DvbsMediaFlow d;
            d.dst = m.dst; d.src = m.src; d.port = m.port; d.kind = m.kind; d.codec = m.codec;
            d.packets = m.packets; d.bytes = m.bytes; d.lost = m.lost; d.kbps = m.kbps;
            char key[96];
            snprintf(key, sizeof key, "%s:%d", m.dst.c_str(), m.port);
            auto it = mcastTs.find(key);
            if (it != mcastTs.end()) {
                const TsSnapshot sn = it->second->demux.snapshot();
                for (const auto& sv : sn.services) d.services.push_back((sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name) + " (" + sv.typeName() + ")" + (sv.scrambled ? " scrambled" : ""));
                d.scrambled = it->second->scrambled > 0 && it->second->scrambled * 2 > it->second->packets;
            }
            if (o.media.size() < 40) o.media.push_back(std::move(d));
        }
        for (const auto& sdp : media.sessions()) {
            DvbsSdp d;
            d.name = sdp.name; d.info = sdp.info; d.address = sdp.address; d.source = sdp.source; d.seen = sdp.seen;
            for (const auto& md : sdp.media) {
                std::string c;
                for (int pt : md.payloadTypes) { auto r = md.rtpmap.find(pt); c += (c.empty() ? "" : "/") + (r != md.rtpmap.end() ? r->second : std::string(rtpPayloadName(pt))); }
                d.media += (d.media.empty() ? "" : ", ") + md.type + " " + std::to_string(md.port) + (c.empty() ? "" : " (" + c + ")");
            }
            if (o.sdp.size() < 40) o.sdp.push_back(std::move(d));
        }
        o.pcapPath = ipSink.pcap.isOpen() ? ipSink.pcap.path : std::string();
        o.pcapPackets = ipSink.pcap.packets; o.pcapBytes = ipSink.pcap.bytes;
        (void)otherBytesJudged;
    }

    // ------------------------------------------------------------------------ telemetry
    void publish() {
        DvbsTelemetry t;
        t.computeMode = computeMode.load(); t.gpuAvailable = ComputeChoice::gpuUsable() || ComputeChoice::viterbiUsable();
        const double nowSec = (double)inputCount / fs;
        t.standard = mode == kS1 ? 1 : mode == kS2 ? 2 : 0;
        t.symbolRate = mode == kSearch ? (specRes.valid ? specRes.rateHz : 0) : front.symbolRateNow();
        t.symbolRateManual = manualRs.load() > 0;
        { std::lock_guard<std::mutex> lk(carMu); t.carriers = carriers; }
        { std::lock_guard<std::mutex> lk(dumpMu); t.symbolDumpPath = dumpPath; t.symbolDumpCount = dumpCount; }
        t.carrierPicked = pickOn.load(); t.carrierPickHz = pickHz.load();
        { std::lock_guard<std::mutex> lk(cidMu); t.cid = cidTel; t.cid.on = cidOn.load(); }
        t.symbolRateSpectrum = specRes.valid ? specRes.rateHz : 0;
        t.symbolRateLoop = mode == kSearch ? 0 : front.symbolRateNow();
        t.spectrumSnrDb = specRes.valid ? specRes.snrDb : 0;
        t.spectrumFitRms = specRes.fitRms;
        t.rollOff = (float)rollUsed;
        t.rollOffSource = rollSource;
        t.lockSpectrum = mode != kSearch;
        t.lockTiming = mode != kSearch && timingNarrow && front.timer().errorRms() > 0 && front.timer().errorRms() < 0.6f;
        t.psdDb = psdDb;
        t.psdSpanHz = fs;
        t.carrierLoHz = specRes.valid ? specRes.edgeLoHz : 0;
        t.carrierHiHz = specRes.valid ? specRes.edgeHiHz : 0;
        t.cfoHz = specRes.valid ? specRes.centerHz : 0;
        if (mode == kS1) {
            S1Snap q;
            { std::lock_guard<std::mutex> lk(s1Mu); q = s1Snap; }
            t.modulation = 0; t.modulationName = "QPSK";
            static const char* rn[5] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
            t.codeRate = rn[q.rate];
            t.inverted = q.inverted;
            t.lockCarrier = q.lock > 0.4f;
            t.snrDb = q.snrDb; t.merDb = q.snrDb;
            t.cfoHz = front.config().centerHz + q.freq * front.symbolRateNow();
            t.lockFrame = q.synced;
            t.rsClean = q.rsClean; t.rsCorrected = q.rsCorrected; t.rsFailed = q.rsFailed;
            t.onGpu = s1OnGpu;
            t.blocksOk = q.rsClean + q.rsCorrected; t.blocksBad = q.rsFailed;
            t.lockFec = q.synced && q.consecutiveBad < 8;
            // bit error rate after the Viterbi decoder, from the bytes Reed-Solomon had to change (about 1.5 bit errors in a byte it changes); too low when blocks fail
            t.preFecBer = q.rsClean + q.rsCorrected > 100 ? (float)std::min(0.5, q.byteErrors * 1.5 / 8.0) : -1.f;
        } else if (mode == kS2) {
            const S2Stats s = s2.stats();
            if (s.mod >= 0) { t.modulation = s.mod; t.modulationName = s2ModNameFor(s.mod, s.rate); t.codeRate = s2RateName(s.rate); }
            // an S2X MODCOD makes it DVB-S2X; its number is shown as the PLS code value of EN 302 307-2 table 17a
            if (s2IsS2x(s.rate) || s2ModcodIsS2x(s.modcod) || s.framesUnsupported > 0) t.standard = 3;
            t.modcod = s2ModcodIsS2x(s.modcod) ? s.modcod << 1 : s.modcod;
            t.frameSize = s.shortFrame ? 2 : 1; t.pilots = s.pilots; t.vcm = s.vcm; t.isi = s.isi;
            if (s.vlMode >= 0) {
                // VL-SNR frame (EN 302 307-2 table 18a): the mode, not a MODCOD of table 17
                const VlMode& m = vlMode(s.vlMode);
                t.standard = 3; t.modulation = 0;
                t.modulationName = m.bpsk ? "pi/2 BPSK" : "QPSK";
                t.codeRate = std::string(m.rate) + (m.spread == 2 ? " SF2" : "");
                t.modcod = vlPlsCode(m.set);
                t.frameSize = m.nldpc == 16200 ? 2 : m.nldpc == 64800 ? 1 : 3;
            }
            t.plScramblingCode = plCode.load() < 0 ? 0 : plCode.load();
            t.inverted = s2.inverted();
            t.merDb = (float)s.merDb; t.snrDb = (float)s.merDb;
            t.lockCarrier = s2.carrierLocked();
            t.cfoHz = front.config().centerHz + s.carrierRadPerSym / (2 * 3.14159265358979) * front.symbolRateNow();
            t.lockFrame = s2.tracking() && s.consecutiveHeaderBad == 0 && s.framesSeen > 0;
            t.lockFec = s.consecutiveBad < 3 && s.fecOk > 0;
            t.blocksOk = s.fecOk; t.blocksBad = s.fecBad;
            t.bchOk = s.fecOk; t.bchBad = s.bchBad;
            t.ldpcIterAvg = (float)s.ldpcIterAvg;
            t.onGpu = s.ldpcOnGpu;
            t.preFecBer = (float)s.berPre;
            t.framesSeen = s.framesSeen; t.framesDummy = s.framesDummy; t.crcErrors = s.crcErrors; t.gseFrames = s.gseFrames;
            t.framesUnsupported = s.framesUnsupported;
            if (s.roSignalled >= 0 && s.roSignalled < 6) { static const float kRo[6] = {0.35f, 0.25f, 0.20f, 0.15f, 0.10f, 0.05f}; t.rollOff = kRo[s.roSignalled]; t.rollOffSource = 2; }
            { const SfView sv = sfStrip.view(); if (sv.superframes > 0 && sv.format >= 0) { char b[200]; snprintf(b, sizeof b, "super-frames (EN 302 307-2 annex E): format %d (%s), SF-pilots %s, %llu read%s%s", sv.format, sfFormatName(sv.format), sv.pilots ? "on" : "off", (unsigned long long)sv.superframes, sv.losses ? (", " + std::to_string(sv.losses) + " losses").c_str() : "", sv.format > 1 ? "; not decoded (only formats 0 and 1 are)" : ""); t.superframe = b; } }
            if (s.gseFrames > 0 && s.tsGs != 3) t.signalNote = "generic stream (GSE) seen: IP packets are listed under IP DATA";
            if (s.framesUnsupported > 0) {
                const int c = s.unsupportedCode;
                t.signalNote = c == 129 || c == 131 ? std::string("S2X VL-SNR frames seen (set ") + (c == 129 ? "1" : "2") + "): header not recognised"
                                                     : "S2X frames with the reserved PLS code " + std::to_string(c) + " seen: not decoded";
            }
            { std::lock_guard<std::mutex> lk(cellMu); s2.cells(cells); }
            for (const auto& kv : s.streams) {
                const S2StreamStat& q = kv.second;
                DvbsStream d;
                d.isi = q.isi; d.single = q.single; d.decoded = (q.tsGs == 3 || (q.upl == 1504 && q.tsGs != 2)) && (q.single || q.isi == s.isi);
                char b[64];
                if (q.tsGs == 3) d.content = "transport stream";
                else if (q.tsGs == 2) d.content = q.npd ? "GSE-HEM, GSE-Lite" : "GSE-HEM";
                else if (q.upl == 0) d.content = q.npd ? "GSE-Lite" : "GSE";
                else if (q.upl == 1504) { snprintf(b, sizeof b, "transport packets in a generic stream (sync %02Xh)", q.sync); d.content = b; }
                else { snprintf(b, sizeof b, "%d-byte packets (sync %02Xh)", q.upl / 8, q.sync); d.content = b; }
                d.ccm = q.ccm; d.issyi = q.issyi; d.npd = q.npd; d.hem = q.hem; d.frames = q.frames; d.bps = q.bps;
                uint64_t tot = 0;
                for (const auto& m : q.modcods) tot += m.second;
                std::vector<std::pair<int, uint64_t>> mv(q.modcods.begin(), q.modcods.end());
                std::sort(mv.begin(), mv.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                for (size_t k = 0; k < mv.size() && k < 3; k++) { snprintf(b, sizeof b, "%s%.0f %%", k ? ", " : "", 100.0 * (double)mv[k].second / (double)std::max<uint64_t>(1, tot)); d.modcods += modcodText(mv[k].first) + " " + b; }
                t.streams.push_back(std::move(d));
            }
        } else if (mode == kHunt) {
            t.snrDb = specRes.valid ? specRes.snrDb : 0;
        } else {
            t.snrDb = 0;
        }
        fillIp(t.ip);   // GSE, MPE and ULE
        { const S2Stats s = mode == kS2 ? s2.stats() : S2Stats(); t.packetsBad = mode == kS2 ? s.packetsBad : packetsBad.load(); }
        t.packets = packets.load();
        t.tsLock = tsLockFlag.load() && t.lockFec;
        t.dataValid = t.tsLock;
        t.secsSinceLock = t.tsLock ? nowSec - (double)lockSinceInput / fs : 0;
        t.netBitrate = netBitrate;
        t.state = t.dataValid ? 2 : (mode == kSearch ? 0 : 1);
        {
            std::lock_guard<std::mutex> lk(cellMu);
            if (!cells.empty()) {
                const size_t n = std::min(cells.size(), kCellsMax);
                t.cells.assign(cells.end() - (std::ptrdiff_t)n, cells.end());
            }
        }
        // the transport stream bit rate over the last second or so
        if (inputCount.load() - rateMark >= (uint64_t)fs) {
            const double dt = (double)(inputCount.load() - rateMark) / fs;
            const uint64_t pk = packets.load();
            netBitrate = (double)(pk - ratePackets) * 188.0 * 8.0 / dt;
            rateMark = inputCount.load(); ratePackets = pk;
        }
        merHist.push_back(t.merDb);
        if (merHist.size() > 120) merHist.erase(merHist.begin());
        t.merHistory = merHist;
        if (mode != kSearch && std::isfinite(t.cfoHz)) hostCfo = t.cfoHz;   // where the carrier identification signal's tap centres
        {
            std::lock_guard<std::mutex> lk(tmu);
            seq++;
            t.seq = seq;
            tel = std::move(t);
        }
    }
};

// ============================================================================ the class
DvbsReceiver::DvbsReceiver() : p_(std::make_unique<Impl>()) { p_->initIp(); }
DvbsReceiver::~DvbsReceiver() {
    // the threads use the members of Impl: stop them while those are all alive
    p_->s1th.stop();
    p_->s2.stop();
    setSymbolDump(std::string());
}

void DvbsReceiver::configure(double inputRateHz) {
    p_->fs = inputRateHz;
    p_->spec.configure(inputRateHz, 4096);
    p_->spec.setStride((size_t)std::max(4096.0, inputRateHz * 0.004));
    p_->resetState();
}
bool DvbsReceiver::ready() const { return p_->fs >= dvbsTuning().minSampleRate - 1; }
void DvbsReceiver::reset() { p_->resetReq = true; }
void DvbsReceiver::resetNow() {
    Impl& I = *p_;
    const uint64_t keep = I.seq;
    I.resetState();   // ends the worker threads (no packet reaches the callback afterwards); the caller makes sure nothing feeds the receiver
    I.seq = keep;
    I.resetReq = false; I.restartReq = false;
}
void DvbsReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->cbMu); p_->logCb = std::move(cb); }
void DvbsReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { std::lock_guard<std::mutex> lk(p_->cbMu); p_->pktCb = std::move(cb); }
void DvbsReceiver::setSymbolDump(const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->dumpMu);
    if (p_->dumpF) { fclose(p_->dumpF); p_->dumpF = nullptr; }
    p_->dumpPath.clear(); p_->dumpCount = 0;
    if (path.empty()) return;
    p_->dumpF = fopen(path.c_str(), "wb");
    if (p_->dumpF) p_->dumpPath = path;
}
void DvbsReceiver::setCidSearch(bool on) { p_->cidOn = on; if (!on) { std::lock_guard<std::mutex> lk(p_->cidMu); p_->cidTap.reset(); p_->cidRx.reset(); p_->cidChipRate = 0; p_->cidTel = DvbsCid(); } }
void DvbsReceiver::selectCarrier(bool on, double offsetHz) {
    if (on != p_->pickOn.load() || (on && offsetHz != p_->pickHz.load())) { p_->pickHz = offsetHz; p_->pickOn = on; p_->restartReq = true; }
}
void DvbsReceiver::setSymbolRate(double hz) { if (hz != p_->manualRs.load()) { p_->manualRs = hz; p_->restartReq = true; } }
void DvbsReceiver::setStandardHint(int s) { if (s != p_->stdHint.load()) { p_->stdHint = s; p_->restartReq = true; } }
void DvbsReceiver::setRollOff(double r) { if (r != p_->rollHint.load()) { p_->rollHint = r; p_->restartReq = true; } }
void DvbsReceiver::setIsi(int isi) { p_->isiSel = isi; }
void DvbsReceiver::setIpPcap(const std::string& path) {
    std::lock_guard<std::mutex> lk(p_->ipMu);
    p_->ipSink.pcap.close();
    if (!path.empty()) p_->ipSink.pcap.open(path);
}
void DvbsReceiver::feedTs(const uint8_t* pk, size_t n) {
    Impl& I = *p_;
    std::vector<std::vector<uint8_t>> out;
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(I.ipMu);
    for (size_t i = 0; i < n; i++, pk += 188) {
        if (pk[0] != 0x47) continue;
        out.clear();
        I.mpe.packet(pk, out);
        for (const auto& d : out) I.ipSink.push(d.data(), d.size(), now);
        out.clear();
        I.ule.packet(pk, out);
        for (const auto& d : out) I.ipSink.push(d.data(), d.size(), now);
    }
}
void DvbsReceiver::clearIp() {
    std::lock_guard<std::mutex> lk(p_->ipMu);
    const bool pc = p_->ipSink.pcap.isOpen();
    const std::string path = p_->ipSink.pcap.path;
    p_->isiIp.clear();
    p_->ipSink.stats.clear();
    p_->ipSink.frag = dvbsip::Reassembler();
    p_->gseFields = p_->gseIpPackets = 0;
    p_->mpe = dvbsip::MpeExtractor(); p_->ule = dvbsip::UleExtractor(); p_->media.clear(); p_->mcastTs.clear();
    if (pc) p_->ipSink.pcap.open(path);
}
void DvbsReceiver::setPlScrambling(int n) { p_->plCode = n; }
void DvbsReceiver::setComputeMode(int m) { p_->computeMode = m; p_->s2.setComputeMode(m); p_->s1Compute.mode = m; }
void DvbsReceiver::setBlocking(bool b) { p_->blocking = b; }
void DvbsReceiver::flush() {
    Impl& I = *p_;
    if (I.mode == Impl::kS1) {
        I.s1th.post([&I] { if (I.s1.started()) { I.s1.flushPending(); I.s1pk.clear(); I.s1.takePackets(I.s1pk); I.deliver(); } });
        I.s1th.sync();
    }
    else if (I.mode == Impl::kS2) I.s2.drain();
    I.publish();
}

bool DvbsReceiver::telemetry(DvbsTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->tmu);
    if (p_->seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void DvbsReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    if (n == 0 || !ready()) return;
    if (I.resetReq.exchange(false) || I.restartReq.exchange(false)) {
        const uint64_t keep = I.seq;
        I.resetState();
        I.seq = keep;
    }
    // a NaN or infinite sample (a broken file) would stay in the AGC power of the symbol timer, whose loop then runs away and reads outside its
    // buffer: such samples count as silence
    for (size_t i = 0; i < n; i++) {
        if (std::isfinite(x[i].real() + x[i].imag())) continue;
        I.clean.assign(x, x + n);
        for (cf32& v : I.clean) if (!std::isfinite(v.real() + v.imag())) v = cf32(0, 0);
        x = I.clean.data();
        break;
    }
    I.inputCount += n;
    I.spec.push(x, n);
    if (I.mode == Impl::kSearch) {
        I.raw.insert(I.raw.end(), x, x + n);
        if (I.raw.size() > (size_t)kCyclicN) I.raw.erase(I.raw.begin(), I.raw.end() - kCyclicN);
        I.doSearch();
        if (I.mode != Impl::kSearch) std::vector<cf32>().swap(I.raw);
    }
    else I.runFront(x, n);
    I.cidFeed(x, n);
    if (I.inputCount >= I.reportAt) {
        I.reportAt = I.inputCount + (uint64_t)(0.25 * I.fs);
        if (I.mode != Impl::kSearch && I.spec.segments() >= 8) {
            I.spec.setPick(I.pickOn.load(), I.pickHz.load());
            I.specRes = I.spec.analyse();
            I.surveyUpdate();
            I.spec.display(I.psdDb, 512);
            I.spec.reset();
        }
        I.publish();
    }
}

} // namespace dect2
