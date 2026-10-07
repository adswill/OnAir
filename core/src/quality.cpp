#include "dect2/quality.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dect2 {

double requiredSnrDb(const PlpFec& f) {
    // rows: QPSK, 16-QAM, 64-QAM, 256-QAM; columns: code rate 1/2, 3/5, 2/3, 3/4, 4/5, 5/6
    static const double t[4][6] = {{1.0, 2.2, 3.1, 4.1, 4.7, 5.2},
                                   {6.0, 7.7, 8.9, 10.2, 11.0, 11.6},
                                   {9.2, 11.4, 13.1, 14.9, 15.9, 16.7},
                                   {12.1, 14.6, 17.0, 18.9, 20.3, 21.4}};
    const int m = std::max(0, std::min(3, f.mod));
    const int r = f.rate >= 0 && f.rate < 6 ? f.rate : 0;
    return t[m][r] + (f.rotation ? 0.0 : 0.0);
}

void QualityMeter::reset() { *this = QualityMeter(); }

// DVB-T: required C/N for quasi error free reception in a Gaussian channel (EN 300 744 table), by modulation and code rate
static double dvbtRequiredDb(int mod, int rate) {
    static const double t[3][5] = {{3.1, 4.9, 5.9, 6.9, 7.7}, {8.8, 11.1, 12.5, 13.5, 13.9}, {14.4, 16.5, 18.0, 19.3, 20.1}};
    return t[std::max(0, std::min(2, mod))][std::max(0, std::min(4, rate))];
}

