// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/thread_name.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/engine/scanner.h"
#include "dect2/power/power.h"
#include "dect2/isdbt/isdbt.h"
#include "dect2/dsp/bandwidth.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/source/autogain.h"
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
void Scanner::setResults(std::vector<ScanResult> r) {
    std::lock_guard<std::mutex> lk(mu_);
    if (prog_.running) return;
    results_ = std::move(r);
}

bool Scanner::check(const DeviceInfo& dev, const ScanConfig& cfg, std::string& err) {
    if (!dev.isRadio() && !(cfg.testTune && dev.kind == DeviceInfo::Synthetic)) { err = "scanning needs a radio (the synthetic signal and recordings cannot be scanned)"; return false; }
    // a range that is empty or silly would tune the radio to 0 Hz, or step through thousands of channels
    if (!(cfg.startMHz > 0) || !(cfg.stopMHz >= cfg.startMHz)) { err = "the scan range is empty: the start frequency must be above 0 and not above the stop frequency"; return false; }
    if (!(cfg.stepMHz >= 0.1) || !(cfg.bwMhz >= 1.0 && cfg.bwMhz <= 10.0)) { err = "the step must be at least 0.1 MHz and the channel bandwidth between 1 and 10 MHz"; return false; }
    if ((cfg.stopMHz - cfg.startMHz) / cfg.stepMHz > 2000) { err = "the scan range has more than 2000 channels: use a larger step"; return false; }
    // a radio that cannot reach the sample rate a channel needs would only see part of it
    const double needMhz = cfg.atsc || cfg.isdbt || cfg.atsc3 ? 6.0 : cfg.dtmb ? (cfg.bwMhz < 7 ? 6.0 : 8.0) : cfg.bwMhz;
    if (dev.isGeneric() && dev.maxRateHz > 0 && dev.maxRateHz < needMhz * 1e6 * 1.15) {
        char b[200];
        snprintf(b, sizeof b, "%s reaches at most %.1f Msps, too low for a %.0f MHz channel (it needs about %.1f)", dev.name.c_str(), dev.maxRateHz / 1e6, needMhz, needMhz * 1.15);
        err = b;
        return false;
    }
    // no channel is left out for the radio's specified range: every channel is asked of the driver, and one it refuses is noted in its result
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
    // an exception that leaves a thread ends the whole program: the scan fails instead, with the reason in the progress line
    th_ = dect2::namedThread("onair-scan", [this] {
        std::string why;
        try { run(); return; }
        catch (const std::exception& ex) { why = ex.what(); }
        catch (...) { why = "unknown error"; }
        try { if (eng_) eng_->stop(); } catch (...) {}
        std::lock_guard<std::mutex> lk(mu_);
        prog_.running = false;
        prog_.phase = "scan failed: " + why;
    });
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
    // Save power when hidden: the scan waits (the dwell restarts) until the window is shown again or the scan is cancelled
    while (power::pauseWhenHidden() && !cancel_) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
    while (Clock::now() < end) {
        if (cancel_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(power::lowPower() ? 25 : 10));
    }
    return !cancel_;
}

