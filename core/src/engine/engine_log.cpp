// The engine's log: the line buffer behind log()/logSnapshot() and the per-standard event loggers that turn receiver telemetry
// changes (lock, parameters, ensembles, services) into log lines.
#include "dect2/engine/engine.h"
#include "engine_internal.h"
#include "dect2/source/converter.h"
#include "dect2/net/share.h"
#include "dect2/engine/device_claims.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/isdbt/isdbt.h"
#include "dect2/engine/modes.h"
#include "dect2/t2/t2rx.h"
#include <chrono>
#include "dect2/common/platform.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

void Engine::log(const std::string& line) {
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    char buf[32];
    snprintf(buf, sizeof buf, "%7.2fs ", t);
    std::lock_guard<std::mutex> lk(logMu_);
    log_.push_back(buf + line);
    if (log_.size() > 5000) log_.erase(log_.begin(), log_.begin() + 1000);
}

std::string Engine::loadProfile() const {
    char b[200];
    snprintf(b, sizeof b, "analysis thread: receiver %.2f s for %.2f s of signal processed (the spectrum thread used %.2f s)", tRx_, nSamp_ / std::max(1.0, rate_.load()), tSpec_);
    return b;
}

std::vector<std::string> Engine::logSnapshot(size_t& total) {
    std::lock_guard<std::mutex> lk(logMu_);
    total = log_.size();
    return log_;
}

void Engine::clearLog() {
    std::lock_guard<std::mutex> lk(logMu_);
    log_.clear();
}

void Engine::logDabEvents(const RxTelemetry& t) {
    const DabTelemetry& d = t.dab;
    if (d.state != logDState_) {
        if (d.state == 2) { char b[120]; snprintf(b, sizeof b, "DAB: frame sync, carrier offset %+.0f Hz, SNR %.1f dB", d.cfoHz, d.snrDb); log(b); }
        else if (logDState_ == 2) log("DAB: sync lost");
        logDState_ = d.state;
    }
    if (d.ensemble && !logDEns_) {
        char b[160];
        snprintf(b, sizeof b, "DAB: ensemble \"%s\", %d services", d.ensembleLabel.c_str(), d.services);
        log(b);
        logDEns_ = true;
    } else if (!d.ensemble) logDEns_ = false;
}

void Engine::logAtsc3Events(const Atsc3Telemetry& a) {
    const int st = a.locked ? 2 : a.bootstraps > 0 ? 1 : 0;
    if (st != logA3State_) {
        char b[200];
        if (st == 2) snprintf(b, sizeof b, "ATSC 3.0: frames decoding, carrier offset %.0f Hz", a.cfoHz);
        else if (st == 1) snprintf(b, sizeof b, "ATSC 3.0: bootstrap found, waiting for a frame that decodes");
        else snprintf(b, sizeof b, "ATSC 3.0: searching for a bootstrap");
        log(b);
        logA3State_ = st;
    }
    const bool svc = a.serviceReady;
    if (svc != logA3Svc_) {
        logA3Svc_ = svc;
        if (svc) { char b[160]; snprintf(b, sizeof b, "ATSC 3.0: signaling of service %d received", a.selected); log(b); }
    }
}

void Engine::logAtscEvents(const RxTelemetry& t) {
    const AtscTelemetry& a = t.atsc;
    const int level = a.tsOk ? 4 : a.fieldSync ? 3 : a.segSync ? 2 : a.pilot ? 1 : 0;
    if (level != logAState_) {
        static const char* nm[] = {"ATSC: searching for the pilot and segment sync", "ATSC: pilot locked", "ATSC: segment sync, symbol clock locked", "ATSC: field sync found", "ATSC: transport stream flowing"};
        log(nm[level]);
        if (level >= 3) { char b[120]; snprintf(b, sizeof b, "ATSC: carrier offset %+.0f Hz, symbol clock %+.1f ppm, SNR %.1f dB", a.cfoHz, a.sroPpm, a.snrDb); log(b); }
        logAState_ = level;
    }
}

