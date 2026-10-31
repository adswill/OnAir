// Carrier finder and channelizer for "decode the whole visible span": from the power spectrum of the captured span, find the carriers that sit
// on a channel raster (12.5 kHz, 25 kHz, 6.25 kHz or any step), keep each one tracked while it talks and for a holding time after it goes quiet,
// and bring each one down to a baseband stream at the rate a single-channel decoder wants. Nothing here knows a protocol.
//
//   span samples -> windowed FFT, averaged -> power in each raster cell -> noise floor per cell (a low percentile of the cells around it,
//   bounded by the floor of the whole span, smoothed over time: a sloped or rippled front end does not make carriers) -> threshold with
//   hysteresis (on above floor + onDb, stays above floor + offDb; a known channel of the frequency database or band plan gets priorDb off)
//   -> not a carrier: the skirt of a strong neighbour, an unmodulated line (a spur, the LO; rejectTones) -> one signal that covers two
//   adjacent cells is one carrier (no dip between the cells; mergeSplit) -> tracked carriers: a carrier that drifts into the next cell keeps
//   its id (trackDrift); a newcomer must stay above the threshold for debounceSec and reach minSnrDb -> at most maxCarriers (the active and
//   the strongest first, a newcomer must beat a tracked one by 6 dB to push it out, the kept carrier never) -> Channelizer per carrier.
//
// Frequencies are in Hz relative to the centre of the span (0 = the middle of the samples). The raster is gridOriginHz + k * stepHz.
// After a retune of the radio by d Hz, shift(d) moves the tracked carriers with it (same ids), so their decoders keep running.
#pragma once
#include "resampler.h"
#include "dect2/common/ring.h"
#include <algorithm>
#include <complex>
#include <memory>
#include <utility>
#include <vector>

