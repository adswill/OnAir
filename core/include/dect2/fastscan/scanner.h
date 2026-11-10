// The fast wideband scanner: FFT channel power instead of retune-and-listen. Per hop it captures a block at the radio's full rate, computes the
// averaged power spectrum of all channels in the hop, calibrates it (hop-edge roll-off, DC spike), updates the dynamic floor, finds signals
// above the floor, feeds the hit tracker and writes the pseudo-spectrum (current / average / max-hold / floor), then moves to the next hop.
// Capture and processing run on two threads (the next hop is captured while the last one is processed); the buffers are allocated once.
// When a hit starts and a RecorderBank is given, the scanner stays on that hop (no retune) and records the channel from the same capture.
#pragma once
#include "dect2/fastscan/floor.h"
#include "dect2/fastscan/hits.h"
#include "dect2/fastscan/plan.h"
#include "dect2/fastscan/qualify.h"
#include "dect2/fastscan/record.h"
#include "dect2/sweep/sweep.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

struct FastScanConfig {
    FastPlanConfig plan;
    FloorParams floor;
    HitParams hits;
    std::string floorFile;           // per-band floor persistence ("" none): loaded at configure(), saved by saveFloor()
    double avgAlpha = 0.2;           // weight of a new look in the average trace
    double maxholdFallDbPerSec = 2;  // the max-hold trace falls at this rate (0: holds for ever)
    int threads = 0;                 // 0: the shared pool, 1: none (the result is the same)
    QualifyParams qualify;           // channel confirmation / spur / image rules
    bool identifyNew = false;        // a new hit: stay on its hop for a moment and name the signal (sig_ident.h)
};

struct SpectrumView {
    double startHz = 0, binHz = 0;
    uint64_t seq = 0;
    std::vector<float> cur, avg, maxhold, floor;   // dB per bin of the stitched grid
    std::vector<uint8_t> mask;
    size_t bins() const { return cur.size(); }
    double freqOf(size_t i) const { return startHz + (double)i * binHz; }
};

struct ScanStats {
    uint64_t passes = 0, hops = 0;
    double passSec = 0;              // wall time of the last pass
    double mhzPerSec = 0;            // measured: covered MHz / passSec
    double hopsPerSec = 0;
    double dspSecPerHop = 0;         // processing time of a hop (the DSP ceiling is usable MHz per hop / this)
    double settleSec = 0;            // the retune settle in use
    int activeHits = 0;
    bool parked = false;             // staying on one hop to record
};

// What the identifier said about a hit (stays with the frequency)
struct HitLabel { double freqHz = 0; std::string name, headline, modeId; int verdict = 0; float score = 0; };

struct RunHooks {
    RecorderBank* rec = nullptr;                          // record hits from the capture (the scan parks on the hop while any is recording)
    std::function<std::string(double hz)> modeOf;         // the mode a hit is recorded in (the memory entry's, else guessMode)
    std::function<void(const std::vector<HitEvent>&)> onEvents;   // called on the processing thread
    std::function<void(const ScanStats&)> onPass;                 // after every pass
    std::function<double()> clock;                        // seconds; default: wall clock
    int maxPasses = 0;                                    // 0: until cancelled
    double maxParkSec = 120;                              // longest stay on one hop for recording
};

// A source that delivers blocks tagged with the centre frequency they were taken at (the HackRF firmware sweep: the radio retunes by itself).
struct BlockSource {
    virtual ~BlockSource() {}
    virtual double sampleRate() const = 0;
    // The next block: its centre and samples. false: ended or failed (err; empty on cancel).
    virtual bool next(double& centerHz, std::vector<cf32>& out, const std::atomic<bool>* cancel, std::string& err) = 0;
};

class FastScanner {
public:
    FastScanner();
    ~FastScanner();
    bool configure(const FastScanConfig& c, const RadioProfile& r, std::string& err);
    const FastPlan& plan() const { return plan_; }
    const RadioProfile& profile() const { return prof_; }
    HitTracker& tracker() { return tracker_; }
    const DynamicFloor& floor() const { return floor_; }

    // One captured block of hop `hop` (plan.samplesPerHop samples) taken at time t. Everything the scanner knows is updated; the events (hits
    // that started or ended) are appended to ev.
    void processHop(int hop, const cf32* x, size_t n, double t, std::vector<HitEvent>& ev);

    // The scan loop over a source until cancelled / maxPasses. false: the source failed (err).
    bool run(sweep::SweepSource& src, const std::atomic<bool>* cancel, RunHooks& hk, std::string& err);

    // The same scan fed by a source that retunes by itself: every block is one hop capture of the hop whose centre it carries (within a quarter of
    // a hop). A pass ends when the hop index wraps. No recording in this mode (the radio is never parked on a hop).
    bool runBlocks(BlockSource& src, const std::atomic<bool>* cancel, RunHooks& hk, std::string& err);

    // thread-safe while a scan runs (the tracker is touched under the scanner's lock)
    std::vector<Hit> hitsSnapshot() const;
    void setFilters(std::vector<FreqRange> excluded, std::vector<double> locked, std::vector<double> priority);
    void removeHit(uint64_t id);
    std::vector<HitLabel> labels() const;    // thread-safe
    SpectrumView snapshot() const;           // copy of the pseudo-spectrum
    ScanStats stats() const;
    // The channel list of fast scan v2: hits with sweeps seen, class, and the spur / image / DC flags. reliableOnly hides unconfirmed and false ones.
    std::vector<ChannelInfo> channels(bool reliableOnly) const;
    void beginSweep() { std::lock_guard<std::mutex> g(m_); sweep_++; }   // run() does this by itself; tests driving processHop() call it per pass
    bool saveFloor() const;                  // to config.floorFile
    void resetTraces();                      // clears average and max-hold (not the floor)

private:
    void hopPsd(const cf32* x, size_t n);
    FastScanConfig cfg_;
    RadioProfile prof_;
    FastPlan plan_;
    DynamicFloor floor_;
    HopCalibration cal_;
    HitTracker tracker_;
    std::vector<float> win_, acc_, scratchRe_, scratchIm_, pw_, hopDb_, hopCal_, cand_;
    float winSum_ = 0;
    int log2n_ = 0;
    mutable std::mutex m_;
    SpectrumView view_;
    std::vector<double> lastT_;
    ScanStats st_;
    bool floorLoaded_ = false;
    Qualifier qual_;
    int sweep_ = 0;
    std::atomic<int> park_{-1};
    std::vector<HitLabel> labels_;   // under m_
};

}  // namespace fastscan
}  // namespace dect2
