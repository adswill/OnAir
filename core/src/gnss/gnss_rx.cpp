// GNSS receiver: orchestrates the bands, the acquisition, the tracking channels, the navigation data base and the position solution.
// This file: the public GnssReceiver API, the block loop and the tuning table. The state and the bigger member functions are in
// gnss_rx_impl.h and gnss_rx_{plan,acq,nav,measure,telemetry}.cpp.
#include "gnss_rx_impl.h"

namespace dect2 {

GnssReceiver::GnssReceiver() : p_(std::make_unique<Impl>()) {}
GnssReceiver::~GnssReceiver() = default;

void GnssReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->rate = inputRateHz;
    p_->rateAt = inputRateHz;
    p_->resetState();
    p_->lastRate = 0;
    p_->replan();
}
bool GnssReceiver::ready() const { return p_->rateAt.load() >= gnssTuning().minSampleRate - 1; }

void GnssReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->resetState();
    p_->lastRate = 0;
    if (p_->rate > 0) p_->replan();
}

void GnssReceiver::setClockCompare(bool on) { p_->clockCompare = on; }

void GnssReceiver::feed(const cf32* x, size_t n) {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lk(p.procMu);
    if (p.rate <= 0 || n == 0) return;
    if (p.planGen != p.settingsGen.load()) p.replan();
    // The input is cut into blocks of a fixed size whatever the size of the chunks it arrives in: the result does not depend on the chunking
    size_t i = 0;
    if (!p.pend.empty()) {
        const size_t take = std::min(kBlock - p.pend.size(), n);
        p.pend.insert(p.pend.end(), x, x + take);
        i = take;
        if (p.pend.size() == kBlock) { p.processBlock(p.pend.data(), kBlock); p.pend.clear(); }
    }
    while (n - i >= kBlock) { p.processBlock(x + i, kBlock); i += kBlock; }
    if (i < n) p.pend.insert(p.pend.end(), x + i, x + n);
    if (p.clockCompare.load()) {   // when the last sample arrived, less its place in the stream: the earliest of the last minute is the least delayed
        const double w = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const double v = w - (double)(p.inSamples + (int64_t)p.pend.size()) / p.rate;
        if (p.wallWin == 0) p.wallWin = w;
        if (w - p.wallWin > 30) { p.wallMinPrev = p.wallMinCur; p.wallMinCur = 1e300; p.wallWin = w; }
        p.wallMinCur = std::min(p.wallMinCur, v);
    }
}

bool GnssReceiver::telemetry(GnssTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (!p_->telReady || p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void GnssReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}
void GnssReceiver::setMeasurementCallback(std::function<void(const std::vector<GnssMeasurement>&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->measCb = std::move(cb);
}
void GnssReceiver::setSbasCallback(std::function<void(int, const uint8_t*, double)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->sbasCb = std::move(cb);
}
void GnssReceiver::setCenterMhz(double mhz) { p_->centerMhz = mhz; p_->settingsGen++; }
void GnssReceiver::setSystems(unsigned mask) { p_->sysMask = mask; p_->settingsGen++; }
void GnssReceiver::setWeekReference(int w) { p_->weekRef = w; }
void GnssReceiver::setSearchRange(double hz) { p_->dopHalf = hz; p_->settingsGen++; p_->lastRate = 0; }
void GnssReceiver::setFrequencyHint(double hz, bool valid) { p_->cfoHintHz = hz; p_->cfoHintValid = valid; }
void GnssReceiver::setAcquisitionRate(int u) { p_->acqRate = std::max(1, u); }
void GnssReceiver::setApproxPosition(double lat, double lon, bool valid) { p_->hintLat = lat; p_->hintLon = lon; p_->hintValid = valid; }
void GnssReceiver::setElevationMask(double deg) { p_->elMask = deg; }
bool GnssReceiver::getEphemeris(int prn, GpsEphemeris& e, int* week) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (prn < 1 || prn > 32 || !p_->sat[prn].hasEph) return false;
    e = p_->sat[prn].eph;
    if (week) *week = p_->sat[prn].week;
    return true;
}
bool GnssReceiver::getEphemerisOf(int sys, int prn, GpsEphemeris& e, int* week) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (sys == GnssGalileo) {
        if (prn < 1 || prn >= kGalPrns || !p_->gsat[prn].hasEph) return false;
        e = p_->gsat[prn].eph;
        if (week) *week = p_->gsat[prn].week;
        return true;
    }
    if (sys == GnssBeidou) {
        if (prn < 1 || prn >= kBdsPrns || !p_->bsat[prn].hasEph) return false;
        e = p_->bsat[prn].eph;
        if (week) *week = p_->bsat[prn].week;
        return true;
    }
    if (prn < 1 || prn >= kCaPrns || l1caSystem(prn) != sys || !p_->sat[prn].hasEph) return false;
    e = p_->sat[prn].eph;
    if (week) *week = p_->sat[prn].week;
    return true;
}
bool GnssReceiver::getGloEphemeris(int fcn, GloEphemeris& e) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    const int i = fcn - kGloMinK;
    if (i < 0 || i >= kGloSlots || !p_->gloSat[i].hasEph) return false;
    e = p_->gloEph[i];
    return true;
}
bool GnssReceiver::getAlmanac(int prn, GpsAlmanac& a) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (prn < 1 || prn > 32 || !p_->sat[prn].alm.valid) return false;
    a = p_->sat[prn].alm;
    return true;
}
bool GnssReceiver::getIonoUtc(GpsIono& i, GpsUtc& u) const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    i = p_->iono; u = p_->utc;
    return p_->iono.valid;
}
uint64_t GnssReceiver::workUnitsUsed() const { return p_->units; }
void GnssReceiver::cpuBreakdown(double* front, double* track, double* acq) const { *front = p_->cpuFront; *track = p_->cpuTrack; *acq = p_->cpuAcq; }

ModeTuning gnssTuning() {
    ModeTuning t;
    t.stdMode = 14; t.id = "gnss"; t.name = "GNSS";
    t.minMhz = 1550; t.maxMhz = 1610; t.defMhz = 1575.42;
    t.sampleRate = 4e6;          // the C/A main lobe is 2.046 MHz wide; 4 Msps is the lowest comfortable rate for GPS alone (the other tunes: gnssAutoRate)
    t.basebandHz = 2.5e6;
    t.bandwidthMhz = 2.046;
    t.minSampleRate = 2.046e6;
    return t;
}

int gnssTuneOf(double mhz) {
    for (int k = 0; k < (int)(sizeof kGnssTuneMhz / sizeof *kGnssTuneMhz); k++) if (std::fabs(mhz - kGnssTuneMhz[k]) < 0.001) return k;
    return GnssTuneCustom;
}
double gnssAutoRate(double mhz) {
    switch (gnssTuneOf(mhz)) {
    case GnssTuneGlo: return 10e6;        // k = -7 .. +6 lie -3.94 .. +3.38 MHz from 1602 MHz, each with +-0.511 MHz of code
    case GnssTuneAll: return kGnssAllRate;
    default: return gnssTuning().sampleRate;      // L1 and B1I: the main lobe of their 1 ms codes in 4 Msps
    }
}
const char* gnssTuneKey(double mhz) {
    switch (gnssTuneOf(mhz)) {
    case GnssTuneB1I: return "b1i";
    case GnssTuneGlo: return "glo";
    case GnssTuneAll: return "all";
    default: return "";
    }
}

} // namespace dect2
