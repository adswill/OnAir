// Stall watchdog of a USB sample stream (Android RTL-SDR, rtl_fd_shim.h; no Android types, so the logic runs in desktop tests).
// A phone can stop a bulk stream without anything unplugging: the endpoint halts (EPIPE, "broken pipe") or the transfers stop completing
// (power saving, a USB controller glitch). Waiting 10 s and then reopening the radio was not enough: a halted endpoint stays halted over a
// reopen, so the stream died again at once and the spectrum stayed frozen. The ladder, climbed one step per stall or error burst and reset
// when samples have flowed again for kHealthySec:
//   1. clear the endpoint's halt and resubmit the transfers   2. the same again   3. reset the USB device, then resubmit   4. give up on this
//   open: the caller returns and the radio is closed and opened again (native_common.h), after which the ladder starts over.
#pragma once
#include <algorithm>
#include <cstdint>

namespace dect2 {

struct StreamStats {
    uint64_t transfers = 0, bytes = 0, errors = 0, stalls = 0, restarts = 0, resets = 0, reopens = 0;
    int lastError = 0;
};

class StreamWatchdog {
public:
    enum class Action { None, ClearHaltRestart, ResetRestart, Reopen };
    static constexpr double kStallSec = 2.0;     // no completed transfer for this long = a stall
    static constexpr double kHealthySec = 3.0;   // samples this long after a restart = the stream is well again
    static constexpr int kErrorBurst = 3;        // consecutive failed transfers before the stream is restarted (one stray error is ignored)

    void start(double now) { lastData_ = now; healthySince_ = now; burst_ = 0; rung_ = 0; }
    // a transfer completed with `bytes` of samples
    void onData(double now, uint64_t bytes) {
        st_.transfers++; st_.bytes += bytes;
        lastData_ = now; burst_ = 0;
        if (rung_ > 0 && now - healthySince_ >= kHealthySec) rung_ = 0;
    }
    // a transfer failed with this libusb code (kUsbPipe -9 ...); a device that is gone (-4) means Reopen at once
    Action onError(double now, int libusbCode) {
        st_.errors++; st_.lastError = libusbCode;
        if (libusbCode == -4) return escalate(now, true);
        if (++burst_ < kErrorBurst) return Action::None;
        burst_ = 0;
        return escalate(now, false);
    }
    // called while waiting; an Action when the silence has lasted kStallSec
    Action poll(double now) {
        if (now - lastData_ < kStallSec) return Action::None;
        st_.stalls++;
        return escalate(now, false);
    }
    const StreamStats& stats() const { return st_; }
    int rung() const { return rung_; }

private:
    Action escalate(double now, bool gone) {
        lastData_ = now;           // the next stall is counted from here
        healthySince_ = now;
        if (gone || rung_ >= 3) { rung_ = 0; st_.reopens++; return Action::Reopen; }
        rung_++;
        st_.restarts++;
        if (rung_ == 3) { st_.resets++; return Action::ResetRestart; }
        return Action::ClearHaltRestart;
    }
    StreamStats st_;
    double lastData_ = 0, healthySince_ = 0;
    int burst_ = 0, rung_ = 0;
};

} // namespace dect2
