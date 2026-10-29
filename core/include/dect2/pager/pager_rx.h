// Pagers: POCSAG and FLEX paging messages on one 25 kHz (or 12.5 kHz) channel.
// Skeleton: counts the samples, measures the input level and reports four times a second of signal (state 0: no decoder yet); the
// mode's worker replaces the body. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "pager_tel.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class PagerReceiver {
public:
    PagerReceiver();
    ~PagerReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's frequency sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the messages stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(PagerTelemetry& out, uint64_t lastSeq);
    // The message list stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setProtocols(int mask);                         // 1 POCSAG, 2 FLEX (default 3: both)
    void setPocsagBauds(int mask);                       // 1 512, 2 1200, 4 2400 bd (default 7: all three)
    void setWideInput(bool on);                          // the input is one carrier brought to 192 ksps by the whole-span bank (accepted below the radio's minimum rate)
    void setNarrowChannel(bool on);                      // channel filter width: false 25 kHz (default), true 12.5 kHz (rejects a neighbouring channel better; the signal must be centred)
    bool narrowChannel() const;
    int protocols() const;                               // the masks as set (the whole-span bank copies them to its receivers)
    int pocsagBauds() const;
    void setPocsagCharset(int cs);                       // PocsagCharset, already resolved (kPocsagCharPlain default, kPocsagCharGerman): the text of new POCSAG pages; alphaText stays raw
    int pocsagCharset() const;
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning pagerTuning();

} // namespace dect2