namespace dect2 {

struct CarrierCfg {
    double stepHz = 12500;          // channel raster (6250, 12500, 25000 or any)
    double gridOriginHz = 0;        // a channel centre sits at gridOriginHz + k * stepHz (relative to the span centre)
    double onDb = 9;                // a cell must be this far above the noise floor to count as a carrier
    double offDb = 5;               // and it counts as quiet only below this (hysteresis)
    double holdSec = 8;             // a carrier stays tracked this long after it went quiet
    double debounceSec = 0.3;       // dwell: it must stay above the threshold this long before it is reported
    double minSnrDb = 0;            // and reach this SNR at least once meanwhile (0: onDb is enough)
    int maxCarriers = 8;            // simultaneous carriers (the CPU budget of the decoders)
    double avgSec = 0.1;            // one spectrum estimate covers this much signal
    double skirtDb = 30;            // a cell this far below a neighbour is that neighbour's skirt, not a carrier
    double edgeFrac = 0.04;         // the outer part of the span (filter roll-off) is not searched
    int localCells = 12;            // the noise floor of a cell: from the cells within this many on each side (0: one floor for the span)
    double floorSec = 1.0;          // time constant of the noise floor
    bool mergeSplit = true;         // one signal over two adjacent cells (a wide carrier, or one between two raster points) is one carrier
    bool trackDrift = true;         // a tracked carrier that moved into the next cell keeps its id
    bool rejectTones = false;       // an unmodulated line (nearly all the cell's power in 3 bins) is not a carrier
    std::vector<double> priorHz;    // known channels (frequency database, band plan), relative to the span centre
    double priorDb = 3;             // their threshold is this much lower
    bool snapReported = true;       // a whole-span row/log reports the nearest raster channel centre, not the measured centroid (WideBank)
    bool priorOnly = false;         // only the known channels are searched
};

struct Carrier {
    int id = 0;                     // stable while it is tracked (also over a drift into the next cell and a retune), never reused
    double hz = 0;                  // centre frequency relative to the span centre (the raster cell centre moved to the measured centroid)
    double cellHz = 0;              // the raster cell centre
    double levelDb = -200;          // power in the cell, dB relative to full scale (rms 1.0 over the whole span)
    double snrDb = 0;               // above the noise floor in the same cell width
    double widthHz = 0;             // occupied width (the bins more than 6 dB above the floor), 0 before the first measurement
    bool active = false;            // above the threshold now
    bool prior = false;             // on a known channel
    double ageSec = 0;              // since it was first reported
    double idleSec = 0;             // since it was last active (0 while active)
};

class CarrierFinder {
public:
    void configure(double spanRateHz, const CarrierCfg& cfg);
    void reset();
    void feed(const cf32* x, size_t n);
    const std::vector<Carrier>& carriers() const { return cars_; }
    // ids of carriers dropped (hold time over, or pushed out by the cap) since the last call
    std::vector<int> takeRemoved();
    double noiseDb() const { return noiseDb_; }
    uint64_t updates() const { return updates_; }
    const CarrierCfg& cfg() const { return cfg_; }
    double rate() const { return rate_; }
    // The radio moved by deltaHz: every tracked carrier moves by -deltaHz in the span (same id); those that left the span are removed.
    // The spectrum average and the noise floor start again.
    void shift(double deltaHz);
    // Lower cap than cfg.maxCarriers (the CPU budget); 0 = cfg.maxCarriers. Applies with the next update.
    void setCap(int n) { cap_ = n; }
    int cap() const { return cap_ > 0 && cap_ < cfg_.maxCarriers ? cap_ : cfg_.maxCarriers; }
    // This carrier is never pushed out by the cap (the one listened to); 0 = none
    void setKeep(int id) { keep_ = id; }
    void setPriors(const std::vector<double>& hz, bool only = false) { cfg_.priorHz = hz; cfg_.priorOnly = only; }
    void setMaxCarriers(int n) { cfg_.maxCarriers = n > 0 ? n : 1; }
    // Tuning that may change while running (the settings of the table); the raster and the rate stay
    void setThresholds(double onDb, double offDb, double minSnrDb, double debounceSec, double holdSec);
    // Parts of the span that hold something wide (a TV channel): their cells are neither searched nor counted in the noise floor, and a tracked
    // carrier inside one is dropped. {low Hz, high Hz} relative to the span centre. Empty: the whole span as before.
    // Stop tracking this carrier (no removal report: the caller knows). Returns false when it is not tracked.
    bool forget(int id) { for (size_t i = 0; i < trk_.size(); i++) if (trk_[i].reported && trk_[i].c.id == id) { trk_.erase(trk_.begin() + (long)i); cars_.erase(std::remove_if(cars_.begin(), cars_.end(), [id](const Carrier& c) { return c.id == id; }), cars_.end()); return true; } return false; }
    void setExclusions(std::vector<std::pair<double, double>> zones) { excl_ = std::move(zones); }

private:
    void update();
    CarrierCfg cfg_;
    double rate_ = 0, noiseDb_ = -200;
    int nfft_ = 0, fill_ = 0, nAvg_ = 0, avgTarget_ = 1;
    std::vector<float> win_;
    std::vector<double> acc_;
    std::vector<cf32> buf_, wbuf_, work_;
    struct FftHolder;
    std::shared_ptr<FftHolder> fft_;
    struct Track { Carrier c; double aboveSec = 0, peakSnr = -100; bool reported = false; };
    std::vector<Track> trk_;
    std::vector<Carrier> cars_;
    std::vector<int> removed_;
    std::vector<double> floorDb_;   // smoothed noise floor per raster cell (dB), indexed from kMin
    int floorKMin_ = 0;
    std::vector<std::pair<double, double>> excl_;
    int nextId_ = 1, cap_ = 0, keep_ = 0;
    uint64_t updates_ = 0;
    bool noiseInit_ = false;
};

// One carrier brought to baseband: mixed down by shiftHz (the carrier's frequency in the span), low-pass filtered and resampled to outRate.
class Channelizer {
public:
    bool configure(double inRate, double outRate, double shiftHz, double occupiedHz);
    void reset();
    // appends the carrier's baseband samples to `out`
    void process(const cf32* x, size_t n, std::vector<cf32>& out);
    double shiftHz() const { return shift_; }
    // follows a drifting carrier or a retune: the mixer moves, the phase and the filter state stay (no gap in the output)
    void setShift(double hz) { shift_ = hz; }
    double outRate() const { return outRate_; }

private:
    // The mixer: a table of the first kTab phasor steps in float and one double phasor per table period (renormalised), the loop body plain
    // float arithmetic the compiler vectorises (the std::complex<double> phasor with its NaN checks per sample cost more than the resampler).
    static constexpr size_t kTab = 1024;
    RationalResampler rs_;
    std::vector<cf32> mix_;
    std::vector<float> tre_, tim_;
    double inRate_ = 0, outRate_ = 0, shift_ = 0;
    double phRe_ = 1, phIm_ = 0, stepRe_ = 1, stepIm_ = 0;
    size_t pos_ = 0;
};

} // namespace dect2