void QualityMeter::update(const RxTelemetry& rx) {
    if (rx.standard == 2) {   // ATSC: the threshold of visibility is about 15.2 dB SNR (A/74)
        const AtscTelemetry& a = rx.atsc;
        if (!a.fieldSync) { rep_ = QualityReport(); rep_.label = "no lock"; return; }
        const uint64_t good = a.rsClean + a.rsCorrected, bad = a.rsFailed;
        if (lastOk_ != ~0ull && good >= lastOk_ && bad >= lastBad_ && (good - lastOk_ + bad - lastBad_) > 0) {
            hist_.push_back({good - lastOk_, bad - lastBad_});
            if (hist_.size() > 30) hist_.pop_front();
        }
        lastOk_ = good; lastBad_ = bad; lastFrames_ = 0;
        uint64_t ok = 0, ng = 0;
        for (auto& h : hist_) { ok += h.first; ng += h.second; }
        QualityReport r;
        r.valid = true;
        r.snrDb = a.snrDb;
        r.requiredDb = 15.2;
        r.marginDb = r.snrDb - r.requiredDb;
        r.fecOk = (ok + ng) ? (double)ok / (double)(ok + ng) : (a.tsOk ? 1.0 : 0.0);
        const double qs = std::max(0.0, std::min(100.0, 25.0 + r.marginDb * 12.5));
        const double q = qs * r.fecOk * r.fecOk;
        smooth_ = rep_.valid ? smooth_ + 0.25 * (q - smooth_) : q;
        r.percent = smooth_;
        r.label = r.percent >= 85 ? "excellent" : r.percent >= 65 ? "good" : r.percent >= 40 ? "marginal" : "poor";
        rep_ = r;
        return;
    }
    if (rx.standard >= 7) {   // the modes added after FM (DVB-S/S2, DTMB, ...): there is no table of required SNR, so the score is how much of the recent data decoded
        if (rx.state != 2 && !rx.dataValid) { rep_ = QualityReport(); rep_.label = "no lock"; return; }
        const uint64_t good = rx.blocksOk, bad = rx.blocksBad;
        if (lastOk_ != ~0ull && (good >= lastOk_ && bad >= lastBad_) && (good - lastOk_ + bad - lastBad_) > 0) {
            hist_.push_back({good - lastOk_, bad - lastBad_});
            if (hist_.size() > 40) hist_.pop_front();
        }
        lastOk_ = good; lastBad_ = bad; lastFrames_ = 0;
        uint64_t ok = 0, ng = 0;
        for (auto& h : hist_) { ok += h.first; ng += h.second; }
        QualityReport r;
        r.valid = true;
        r.snrDb = rx.dataSnrDb;
        r.fecOk = (ok + ng) ? (double)ok / (double)(ok + ng) : (rx.dataValid ? 1.0 : 0.0);
        const double q = 100.0 * r.fecOk * r.fecOk * (rx.dataValid ? 1.0 : 0.5);
        smooth_ = rep_.valid ? smooth_ + 0.25 * (q - smooth_) : q;
        r.percent = smooth_;
        r.label = r.percent >= 85 ? "excellent" : r.percent >= 65 ? "good" : r.percent >= 40 ? "marginal" : "poor";
        rep_ = r;
        return;
    }
    if (rx.standard == 5) {   // ISDB-T: the layer that carries most of the data sets the requirement
        if (!rx.isdbt.tmccOk) { if (rx.state != 2) { rep_ = QualityReport(); rep_.label = "no lock"; } return; }
        const uint64_t good = rx.blocksOk, bad = rx.blocksBad;
        if (lastOk_ != ~0ull && (good >= lastOk_ && bad >= lastBad_) && (good - lastOk_ + bad - lastBad_) > 0) {
            hist_.push_back({good - lastOk_, bad - lastBad_});
            if (hist_.size() > 40) hist_.pop_front();
        }
        lastOk_ = good; lastBad_ = bad; lastFrames_ = 0;
        uint64_t ok = 0, ng = 0;
        for (auto& h : hist_) { ok += h.first; ng += h.second; }
        double need = 0, bestRate = -1;
        for (int i = 0; i < 3; i++) {
            const auto& L = rx.isdbt.layer[i];
            if (!L.segments) continue;
            const double bits = (double)L.segments * (L.mod == 3 ? 6 : L.mod == 2 ? 4 : 2) * (L.rate == 0 ? 0.5 : L.rate == 1 ? 2.0 / 3 : L.rate == 2 ? 0.75 : L.rate == 3 ? 5.0 / 6 : 7.0 / 8);
            if (bits > bestRate) {
                bestRate = bits;
                need = L.mod == 0 ? dvbtRequiredDb(0, L.rate) + 3.0 : dvbtRequiredDb(L.mod - 1, L.rate);   // DQPSK: about 3 dB more than QPSK
            }
        }
        QualityReport r;
        r.valid = true;
        r.snrDb = rx.dataSnrDb;
        r.requiredDb = need;
        r.marginDb = r.snrDb - r.requiredDb;
        bool sync = false;
        for (int i = 0; i < 3; i++) sync |= rx.isdbt.layer[i].synced;
        r.fecOk = (ok + ng) ? (double)ok / (double)(ok + ng) : (sync ? 1.0 : 0.0);
        const double qs = std::max(0.0, std::min(100.0, 25.0 + r.marginDb * 12.5));
        const double qq = qs * (sync ? r.fecOk * r.fecOk : 0.5);
        smooth_ = rep_.valid ? smooth_ + 0.25 * (qq - smooth_) : qq;
        r.percent = smooth_;
        r.label = r.percent >= 85 ? "excellent" : r.percent >= 65 ? "good" : r.percent >= 40 ? "marginal" : "poor";
        rep_ = r;
        return;
    }
    if (rx.standard == 1) {
        if (!rx.dvbt.tpsOk) { if (rx.state != 2) { rep_ = QualityReport(); rep_.label = "no lock"; } return; }
        const uint64_t good = rx.dvbt.rsClean + rx.dvbt.rsCorrected, bad = rx.dvbt.rsFailed;
        if (lastOk_ != ~0ull && (good >= lastOk_ && bad >= lastBad_) && (good - lastOk_ + bad - lastBad_) > 0) {
            hist_.push_back({good - lastOk_, bad - lastBad_});
            if (hist_.size() > 40) hist_.pop_front();
        }
        lastOk_ = good; lastBad_ = bad; lastFrames_ = 0;
        uint64_t ok = 0, ng = 0;
        for (auto& h : hist_) { ok += h.first; ng += h.second; }
        QualityReport r;
        r.valid = true;
        r.snrDb = rx.dataSnrDb;
        r.requiredDb = dvbtRequiredDb(rx.dvbt.mod, rx.dvbt.crHp);
        r.marginDb = r.snrDb - r.requiredDb;
        r.fecOk = (ok + ng) ? (double)ok / (double)(ok + ng) : (rx.dvbt.fecSync ? 1.0 : 0.0);
        const double qs = std::max(0.0, std::min(100.0, 25.0 + r.marginDb * 12.5));
        const double q = qs * (rx.dvbt.fecSync ? r.fecOk * r.fecOk : 0.5);
        smooth_ = rep_.valid ? smooth_ + 0.25 * (q - smooth_) : q;
        r.percent = smooth_;
        r.label = r.percent >= 85 ? "excellent" : r.percent >= 65 ? "good" : r.percent >= 40 ? "marginal" : "poor";
        rep_ = r;
        return;
    }
    if (!rx.dataValid) { if (rx.state != 2) { rep_ = QualityReport(); rep_.label = "no lock"; } return; }
    if (rx.dataFrames != lastFrames_) {
        if (lastFrames_ != ~0ull && rx.blocksOk + rx.blocksBad >= lastOk_ + lastBad_) {
            hist_.push_back({rx.blocksOk - lastOk_, rx.blocksBad - lastBad_});
            if (hist_.size() > 12) hist_.pop_front();
        }
        lastFrames_ = rx.dataFrames; lastOk_ = rx.blocksOk; lastBad_ = rx.blocksBad;
    }
    uint64_t ok = 0, bad = 0;
    for (auto& h : hist_) { ok += h.first; bad += h.second; }
    QualityReport r;
    r.valid = true;
    r.snrDb = rx.dataSnrDb;
    r.requiredDb = rx.plpValid ? requiredSnrDb(rx.plpFec) : 8.0;
    r.marginDb = r.snrDb - r.requiredDb;
    r.fecOk = (ok + bad) ? (double)ok / (double)(ok + bad) : (rx.plpValid ? 0.0 : 1.0);
    // 0 dB of margin is the edge of reception (25%); +6 dB or more is comfortable (100%)
    double qs = std::max(0.0, std::min(100.0, 25.0 + r.marginDb * 12.5));
    // The pilot SNR only sees the noise; echoes, fades and interference that the equaliser cannot remove show up in the bit errors the
    // LDPC decoder has to repair. Close to a pre-LDPC error rate of 8 % the decoder is at its cliff however good the SNR reads, so that
    // headroom limits the score as well (a clean signal sits around 1 to 2 %).
    if (rx.plpValid && rx.plpPreBer > 0) qs = std::min(qs, std::max(0.0, std::min(100.0, (0.085 - rx.plpPreBer) / 0.06 * 100.0)));
    double q = qs * (rx.plpValid && !hist_.empty() ? r.fecOk * r.fecOk : 1.0);
    smooth_ = rep_.valid ? smooth_ + 0.25 * (q - smooth_) : q;
    r.percent = smooth_;
    r.label = r.percent >= 85 ? "excellent" : r.percent >= 65 ? "good" : r.percent >= 40 ? "marginal" : "poor";
    rep_ = r;
}

} // namespace dect2
