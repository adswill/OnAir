#include "dect2/scanner.h"
#include "dect2/isdbt.h"
#include "dect2/bandwidth.h"
#include "dect2/dvbt.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

using Clock = std::chrono::steady_clock;

Scanner::Scanner() {}
Scanner::~Scanner() { stop(); }

std::vector<double> Scanner::channels(const ScanConfig& c) {
    std::vector<double> f;
    for (double x = c.startMHz; x <= c.stopMHz + 1e-6; x += c.stepMHz) f.push_back(x);
    return f;
}

ScanProgress Scanner::progress() const { std::lock_guard<std::mutex> lk(mu_); return prog_; }
std::vector<ScanResult> Scanner::results() const { std::lock_guard<std::mutex> lk(mu_); return results_; }

bool Scanner::check(const DeviceInfo& dev, const ScanConfig& cfg, std::string& err) {
    if (!dev.isRadio()) { err = "scanning needs a radio (the synthetic signal and recordings cannot be scanned)"; return false; }
    // a radio that cannot reach the sample rate a channel needs would only see part of it
    const double needMhz = cfg.atsc || cfg.isdbt ? 6.0 : cfg.bwMhz;
    if (dev.isGeneric() && dev.maxRateHz > 0 && dev.maxRateHz < needMhz * 1e6 * 1.15) {
        char b[200];
        snprintf(b, sizeof b, "%s reaches at most %.1f Msps, too low for a %.0f MHz channel (it needs about %.1f)", dev.name.c_str(), dev.maxRateHz / 1e6, needMhz, needMhz * 1.15);
        err = b;
        return false;
    }
    return true;
}

bool Scanner::start(const DeviceInfo& dev, const ScanConfig& cfg, std::string& err) {
    stop();
    if (!check(dev, cfg, err)) return false;
    cfg_ = cfg;
    dev_ = dev;
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        results_.clear();
        prog_ = ScanProgress();
        prog_.running = true;
        prog_.total = (int)channels(cfg).size();
        prog_.phase = "starting";
    }
    eng_ = std::make_unique<Engine>();
    th_ = std::thread([this] { run(); });
    return true;
}

void Scanner::stop() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
    eng_.reset();
    std::lock_guard<std::mutex> lk(mu_);
    prog_.running = false;
}