void Scanner::run() {
    Engine& e = *eng_;
    const auto freqs = channels(cfg_);
    TuneSettings tune = cfg_.tune;
    tune.bandwidthMhz = cfg_.bwMhz;
    tune.sampleRate = cfg_.bwMhz >= 7 || cfg_.autoBandwidth ? 10e6 : 8e6;
    tune.basebandFilterHz = 0;   // automatic: the receiver's tune may still carry the narrow filter of FM, DAB or a narrow-band mode
    tune.centerHz = freqs.empty() ? 0 : freqs[0] * 1e6;
    if (cfg_.atsc) { tune.bandwidthMhz = 6; tune.sampleRate = 8e6; e.setStandard(3); }   // ATSC: always 6 MHz, no bandwidth detection
    if (cfg_.isdbt) { tune.bandwidthMhz = 6; tune.sampleRate = 8e6; e.setStandard(6); }   // ISDB-T: 6 MHz channels as well
    if (cfg_.atsc3) { tune.bandwidthMhz = 6; tune.sampleRate = 8e6; e.setStandard(5); }   // ATSC 3.0 needs at least 6.144 Msps; 8 leaves room for the channel edges
    if (cfg_.dtmb) { tune.bandwidthMhz = cfg_.bwMhz < 7 ? 6 : 8; tune.sampleRate = 10e6; e.setStandard(9); }   // DTMB: 8 MHz channels (6 MHz in Cuba), no bandwidth detection
    // the width of the channels: the 6 MHz modes and DTMB have their own, whatever bwMhz the scan tab was left on
    const double chBw = cfg_.atsc || cfg_.isdbt || cfg_.atsc3 ? 6.0 : cfg_.dtmb ? (cfg_.bwMhz < 7 ? 6.0 : 8.0) : cfg_.bwMhz;
    if (cfg_.testTune) cfg_.testTune(tune.centerHz / 1e6, tune);
    // other radios than the HackRF report the rates and gains they can do
    if (dev_.isGeneric()) {
        // DVB-T/T2 on a PlutoSDR: its native rate; on its USB cable above what the cable carries when the channel needs it (linkRateFor)
        const int sm0 = cfg_.atsc ? 3 : cfg_.isdbt ? 6 : cfg_.atsc3 ? 5 : cfg_.dtmb ? 9 : 0;
        if (dev_.kind == DeviceInfo::Native && dev_.board == "pluto" && sm0 == 0 && !cfg_.autoBandwidth && chBw >= 5) tune.sampleRate = dvbNativeRate(chBw);
        tune.sampleRate = linkRateFor(dev_, tune, tune.sampleRate, minSampleRateFor(sm0, cfg_.autoBandwidth && sm0 == 0 ? 8.0 : chBw));
        if (dev_.gainMaxDb > 0 && tune.gainDb > dev_.gainMaxDb) tune.gainDb = dev_.gainMaxDb;
        if (dev_.gainMaxDb > dev_.gainMinDb && tune.gainDb < dev_.gainMinDb) tune.gainDb = dev_.gainMinDb;
    }
    FileOptions fo;
    const std::string radioName = dev_.kind == DeviceInfo::HackRF ? "the HackRF" : dev_.name;
    // whether the radio sends samples: a spectrum frame other than the one there was (a reopened source may count from 0 again) comes
    // within `sec` seconds
    auto delivering = [&](double sec) {
        uint64_t s0 = 0;
        { SpectrumFrame z; if (e.latestSpectrum(z, 0)) s0 = z.seq; }
        const auto t = Clock::now();
        while (!cancel_ && std::chrono::duration<double>(Clock::now() - t).count() < sec) {
            SpectrumFrame z;
            if (e.latestSpectrum(z, 0) && z.seq != s0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    // The radio was probably in use by the receiver a moment ago: the system may need a short while to let go of it, so a failed open is
    // tried again. An open that succeeds is not enough either: a radio opened the moment another handle let go of it can start a stream
    // that never delivers (the first scan after the receiver ran then went through every channel without seeing anything), so the scan
    // starts only once samples come, and a silent radio is closed and opened again.
    bool silent = false;
    auto openRadio = [&](const TuneSettings& t) {
        silent = false;
        for (int attempt = 0; attempt < 3 && !cancel_; attempt++) {
            if (attempt) { { std::lock_guard<std::mutex> lk(mu_); prog_.phase = "opening the radio again"; } if (!sleepOrCancel(attempt * 0.5)) break; }
            if (!e.start(dev_, t, fo)) continue;
            if (delivering(dev_.isRadio() ? 5.0 : 15.0)) return true;   // the test signal of a slow mode on a busy machine may take longer
            if (cancel_) break;
            silent = true;
            e.log("scan: " + radioName + " opened but sends no samples: opening it again");
            e.stop();
        }
        return false;
    };
    auto openFailed = [&]() {
        std::string why;   // the engine logged what the radio driver said
        size_t total = 0;
        for (const auto& l : e.logSnapshot(total)) { const size_t p = l.find("source start failed: "); if (p != std::string::npos) why = l.substr(p + 21); }
        e.stop();
        std::lock_guard<std::mutex> lk(mu_);
        prog_.running = false;
        prog_.phase = cancel_ ? "stopped" : silent ? radioName + " sends no samples (unplugged, or in use by another program?)"
                                                   : "could not open " + radioName + (why.empty() ? "" : ": " + why);
    };
    if (!openRadio(tune)) { openFailed(); return; }
    {   // the rate the radio really runs at (an Airspy Mini lists 10 Msps and may give 6, a SoapySDR radio is only known once opened):
        // too slow for the channels, every one would read "signal present, no DVB-T2" - say why instead
        const int sm = cfg_.atsc ? 3 : cfg_.isdbt ? 6 : cfg_.atsc3 ? 5 : cfg_.dtmb ? 9 : 0;
        const double need = minSampleRateFor(sm, chBw);
        if (need > 0 && e.sampleRate() < need - 1) {
            char b[240];
            snprintf(b, sizeof b, "%s runs at %.2f Msps: too low for %g MHz channels (at least %.2f Msps is needed)", dev_.kind == DeviceInfo::HackRF ? "the HackRF" : dev_.name.c_str(), e.sampleRate() / 1e6, chBw, need / 1e6);
            e.stop();
            std::lock_guard<std::mutex> lk(mu_);
            prog_.running = false;
            prog_.phase = b;
            return;
        }
    }
    auto setPhase = [&](const char* ph, int idx, double f) {
        std::lock_guard<std::mutex> lk(mu_);
        prog_.phase = ph; prog_.index = idx; prog_.currentMHz = f;
    };
    int silentRetries = 0;   // times the radio was opened again on the current channel because it sent nothing
    for (size_t i = 0; i < freqs.size() && !cancel_; i++) {
        const double f = freqs[i];
        ScanResult r;
        r.freqMHz = f; r.bwMhz = chBw;
        setPhase("tuning", (int)i, f);
        tune.centerHz = f * 1e6;
        if (cfg_.testTune) cfg_.testTune(f, tune);
        uint64_t rc = e.resetCount();
        if (!e.retuneReset(tune)) {   // the radio stayed where it was: measuring now would file the previous channel under this frequency
            std::string why;
            size_t total = 0;
            for (const auto& l : e.logSnapshot(total)) { const size_t p = l.find("retune failed: "); if (p != std::string::npos) why = l.substr(p + 15); }
            r.note = "could not tune" + (why.empty() ? std::string() : ": " + why);
            std::lock_guard<std::mutex> lk(mu_);
            results_.push_back(r);
            prog_.index = (int)i + 1;
            continue;
        }
        for (int w = 0; w < 100 && e.resetCount() == rc && !cancel_; w++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!sleepOrCancel(0.15)) break;
        // what the receivers reported before this channel is old: only newer reports count
        uint64_t a3base = 0, rxBase = 0;
        { Atsc3Telemetry z; if (e.atsc3Telemetry(z)) a3base = z.seq; RxTelemetry zr; if (e.latestRx(zr, 0)) rxBase = zr.seq; }

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
        // 1.5 s once the spectrum flows; up to 5 s for the first three frames (two settle, one measured) after the retune: a busy machine
        // making the ATSC 3.0 test signal needs seconds for them; a radio delivers them within milliseconds, so it does not wait longer
        while (frames < 8 && std::chrono::duration<double>(Clock::now() - t0).count() < (frames < 3 ? 5.0 : 1.5) && !cancel_) {
            if (!e.latestSpectrum(sf, seq)) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
            if (frames == 0) t0 = Clock::now();   // the 1.5 s count from the first frame, not from the retune
            seq = sf.seq;
            if (frames++ < 2) continue; // settle
            const size_t n = sf.dbfs.size();
            const double fs = e.sampleRate();
            bwDet.add(sf.dbfs, fs / 1e6);
            for (size_t b = 0; b < n; b++) {
                double fhz = ((double)b / n - 0.5) * fs / 1e6;
                double a = std::fabs(fhz);
                if (a < chBw * 0.5 * 0.9) { inb += std::pow(10.0, sf.dbfs[b] / 10.0); nIn++; }
                else if (a > chBw * 0.5 * 1.12 && a < fs / 1e6 * 0.5 * 0.94) floorBins.push_back(sf.dbfs[b]);
            }
        }
        if (getenv("DECT2_SCANDEBUG")) fprintf(stderr, "[scan] %.1f frames %d nIn %d floor %zu rate %.0f\n", f, frames, nIn, floorBins.size(), e.sampleRate());
        if (cancel_) break;
        if (frames == 0) {   // not one spectrum in 5 s: the radio went silent, and every channel from here on would read "empty"
            if (silentRetries++ < 2) {
                e.log("scan: " + radioName + " stopped sending samples: opening it again");
                { std::lock_guard<std::mutex> lk(mu_); prog_.phase = "the radio sends nothing: opening it again"; }
                if (openRadio(tune)) { i--; continue; }   // open again on this channel, and measure it again
            }
            if (!cancel_) { silent = true; openFailed(); return; }
            break;
        }
        silentRetries = 0;
        if (nIn > 0) r.levelDbfs = 10 * std::log10(inb / nIn);
        if (!floorBins.empty()) {
            std::sort(floorBins.begin(), floorBins.end());
            double fl = floorBins[floorBins.size() / 2];
            r.occupancyDb = r.levelDbfs - fl;
        }
        (void)floorSum; (void)nFl;
        r.occupied = r.occupancyDb >= cfg_.occupancyDb;
        // the width of the signal: occupied if a flat-topped signal is there, and decoded with that channel bandwidth
        if (cfg_.autoBandwidth && !cfg_.atsc && !cfg_.isdbt && !cfg_.atsc3 && !cfg_.dtmb) {
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

        // ---- the gain for this channel: a quick "Optimise once" (faster settle, shorter measurements; the amp stays as it is)
        if (cfg_.autoGain && !cfg_.testTune) {
            setPhase("optimising the gain", (int)i, f);
            ag::AgSettings as;
            as.loopSpeed = 3; as.settleSec = 0.6; as.measureSec = 0.5;
            const int stdm = cfg_.atsc ? 3 : cfg_.isdbt ? 6 : cfg_.atsc3 ? 5 : cfg_.dtmb ? 9 : 0;
            const ag::AgRunResult gr = ag::autogainRun(e, tune, dev_, stdm, ag::AgMode::Once, as, cfg_.autoGainSec, &cancel_);
            r.gainDb = gr.plan.totalDb;
            char fb[32];
            snprintf(fb, sizeof fb, "%.1f", f);
            e.log(std::string("scan: ") + fb + " MHz, gain " + gr.text);
            if (cancel_) break;
        }

        // ---- try to lock DVB-T2
        setPhase(cfg_.isdbt ? "looking for ISDB-T" : cfg_.atsc3 ? "looking for ATSC 3.0" : cfg_.dtmb ? "looking for DTMB" : "looking for DVB-T2 / DVB-T", (int)i, f);
        RxTelemetry t;
        Atsc3Telemetry at;
        uint64_t rxSeq = cfg_.dtmb ? rxBase : 0;
        auto tl = Clock::now();
        bool l1 = false, tlock = false, alock = false, ilock = false, a3lock = false, dlock = false;
        while (std::chrono::duration<double>(Clock::now() - tl).count() < cfg_.lockTimeoutSec && !cancel_) {
            if (cfg_.atsc3) {
                // frames are decoded, so the bootstrap was found and L1 decoded
                Atsc3Telemetry a;
                if (e.atsc3Telemetry(a) && a.seq > a3base && a.locked && a.frame.valid) { at = a; a3lock = true; break; }
            } else if (e.latestRx(t, rxSeq)) {
                rxSeq = t.seq;
                if (t.standard == 8) {
                    if (t.dtmb.header >= 0 && t.dtmb.siOk) {
                        dlock = true;
                        if (t.dtmb.tsLock) break;
                        if (std::chrono::duration<double>(Clock::now() - tl).count() > cfg_.lockTimeoutSec - 0.8) break;
                    }
                } else if (t.standard == 5) {
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
        if (!l1 && !tlock && !alock && !ilock && !a3lock && !dlock) {
            r.note = cfg_.atsc3 ? "signal present, no ATSC 3.0" : cfg_.dtmb ? (t.dtmb.header >= 0 ? "DTMB frames found, system information not decodable (weak?)" : "signal present, no DTMB") : cfg_.isdbt ? "signal present, no ISDB-T" : cfg_.atsc ? "signal present, no ATSC" : t.p1Count > 0 ? "T2 preamble found, L1 not decodable (weak?)" : "signal present, no DVB-T2";
            // the standard is there but too weak to decode (a T2 preamble, DTMB frame headers): still listed, unlike a signal of another kind
            r.seen = cfg_.dtmb ? t.dtmb.header >= 0 : !cfg_.atsc3 && !cfg_.isdbt && !cfg_.atsc && t.p1Count > 0 && t.p1.valid;
            if (cfg_.dtmb) r.standard = "DTMB";
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
        r.seen = true;
        r.l1post = t.l1postOk;
        const bool other = !tlock && !alock && !ilock && !a3lock && !dlock;   // a DVB-T2 lock: the others have their own fields
        if (a3lock) {
            r.standard = "ATSC 3.0";
            const auto& fi = at.frame;
            char b[200];
            snprintf(b, sizeof b, "ATSC 3.0, %dK FFT, guard %d samples, pilots Dx%d Dy%d", fi.fftSize / 1024, fi.guard, fi.spDx, fi.spDy);
            r.mode = b;
            if (!fi.plps.empty()) {
                const auto& p = fi.plps[0];
                static const char* modN[] = {"", "", "QPSK", "", "16QAM", "", "64QAM", "", "256QAM", "", "1024QAM", "", "4096QAM"};
                snprintf(b, sizeof b, "%s %d/15 (%zu PLP)", p.bitsPerCell >= 2 && p.bitsPerCell <= 12 && modN[p.bitsPerCell][0] ? modN[p.bitsPerCell] : "?", p.rate15, fi.plps.size());
                r.plpInfo = b;
            }
            // the receiver reports no signal-to-noise ratio for ATSC 3.0: snrDb stays 0
        } else if (dlock) {
            r.standard = "DTMB";
            static const char* hdr[3] = {"PN420", "PN595", "PN945"};
            static const char* map[5] = {"4QAM-NR", "4QAM", "16QAM", "32QAM", "64QAM"};
            static const char* rt[3] = {"0.4", "0.6", "0.8"};
            const DtmbTelemetry& d = t.dtmb;
            char b[200];
            snprintf(b, sizeof b, "DTMB %s, %s", d.carriers == 1 ? "single carrier" : d.carriers ? "multi-carrier" : "carrier mode unknown", hdr[d.header]);
            r.mode = b;
            snprintf(b, sizeof b, "%s %s, interleaver %s", d.mapping >= 0 && d.mapping < 5 ? map[d.mapping] : "?", d.rate >= 0 && d.rate < 3 ? rt[d.rate] : "?", d.interleaver == 1 ? "240" : d.interleaver == 2 ? "720" : "?");
            r.plpInfo = b;
            r.snrDb = d.snrPnDb;
        } else if (ilock) {
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
            snprintf(b, sizeof b, "DVB-T %s GI %s", t.fftN == 8192 ? "8K" : t.fftN == 4096 ? "4K" : "2K", dvbt::guardName(t.giIdx));
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
        if (other) {
        r.snrDb = t.dataValid ? t.dataSnrDb : t.p2SnrDb;
        if (t.plpMerDb > 0) r.snrDb = (float)t.plpMerDb;
        }
        if (other && t.l1postOk) {
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
            if (a3lock) {}   // no SNR to read
            else if (t.standard == 8) r.snrDb = t.dtmb.snrPnDb;
            else if (t.standard == 2) r.snrDb = (float)t.atsc.snrDb;
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