void Engine::logIsdbtEvents(const RxTelemetry& t) {
    char b[240];
    if (t.fftN && (t.fftN != logIMode_ || t.giIdx != logIGi_)) {
        snprintf(b, sizeof b, "ISDB-T found: mode %d (%s carrier spacing), guard interval %s, CFO %+.1f Hz", t.isdbt.mode ? t.isdbt.mode : (t.fftN == 2048 ? 1 : t.fftN == 4096 ? 2 : 3),
                 t.fftN == 2048 ? "4 kHz" : t.fftN == 4096 ? "2 kHz" : "1 kHz", isdbt::guardName(t.giIdx), t.cfoHz);
        log(b);
        logIMode_ = t.fftN; logIGi_ = t.giIdx;
    }
    const int tm = t.isdbt.tmccOk ? 1 : 0;
    if (tm != logITmcc_) {
        if (tm) {
            std::string s;
            for (int i = 0; i < 3; i++) {
                const auto& L = t.isdbt.layer[i];
                if (!L.segments) continue;
                snprintf(b, sizeof b, "%s%c: %d segment%s %s %s", s.empty() ? "" : ", ", 'A' + i, L.segments, L.segments == 1 ? "" : "s", isdbt::modName(L.mod), isdbt::rateName(L.rate));
                s += b;
            }
            snprintf(b, sizeof b, "TMCC decoded: %s%s", s.c_str(), t.isdbt.partial ? " (with a one-segment layer)" : "");
            log(b);
        } else if (logITmcc_ == 1) log("TMCC lost");
        logITmcc_ = tm;
    }
    bool sync = false;
    for (int i = 0; i < 3; i++) sync |= t.isdbt.layer[i].synced;
    if (sync != logISync_) {
        log(sync ? "transport stream locked (Viterbi + Reed-Solomon)" : "transport stream sync lost");
        logISync_ = sync;
    }
}

void Engine::logDvbtEvents(const RxTelemetry& t) {
    char b[200];
    if (t.fftN && (t.fftN != logTMode_ || t.giIdx != logTGi_)) {
        snprintf(b, sizeof b, "DVB-T found: %s, guard interval %s, CFO %+.1f Hz", t.fftN == 8192 ? "8K" : "2K", dvbt::guardName(t.giIdx), t.cfoHz);
        log(b);
        logTMode_ = t.fftN; logTGi_ = t.giIdx;
    }
    const int tps = t.dvbt.tpsOk ? 1 : 0;
    if (tps != logTTps_) {
        if (tps) {
            snprintf(b, sizeof b, "TPS decoded: %s, code rate %s, cell id %d", dvbt::modName(t.dvbt.mod), dvbt::rateName(t.dvbt.crHp), t.dvbt.cellId);
            log(b);
        } else if (logTTps_ == 1) log("TPS lost");
        logTTps_ = tps;
    }
    if (t.dvbt.fecSync != logTFec_) {
        log(t.dvbt.fecSync ? "transport stream locked (Viterbi + Reed-Solomon)" : "transport stream sync lost");
        logTFec_ = t.dvbt.fecSync;
    }
    if (t.dvbt.fecDropped < logTFecDropped_) logTFecDropped_ = 0;   // a new receiver
    const auto nowT = std::chrono::steady_clock::now();
    if (t.dvbt.fecDropped > logTFecDropped_ && nowT - logTFecAt_ > std::chrono::seconds(5)) {   // at most every 5 s
        logTFecAt_ = nowT;
        snprintf(b, sizeof b, "DVB-T: this computer is too slow for the error correction (stages dvbt.demap, dvbt.viterbi, dvbt.rs): %llu symbols dropped",
                 (unsigned long long)(t.dvbt.fecDropped - logTFecDropped_));
        log(b);
        logTFecDropped_ = t.dvbt.fecDropped;
    }
}

void Engine::logRxEvents(const RxTelemetry& t) {
    char b[200];
    if (t.p1Count != logP1Count_ && t.p1Count > 0) {
        bool first = logP1Count_ == 0 || t.p1.s2field1 != logS2_ || t.p1.s1 != logS1_;
        if (first) {
            snprintf(b, sizeof b, "P1 found: %s, S2 FFT %s%s, CFO %+.1f Hz, conf %.2f", s1Name(t.p1.s1),
                     fftModeFromS2(t.p1.s2field1) ? fftModeFromS2(t.p1.s2field1)->name : "?", t.p1.mixed ? " (mixed)" : "",
                     t.p1.cfoHz, t.p1.conf);
            log(b);
        }
        logP1Count_ = t.p1Count;
        logS2_ = t.p1.s2field1;
        logS1_ = t.p1.s1;
    }
    if (t.giIdx != logGi_) {
        if (t.giIdx >= 0) {
            snprintf(b, sizeof b, "guard interval %s detected (margin %.2fx), %d symbols locked", guardName(t.giIdx), t.giMargin, (int)t.symbols);
            log(b);
        }
        logGi_ = t.giIdx;
    }
    if (t.state != logState_) {
        static const char* nm[] = {"searching for P1", "waiting for guard interval", "locked"};
        log(std::string("sync: ") + nm[t.state]);
        logState_ = t.state;
    }
    if (t.symbolsPerFrame != logFrameSyms_ && t.symbolsPerFrame > 0) {
        snprintf(b, sizeof b, "frame length %.2f ms = %d symbols after P1, SRO %+.1f ppm", t.frameMs, t.symbolsPerFrame, t.sroPpm);
        log(b);
        logFrameSyms_ = t.symbolsPerFrame;
    }
}

}   // namespace dect2
