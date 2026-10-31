// Automatic gain, parts 2 and 3: what is measured (ADC, noise floor, channel SNR, the mode's quality, intermodulation) and what each mode
// wants. See docs/AUTOGAIN.md.
#include <string>
#include "dect2/source/autogain.h"
#include <algorithm>
#include <cmath>

namespace dect2::ag {

namespace {
double lin10(double db) { return std::pow(10.0, db / 10.0); }
double db10(double x) { return 10.0 * std::log10(std::max(x, 1e-30)); }
}

// ------------------------------------------------------------------------------------------------------------------------------ ADC
BlockStats measureBlock(const std::complex<float>* iq, size_t n, float clipLevel) {
    BlockStats b;
    b.n = n;
    if (!iq || n == 0) return b;
    double p = 0, pk = 0;
    size_t clipped = 0;
    uint32_t hist[256] = {};
    for (size_t i = 0; i < n; i++) {
        const float re = std::fabs(iq[i].real()), im = std::fabs(iq[i].imag());
        p += (double)re * re + (double)im * im;
        const float m = std::max(re, im);
        if (m > pk) pk = m;
        if (m >= clipLevel) clipped++;
        hist[std::min(255, (int)(re * 256.f))]++;
        hist[std::min(255, (int)(im * 256.f))]++;
    }
    b.peak = std::min(1.0, pk);
    b.rmsDbfs = db10(p / (double)n);
    b.clipFraction = (double)clipped / (double)n;
    const double want = 0.999 * 2.0 * (double)n;
    double cum = 0;
    for (int k = 0; k < 256; k++) { cum += hist[k]; if (cum >= want) { b.p999 = (k + 1) / 256.0; break; } }
    if (b.p999 <= 0) b.p999 = b.peak;
    b.p999 = std::min(b.p999, b.peak > 0 ? b.peak : 1.0);
    b.crestDb = 20 * std::log10(std::max(b.p999, 1e-9)) - b.rmsDbfs;
    b.headroomDb = -20 * std::log10(std::max(b.p999, 1e-9));
    return b;
}

BlockStats fromSignalStats(const SignalStats& s) {
    BlockStats b;
    b.peak = s.peak;
    b.rmsDbfs = s.rmsDbfs;
    b.clipFraction = s.clipFraction;
    uint64_t tot = 0;
    for (uint32_t h : s.hist) tot += h;
    b.p999 = s.peak;
    if (tot > 100) {   // the 64-bin |I|/|Q| histogram over 0..1: the 99.9 % point; at low levels the bins are coarse, so it is limited by rms
        const double want = 0.999 * (double)tot;
        double cum = 0;
        for (int k = 0; k < 64; k++) { cum += s.hist[k]; if (cum >= want) { b.p999 = (k + 1) / 64.0; break; } }
        const double fromRms = std::pow(10.0, (s.rmsDbfs + 7.4) / 20.0);   // Gaussian 99.9 % of one component is 3.3 sigma: rms + 7.4 dB
        if (b.p999 <= 2.0 / 64.0) b.p999 = std::max(fromRms, (double)b.p999 * 0.5);
        b.p999 = std::min(b.p999, (double)std::max(s.peak, 1e-6f));
    } else {
        const double fromRms = std::pow(10.0, (s.rmsDbfs + 7.4) / 20.0);
        b.p999 = s.peak > 0 ? std::min((double)s.peak, std::max(fromRms, s.peak * 0.7)) : fromRms;
    }
    b.crestDb = 20 * std::log10(std::max(b.p999, 1e-9)) - b.rmsDbfs;
    b.headroomDb = -20 * std::log10(std::max(b.p999, 1e-9));
    return b;
}

// ------------------------------------------------------------------------------------------------------------------------------ spectrum
FloorEstimate estimateFloor(const std::vector<float>& dbfs, double rateHz, double chanBwHz, double offsetHz, int adcBits) {
    FloorEstimate e;
    const size_t n = dbfs.size();
    if (n < 64 || !(rateHz > 0)) return e;
    const double binHz = rateHz / (double)n;
    if (!(chanBwHz > 0)) chanBwHz = binHz * 8;
    const double half = 0.5 * chanBwHz;
    const bool narrowAtDc = chanBwHz < 12 * binHz && std::fabs(offsetHz) < chanBwHz;   // a channel of a few bins on the centre: its bins are the DC bins
    std::vector<float> outside;
    double inSum = 0, outSum = 0, all = 0, strongest = -300;
    size_t inN = 0;
    for (size_t i = 0; i < n; i++) {
        const float v = dbfs[i];
        if (!std::isfinite(v)) continue;
        const double f = ((double)i - (double)n / 2.0) * binHz;
        const double p = lin10(v);
        all += p;
        const double d = std::fabs(f - offsetHz);
        if (std::fabs(f) > 0.46 * rateHz) continue;  // the anti-alias roll-off at the edges
        if (std::fabs(f) < 2.5 * binHz && !(narrowAtDc && d <= 0.5 * chanBwHz)) continue;   // the DC spike (kept when a narrow channel sits on it)
        if (d <= 0.4 * chanBwHz || (narrowAtDc && d <= 0.5 * chanBwHz + binHz)) { inSum += p; inN++; }
        else if (d >= std::max(0.75 * chanBwHz, half + 3 * binHz)) { outside.push_back(v); outSum += p; strongest = std::max<double>(strongest, v); }
    }
    e.spanDbfs = db10(all / 1.5);
    if (inN) e.chanDbfs = db10(inSum / (double)inN);
    if (outside.size() >= 16) {
        std::sort(outside.begin(), outside.end());
        const size_t q = outside.size() / 4;   // the median of the quietest half
        e.floorDbfs = outside[q];
        e.oobDbfs = db10(outSum / 1.5);
        e.strongestDbfs = strongest;
        e.valid = true;
    } else if (inN >= 16) {   // the channel fills the capture: the quietest tenth of it as the floor (a pessimistic SNR)
        std::vector<float> in;
        for (size_t i = 0; i < n; i++) if (std::isfinite(dbfs[i])) in.push_back(dbfs[i]);
        std::sort(in.begin(), in.end());
        e.floorDbfs = in[in.size() / 10];
        e.valid = true;
    }
    if (e.valid) {
        e.snrDb = e.chanDbfs - e.floorDbfs;
        // the ADC's quantisation noise per bin: full-scale sine SNR 6.02 b + 1.76 dB spread over n bins (Hann: +1.76 dB)
        const double qPerBin = -(6.02 * adcBits + 1.76) - db10((double)n) + 1.76;
        e.adcMarginDb = e.floorDbfs - qPerBin;
    }
    return e;
}

ImdReport imdCompare(const std::vector<float>& lo, const std::vector<float>& hi, double dg) {
    ImdReport r;
    const size_t n = std::min(lo.size(), hi.size());
    if (n < 64 || !(dg > 0.5)) return r;
    // smooth over 3 bins (the spectra are noisy), find the floor of the higher one
    auto sm = [&](const std::vector<float>& v, size_t i) {
        const size_t a = i ? i - 1 : i, b = std::min(n - 1, i + 1);
        return (lin10(v[a]) + lin10(v[i]) + lin10(v[b])) / 3.0;
    };
    std::vector<float> s(hi.begin(), hi.begin() + (long)n);
    std::nth_element(s.begin(), s.begin() + (long)(n / 4), s.end());
    const double floorHi = s[n / 4];
    std::vector<float> s2(lo.begin(), lo.begin() + (long)n);
    std::nth_element(s2.begin(), s2.begin() + (long)(n / 4), s2.end());
    const double floorLo = s2[n / 4];
    const bool floorMoved = floorHi - floorLo >= dg - 1.5;
    // the power slope over the lines that were clear of the floor at the lower gain (the ADC's own noise does not follow the gain, so the
    // whole span's power would read as "compression" whenever the radio's noise is below it)
    double pLo = 0, pHi = 0;
    for (size_t i = 0; i < n; i++) {
        if (i < 3 || i + 3 >= n || (i > n / 2 - 3 && i < n / 2 + 3)) continue;   // the edges and the DC spike
        const double h = db10(sm(hi, i)), l = db10(sm(lo, i));
        if (l >= floorLo + 10) { pLo += lin10(lo[i]); pHi += lin10(hi[i]); }
        if (h < floorHi + 10) continue;   // only clear lines count
        const double rise = h - l;
        const double excess = rise - dg;
        // an n-th order product rises n dB per dB; a real signal 1 dB per dB. A line that rose at least twice the gain change and by 3 dB
        // more than it, or came out of the floor of the lower spectrum, is a product of the front end.
        // A line can only be judged where the lower spectrum showed it: one hidden in the ADC's own noise at the lower gain comes out of it
        // faster than the gain (unmasking, not a product). A line that came out of the floor counts only when that floor itself followed the gain.
        if (l < floorLo + 3) {
            if (floorMoved && h > floorHi + 13 && rise > dg + 6) { r.appeared++; r.bins.push_back((int)i); r.worstExcessDb = std::max(r.worstExcessDb, excess); }
        } else if (l >= floorLo + 6 && rise > 2.0 * dg && excess > 3.0) { r.spurs++; r.bins.push_back((int)i); r.worstExcessDb = std::max(r.worstExcessDb, excess); }
    }
    // neighbouring bins of one product count once
    int groups = 0;
    for (size_t k = 0; k < r.bins.size(); k++) if (k == 0 || r.bins[k] > r.bins[k - 1] + 3) groups++;
    r.spurs = std::min(r.spurs + r.appeared, groups);
    r.slopeDbPerDb = pLo > 0 ? (db10(pHi) - db10(pLo)) / dg : 1.0;
    r.compressed = pLo > 0 && r.slopeDbPerDb < 0.6;
    r.overload = r.spurs >= 1 || r.compressed;
    r.valid = true;
    return r;
}

Quality qualityFrom(const RxTelemetry& rx) {
    Quality q;
    switch (rx.standard) {
    case 2: q.locked = rx.atsc.fieldSync; if (q.locked && std::isfinite(rx.atsc.snrDb)) { q.snrValid = true; q.snrDb = rx.atsc.snrDb; } break;
    case 3: q.locked = rx.dab.sync; if (q.locked && std::isfinite(rx.dab.snrDb)) { q.snrValid = true; q.snrDb = rx.dab.snrDb; } break;
    case 6: q.locked = rx.fm.carrier && rx.fm.state >= 1; if (q.locked && std::isfinite(rx.fm.snrDb)) { q.snrValid = true; q.snrDb = rx.fm.snrDb; } break;
    default:
        q.locked = rx.dataValid;
        if (q.locked && std::isfinite(rx.dataSnrDb)) { q.snrValid = true; q.snrDb = rx.dataSnrDb; }
        if (rx.plpValid && rx.plpMerDb > 0 && std::isfinite(rx.plpMerDb)) { q.merValid = true; q.merDb = rx.plpMerDb; }
        if (rx.plpValid && rx.plpPreBer > 0 && std::isfinite(rx.plpPreBer)) { q.berValid = true; q.ber = rx.plpPreBer; }
        if (rx.blocksOk + rx.blocksBad > 0) q.fecOk = (double)rx.blocksOk / (double)(rx.blocksOk + rx.blocksBad);
    }
    return q;
}

// ------------------------------------------------------------------------------------------------------------------------------ targets
const char* metricName(Metric m) {
    switch (m) {
    case Metric::Mer: return "MER";
    case Metric::Snr: return "SNR";
    case Metric::Ber: return "BER";
    case Metric::Lock: return "lock";
    case Metric::ChannelSnr: return "channel SNR";
    case Metric::Sensitivity: return "sensitivity";
    case Metric::Headroom: return "headroom";
    default: return "auto";
    }
}
const char* modeKindName(ModeKind k) {
    switch (k) {
    case ModeKind::BroadcastTv: return "broadcast TV";
    case ModeKind::Ofdm: return "wideband OFDM";
    case ModeKind::FmBroadcast: return "FM broadcast";
    case ModeKind::NarrowVoice: return "narrowband voice";
    case ModeKind::NarrowData: return "narrowband data";
    case ModeKind::Pulsed: return "pulsed (ADS-B)";
    case ModeKind::Gnss: return "GNSS";
    case ModeKind::WideSpan: return "wide span";
    default: return "general";
    }
}

const char* bandIdFor(double f) {
    if (f <= 0) return "";
    if (f < 30e6) return "hf";
    if (f < 87.5e6) return "vhf-low";
    if (f < 108e6) return "fm";
    if (f < 174e6) return "vhf";
    if (f < 240e6) return "tv-vhf";
    if (f < 470e6) return "uhf";
    if (f < 700e6) return "tv-uhf";
    if (f < 1000e6) return "cell";
    if (f < 2300e6) return "lband";
    if (f < 2500e6) return "ism24";
    return "microwave";
}

// What the band changes: the strongest signals a band holds decide how much headroom and linearity the front end needs, the quietest bands how
// much sensitivity. Pulsed (ADS-B) and GNSS targets already know their band.
static void applyBandBias(ModeTarget& t, double f) {
    const std::string b = bandIdFor(f);
    if (b.empty() || t.kind == ModeKind::Pulsed || t.kind == ModeKind::Gnss) return;
    const bool narrow = t.kind == ModeKind::NarrowVoice || t.kind == ModeKind::NarrowData;
    t.bandName = b == "hf" ? "HF" : b == "fm" ? "FM broadcast" : b == "tv-vhf" || b == "tv-uhf" ? "TV broadcast" : b == "cell" ? "mobile" : b == "lband" ? "L-band" : b == "microwave" ? "microwave" : "";
    if (b == "hf") {   // the band's own noise is far above the radio's and shortwave stations are strong: less gain, linear stages
        t.headroomDb += 3; t.marginGoalDb = std::max(3.0, t.marginGoalDb - 4); t.policy = Policy::Linearity;
        t.bandNote = "HF: the band's own noise is well above the radio's, strong shortwave stations, so gain is kept low";
    } else if (b == "fm") {   // 100 dBuV stations: everything else in the band gets a linear front end
        if (t.kind != ModeKind::FmBroadcast) t.headroomDb += 3;
        t.marginGoalDb = std::max(4.0, t.marginGoalDb - 3); t.policy = Policy::Linearity;
        t.bandNote = "strong FM stations nearby, kept headroom";
    } else if (b == "tv-vhf" || b == "tv-uhf") {
        if (narrow) { t.headroomDb += 2; if (t.policy == Policy::Sensitivity) t.policy = Policy::Balanced; }
        t.bandNote = "strong TV transmitters in this band, kept headroom";
    } else if (b == "cell") {
        if (narrow) { t.headroomDb += 2; if (t.policy == Policy::Sensitivity) t.policy = Policy::Balanced; }
        t.bandNote = "mobile-phone band: strong nearby signals, kept headroom";
    } else if (b == "vhf-low" || b == "vhf") {
        t.headroomDb += 1;
    } else if (b == "lband" || b == "microwave") {   // quiet: the radio's own noise is what matters
        if (t.kind != ModeKind::WideSpan) {
            t.policy = Policy::Sensitivity;
            t.headroomDb = std::max(5.0, t.headroomDb - 2); t.marginGoalDb += 2;
            t.bandNote = "quiet band: gain chosen for sensitivity";
        }
    }
}

ModeTarget modeTargetFor(int std_, double bwHz, double rateHz, double freqHz) {
    ModeTarget t;
    auto in = [&](std::initializer_list<int> l) { for (int x : l) if (x == std_) return true; return false; };
    if (in({0, 1, 2, 3, 5, 6, 8, 9, 10, 42})) {   // DVB-T2/T, ATSC, ATSC 3.0, ISDB-T, DVB-S/S2, DTMB, analog TV, DVB-C
        t.kind = ModeKind::BroadcastTv; t.metric = Metric::Mer; t.policy = Policy::Balanced;
        t.headroomDb = 7; t.windowDb = 3; t.marginGoalDb = 8; t.floorRiseDb = 4; t.lockSnrDb = 12;
        t.why = "broadcast TV: the decoder's MER decides; OFDM peaks need about 10 dB over the rms, so the ADC keeps headroom";
    } else if (std_ == 4) {
        t.kind = ModeKind::Ofdm; t.metric = Metric::Snr; t.policy = Policy::Balanced;
        t.headroomDb = 7; t.marginGoalDb = 8; t.floorRiseDb = 4; t.lockSnrDb = 10;
        t.why = "DAB: the decoder's SNR, with headroom for the OFDM peaks";
    } else if (in({7, 23})) {
        t.kind = ModeKind::FmBroadcast; t.metric = Metric::Snr; t.policy = Policy::Linearity;
        t.headroomDb = 9; t.marginGoalDb = 5; t.floorRiseDb = 3; t.lockSnrDb = 20; t.maxStepDb = 4;
        t.why = "FM broadcast: many strong stations at once; keep the front end linear and the audio SNR high";
    } else if (in({13, 45})) {
        t.kind = ModeKind::Pulsed; t.metric = Metric::Sensitivity; t.policy = Policy::Sensitivity;
        t.headroomDb = 3; t.windowDb = 3; t.clipLimit = 5e-4; t.marginGoalDb = 12; t.floorRiseDb = 8; t.lockSnrDb = 1e9;   // never frozen: messages come and go
        t.why = "ADS-B / Mode S: the most sensitivity at 1090 MHz; the pulses may come close to full scale, but a strong FM or GSM transmitter must not overload the front end";
    } else if (std_ == 14) {
        t.kind = ModeKind::Gnss; t.metric = Metric::Sensitivity; t.policy = Policy::Sensitivity;
        t.headroomDb = 12; t.marginGoalDb = 10; t.floorRiseDb = 10; t.lockSnrDb = 1e9;
        t.why = "GNSS: the signals are below the noise; the noise floor must sit well above the ADC's own noise";
    } else if (in({44, 50, 34, 56, 71, 54})) {
        t.kind = ModeKind::WideSpan; t.metric = Metric::Headroom; t.policy = Policy::Linearity;
        t.headroomDb = 12; t.windowDb = 4; t.marginGoalDb = 6; t.floorRiseDb = 3; t.lockSnrDb = 1e9;
        t.why = "a wide span: everything in it reaches the ADC, so headroom first";
    } else if (in({11, 33, 35, 40, 47, 48, 52, 28, 57, 51})) {
        t.kind = ModeKind::NarrowVoice; t.metric = Metric::ChannelSnr; t.policy = Policy::Sensitivity;
        t.headroomDb = 14; t.windowDb = 4; t.marginGoalDb = 10; t.floorRiseDb = 6; t.lockSnrDb = 15;
        t.why = "narrowband voice: the SNR in the channel, not over the span; strong neighbours in the span must not overload the front end";
    } else if (bwHz >= 1.0e6) {
        t.kind = ModeKind::Ofdm; t.metric = Metric::Snr; t.policy = Policy::Balanced;
        t.headroomDb = 8; t.marginGoalDb = 8; t.floorRiseDb = 4; t.lockSnrDb = 10;
        t.why = "a wide channel: the decoder's SNR with ADC headroom";
    } else {
        t.kind = ModeKind::NarrowData; t.metric = Metric::ChannelSnr; t.policy = Policy::Sensitivity;
        t.headroomDb = 14; t.windowDb = 4; t.marginGoalDb = 10; t.floorRiseDb = 6; t.lockSnrDb = 10;
        t.why = "narrowband data: the SNR in the channel; weak signals want the noise floor above the ADC's own";
    }
    // a capture much wider than the channel holds more strong signals: keep more headroom
    if (rateHz > 0 && bwHz > 0 && rateHz / bwHz > 50 && t.kind != ModeKind::Pulsed) t.headroomDb += 2;
    // Auto v2 constraints per kind (the climb maximises the objective; headroom is only a floor): see docs/AUTOGAIN.md
    switch (t.kind) {
    case ModeKind::BroadcastTv: t.minHeadroomDb = 4; t.improveDb = 0.5; t.comfortDb = 6; break;
    case ModeKind::Ofdm: t.minHeadroomDb = 4; t.improveDb = 0.5; t.comfortDb = 6; break;
    case ModeKind::FmBroadcast: t.minHeadroomDb = 5; t.improveDb = 0.8; t.comfortDb = 8; break;
    case ModeKind::NarrowVoice: t.minHeadroomDb = 5; t.improveDb = 1.0; t.comfortDb = 6; break;
    case ModeKind::NarrowData: t.minHeadroomDb = 5; t.improveDb = 1.0; t.comfortDb = 6; break;
    case ModeKind::Pulsed: t.minHeadroomDb = 3; t.improveDb = 1.0; t.comfortDb = 1e9; t.usePeakHold = true; break;   // judged on the 2 s peak-hold: pulses are < 0.1 % of the samples
    case ModeKind::Gnss: t.minHeadroomDb = 6; t.improveDb = 1.0; t.comfortDb = 1e9; break;
    case ModeKind::WideSpan: t.minHeadroomDb = 6; t.improveDb = 1.5; t.comfortDb = 1e9; break;
    default: t.minHeadroomDb = 5; t.improveDb = 1.0; t.comfortDb = 6; break;
    }
    if (freqHz > 0) applyBandBias(t, freqHz);
    return t;
}

} // namespace dect2::ag