bool Scanner::sleepOrCancel(double s) {
    auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
    while (Clock::now() < end) {
        if (cancel_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !cancel_;
}

void Scanner::run() {
    Engine& e = *eng_;
    const auto freqs = channels(cfg_);
    TuneSettings tune = cfg_.tune;
    tune.bandwidthMhz = cfg_.bwMhz;
    tune.sampleRate = cfg_.bwMhz >= 7 || cfg_.autoBandwidth ? 10e6 : 8e6;
    tune.centerHz = freqs.empty() ? 0 : freqs[0] * 1e6;
    if (cfg_.atsc) { tune.bandwidthMhz = 6; tune.sampleRate = 8e6; e.setStandard(3); }   // ATSC: always 6 MHz, no bandwidth detection
    if (cfg_.isdbt) { tune.bandwidthMhz = 6; tune.sampleRate = 8e6; e.setStandard(6); }   // ISDB-T: 6 MHz channels as well
    // other radios than the HackRF report the rates and gains they can do
    if (dev_.isGeneric()) {
        if (dev_.maxRateHz > 0) tune.sampleRate = std::min(tune.sampleRate, dev_.maxRateHz);
        if (dev_.gainMaxDb > 0 && tune.gainDb > dev_.gainMaxDb) tune.gainDb = dev_.gainMaxDb;
    }
    FileOptions fo;
    if (!e.start(dev_, tune, fo)) {
        std::lock_guard<std::mutex> lk(mu_);
        prog_.running = false;
        prog_.phase = std::string("could not open ") + (dev_.kind == DeviceInfo::HackRF ? "the HackRF" : dev_.name.c_str());
        return;
    }
    auto setPhase = [&](const char* ph, int idx, double f) {
        std::lock_guard<std::mutex> lk(mu_);
        prog_.phase = ph; prog_.index = idx; prog_.currentMHz = f;
    };
    for (size_t i = 0; i < freqs.size() && !cancel_; i++) {
        const double f = freqs[i];
        ScanResult r;
        r.freqMHz = f; r.bwMhz = cfg_.bwMhz;
        setPhase("tuning", (int)i, f);
        tune.centerHz = f * 1e6;
        uint64_t rc = e.resetCount();
        if (i == 0) e.retuneReset(tune); else e.retuneReset(tune);
        for (int w = 0; w < 100 && e.resetCount() == rc && !cancel_; w++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!sleepOrCancel(0.15)) break;

        // ---- occupancy from the spectrum
        setPhase("measuring", (int)i, f);
        double inb = 0, floorSum = 0;
        int nIn = 0, nFl = 0;
        std::vector<float> floorBins;
        uint64_t seq = 0;
        int frames = 0;
        BandwidthDetector bwDet;
        auto t0 = Clock::now();
        SpectrumFrame sf;
        while (frames < 8 && std::chrono::duration<double>(Clock::now() - t0).count() < 1.5 && !cancel_) {
            if (!e.latestSpectrum(sf, seq)) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
            seq = sf.seq;
            if (frames++ < 2) continue; // settle
            const size_t n = sf.dbfs.size();
            const double fs = e.sampleRate();
            bwDet.add(sf.dbfs, fs / 1e6);
            for (size_t b = 0; b < n; b++) {
                double fhz = ((double)b / n - 0.5) * fs / 1e6;
                double a = std::fabs(fhz);
                if (a < cfg_.bwMhz * 0.5 * 0.9) { inb += std::pow(10.0, sf.dbfs[b] / 10.0); nIn++; }
                else if (a > cfg_.bwMhz * 0.5 * 1.12 && a < fs / 1e6 * 0.5 * 0.94) floorBins.push_back(sf.dbfs[b]);
            }
        }
        if (getenv("DECT2_SCANDEBUG")) fprintf(stderr, "[scan] %.1f frames %d nIn %d floor %zu rate %.0f\n", f, frames, nIn, floorBins.size(), e.sampleRate());
        if (cancel_) break;
        if (nIn > 0) r.levelDbfs = 10 * std::log10(inb / nIn);
        if (!floorBins.empty()) {
            std::sort(floorBins.begin(), floorBins.end());
            double fl = floorBins[floorBins.size() / 2];
            r.occupancyDb = r.levelDbfs - fl;
        }
        (void)floorSum; (void)nFl;
        r.occupied = r.occupancyDb >= cfg_.occupancyDb;
        // the width of the signal: occupied if a flat-topped signal is there, and decoded with that channel bandwidth
        if (cfg_.autoBandwidth && !cfg_.atsc && !cfg_.isdbt) {
            const BandwidthEstimate be = bwDet.estimate();
            if (be.valid) {
                r.occupied = true;
                r.bwMhz = be.bwMhz;
                r.occupancyDb = std::max(r.occupancyDb, be.snrDb);
                if (be.bwMhz != tune.bandwidthMhz) {
                    tune.bandwidthMhz = be.bwMhz;
                    e.setBandwidth(be.bwMhz);
                    uint64_t rc2 = e.resetCount();
                    for (int w = 0; w < 100 && e.resetCount() == rc2 && !cancel_; w++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } else if (r.occupied && be.noFloor == false) r.occupied = false;   // power but no signal shape: not a TV channel
        }
        if (!r.occupied) {
            r.note = "empty";
            std::lock_guard<std::mutex> lk(mu_);
            results_.push_back(r);
            prog_.index = (int)i + 1;
            continue;
        }

        // ---- try to lock DVB-T2
        setPhase(cfg_.isdbt ? "looking for ISDB-T" : "looking for DVB-T2 / DVB-T", (int)i, f);
        RxTelemetry t;
        uint64_t rxSeq = 0;
        auto tl = Clock::now();
        bool l1 = false, tlock = false, alock = false, ilock = false;
        while (std::chrono::duration<double>(Clock::now() - tl).count() < cfg_.lockTimeoutSec && !cancel_) {
            if (e.latestRx(t, rxSeq)) {
                rxSeq = t.seq;
                if (t.standard == 5) {
                    if (t.isdbt.tmccOk) {
                        ilock = true;
                        bool sync = false;
                        for (int k = 0; k < 3; k++) sync |= t.isdbt.layer[k].synced;
                        if (sync) break;
                        if (std::chrono::duration<double>(Clock::now() - tl).count() > cfg_.lockTimeoutSec - 0.8) break;
                    }
                } else if (t.standard == 2) {
                    if (t.atsc.fieldSync && t.atsc.eqTrained) { alock = true; if (t.atsc.tsOk) break; }
                } else if (t.standard == 1) {
                    if (t.dvbt.tpsOk) { tlock = true; if (t.dvbt.fecSync && t.dataValid) break; if (std::chrono::duration<double>(Clock::now() - tl).count() > cfg_.lockTimeoutSec - 0.8) break; }
                } else if (t.l1preOk && t.chValid) { l1 = true; if ((t.l1postOk && t.dataValid) || std::chrono::duration<double>(Clock::now() - tl).count() > 3.0) break; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (cancel_) break;
        if (!l1 && !tlock && !alock && !ilock) {
            r.note = cfg_.isdbt ? "signal present, no ISDB-T" : cfg_.atsc ? "signal present, no ATSC" : t.p1Count > 0 ? "T2 preamble found, L1 not decodable (weak?)" : "signal present, no DVB-T2";
            if (t.p1Count > 0 && t.p1.valid) {
                const FftMode* fm = fftModeFromS2(t.p1.s2field1);
                r.mode = std::string(s1Name(t.p1.s1)) + " " + (fm ? fm->name : "?");
            }
            std::lock_guard<std::mutex> lk(mu_);
            results_.push_back(r);
            prog_.index = (int)i + 1;
            continue;
        }
        r.t2 = true;
        r.l1post = t.l1postOk;
        if (ilock) {
            r.standard = "ISDB-T";
            char b[200];
            snprintf(b, sizeof b, "ISDB-T mode %d, GI %s, %d segments%s", t.isdbt.mode, isdbt::guardName(t.giIdx), t.isdbt.layer[0].segments + t.isdbt.layer[1].segments + t.isdbt.layer[2].segments, t.isdbt.partial ? " (with a one-segment layer)" : "");
            r.mode = b;
            std::string s;
            for (int k = 0; k < 3; k++) {
                const auto& L = t.isdbt.layer[k];
                if (!L.segments) continue;
                snprintf(b, sizeof b, "%s%c: %d x %s %s", s.empty() ? "" : ", ", 'A' + k, L.segments, isdbt::modName(L.mod), isdbt::rateName(L.rate));
                s += b;
            }
            r.plpInfo = s;
            r.snrDb = t.dataSnrDb;
        } else if (alock) {
            r.standard = "ATSC";
            r.mode = "8-VSB, 6 MHz";
            char b[120];
            snprintf(b, sizeof b, "field sync %d, SNR %.1f dB", t.atsc.fieldParity, t.atsc.snrDb);
            r.plpInfo = b;
            r.snrDb = (float)t.atsc.snrDb;
        } else if (tlock) {
            r.standard = "DVB-T";
            char b[160];
            snprintf(b, sizeof b, "DVB-T %s GI %s", t.fftN == 8192 ? "8K" : "2K", dvbt::guardName(t.giIdx));
            r.mode = b;
            snprintf(b, sizeof b, "%s %s%s", dvbt::modName(t.dvbt.mod), dvbt::rateName(t.dvbt.crHp), t.dvbt.hier ? " (hierarchical)" : "");
            r.plpInfo = b;
            r.cellId = t.dvbt.cellId;
            r.snrDb = t.dataSnrDb;
            if (t.dvbt.hier) t.unsupported.push_back("hierarchical modulation: only the high-priority stream is decoded");
        } else {
            const L1Pre& p = t.l1pre;
            const FftMode* fm = fftModeFromS2(p.s2 >> 1);
            char b[160];
            snprintf(b, sizeof b, "%s %s GI %s PP%d%s", s1Name(p.s1), fm ? fm->name : "?", guardName(p.guardInterval), p.pilotPattern + 1, p.bwtExt ? " ext" : "");
            r.mode = b;
            r.cellId = p.cellId; r.networkId = p.networkId;
        }
        if (!tlock && !alock && !ilock) {
        r.snrDb = t.dataValid ? t.dataSnrDb : t.p2SnrDb;
        if (t.plpMerDb > 0) r.snrDb = (float)t.plpMerDb;
        }
        if (!tlock && !alock && !ilock && t.l1postOk) {
            static const char* modN[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
            std::string s;
            for (size_t k = 0; k < t.l1post.plps.size(); k++) {
                const L1PlpConf& c = t.l1post.plps[k];
                if (c.type == 0 && t.l1post.plps.size() > 1) continue;
                char b[100];
                snprintf(b, sizeof b, "%s%s %s%s", k ? ", " : "", c.mod < 4 ? modN[c.mod] : "?", rateName(c.cod), c.rotation ? " rot" : "");
                s += b;
            }
            r.plpInfo = s + " (" + std::to_string(t.l1post.numPlp) + " PLP)";
        }
        // ---- service list
        if (cfg_.identifyServices) {
            setPhase("reading services", (int)i, f);
            auto ts0 = Clock::now();
            double firstNames = -1;
            TsSnapshot ts;
            while (std::chrono::duration<double>(Clock::now() - ts0).count() < cfg_.serviceTimeoutSec && !cancel_) {
                ts = e.tsSnapshot();
                int named = 0, total = 0;
                for (auto& sv : ts.services) { total++; if (!sv.name.empty()) named++; }
                if (named > 0 && firstNames < 0) firstNames = std::chrono::duration<double>(Clock::now() - ts0).count();
                if (total > 0 && named == total && !ts.networkName.empty()) break;
                if (firstNames >= 0 && std::chrono::duration<double>(Clock::now() - ts0).count() - firstNames > 3.0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            r.networkName = ts.networkName;
            for (auto& sv : ts.services) r.services.push_back((sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name) + " [" + sv.typeName() + "]");
            r.muxKbps = ts.muxKbps;
            e.latestRx(t, 0);
            if (t.standard == 2) r.snrDb = (float)t.atsc.snrDb;
            else if (t.standard == 1 || t.standard == 5) r.snrDb = t.dataSnrDb;
            else if (t.plpValid && t.plpMerDb > 0) r.snrDb = (float)t.plpMerDb; else if (t.dataValid) r.snrDb = t.dataSnrDb;
            uint64_t tot = t.blocksOk + t.blocksBad;
            r.fecGood = tot ? (double)t.blocksOk / tot : 0;
        }
        r.unsupported = t.unsupported;
        r.note = r.unsupported.empty() ? r.standard : r.standard + ", partly unsupported: " + r.unsupported[0];
        std::lock_guard<std::mutex> lk(mu_);
        results_.push_back(r);
        prog_.index = (int)i + 1;
    }
    e.stop();
    std::lock_guard<std::mutex> lk(mu_);
    prog_.running = false;
    prog_.phase = cancel_ ? "stopped" : "finished";
}

} // namespace dect2
