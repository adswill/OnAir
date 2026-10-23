// The engine's side of whole-span TV (spantv/spantv.h; multi-channel TV, multitv.h): when it is asked for and the capture holds more than one
// channel of the chosen width (raster standards: DVB-T / DVB-T2) or any capture for a free-frequency standard (DVB-S / S2), the single receiver
// is replaced by one receiver per occupied signal; the selected signal's packets go the way the single receiver's did.
#include "dect2/engine/engine.h"
#include <algorithm>
#include <thread>

namespace dect2 {

int Engine::defaultMaxChannels() { return std::thread::hardware_concurrency() >= 8 ? 3 : 2; }

int Engine::slotsInSpan(double spanRate, double widthMhz, int grid) {
    const double w = widthMhz * 1e6;
    if (spanRate <= 0 || w <= 0) return 0;
    const size_t on = spanSlots(spanRate, w, 0).size(), between = spanSlots(spanRate, w, 0.5 * w).size();
    return (int)(grid == 1 ? on : grid == 2 ? between : std::max(on, between));
}

void Engine::setMultiChannel(bool on, int maxChannels, int grid) {
    mtvMax_ = maxChannels > 0 ? std::min(maxChannels, 8) : 0;
    mtvGrid_ = std::max(0, std::min(2, grid));
    mtvOn_ = on;
}

void Engine::setSpanDecodeAll(bool on) {
    mtvAll_ = on;
    if (auto m = mtvGet()) m->setDecodeAll(on);
}
void Engine::setSpanDecode(int slot, bool on) { if (auto m = mtvGet()) m->setDecode(slot, on); }
void Engine::setSpanPinned(int slot, bool on) { if (auto m = mtvGet()) m->setPinned(slot, on); }

void Engine::selectTvChannel(int slot) {
    auto m = mtvGet();
    if (!m || m->selected() == slot) return;
    const auto rows = m->runningSlots();
    const bool running = std::find(rows.begin(), rows.end(), slot) != rows.end();
    if (!running && m->raster()) return;   // a raster channel plays only once it has a receiver
    const TsSnapshot snap = m->snapshot(slot);
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        player_.select(-1);   // the picture of the other channel stops; the interface picks the service again
        m->setSelected(slot);   // free frequency: a signal without a receiver gets one at the next evaluation
        tsSnap_ = snap;
    }
}

std::vector<TvChannelRow> Engine::tvChannels() {
    auto m = mtvGet();
    return m ? m->channels() : std::vector<TvChannelRow>();
}

std::vector<MultiService> Engine::multiServices() {
    auto m = mtvGet();
    return m ? m->services() : std::vector<MultiService>();
}

void Engine::onMultiPackets(const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap) {
    std::lock_guard<std::mutex> lk(tsMu_);
    if (!playerExternal_) for (size_t i = 0; i < n; i++) { ttx_.feedTs(pk + i * 188); dvbsub_.feedTs(pk + i * 188); }   // bitmap subtitles as on the single-channel path
    tsSnap_ = snap;
    outputs_.packets(pk, n, &tsSnap_);
    if (tap_) tap_(pk, n, &tsSnap_, secs);
    if (rtap_) rtap_(pk, n, &tsSnap_, secs);
    if (player_.selected() >= 0 && !playerExternal_) player_.push(pk, n, tsSnap_);
    outputs_.burstDone(secs);
}

// Called on the analysis thread every round: starts, changes or stops the multi-channel receivers when the switch, the mode, the capture's rate or
// the channel width changed. With the switch off (or a capture that holds one channel) nothing here touches the single receiver.
void Engine::syncMultiTv() {
    const bool on = mtvOn_.load(), all = mtvAll_.load();
    const int sm = stdMode_.load(), mx = mtvMax_.load(), grid = mtvGrid_.load();
    const double rate = rate_.load(), bw = bwMhz_;
    if (on == mtvKeyOn_ && sm == mtvKeyStd_ && mx == mtvKeyMax_ && grid == mtvKeyGrid_ && rate == mtvKeyRate_ && bw == mtvKeyBw_) {
        mtvKeyAll_ = all;   // applied live (setSpanDecodeAll)
        return;
    }
    const bool wasActive = mtvActive_.load();
    mtvKeyOn_ = on; mtvKeyStd_ = sm; mtvKeyMax_ = mx; mtvKeyGrid_ = grid; mtvKeyRate_ = rate; mtvKeyBw_ = bw; mtvKeyAll_ = all;
    std::string note;
    bool want = on;
    const bool raster = spanTvRaster(sm);
    if (on && !multiChannelSupported(sm)) { want = false; note = "whole-span TV works with DVB-T2, DVB-T or DVB-S/S2 selected (not automatic detection)"; }
    else if (on && raster && slotsInSpan(rate, bw, grid) < 2) {
        want = false;
        char b[200];
        snprintf(b, sizeof b, "the capture (%.2f Msps) holds only one %g MHz channel: a wider capture is needed for several", rate / 1e6, bw);
        note = b;
    } else if (on && !raster && rate < 1e6) { want = false; note = "the capture is too narrow for whole-span satellite TV (1 Msps at least)"; }
    { std::lock_guard<std::mutex> lk(mtvNoteMu_); mtvNote_ = note; }
    if (!want) {
        if (!wasActive) return;
        mtvActive_ = false;
        { std::shared_ptr<SpanTv> m; { std::lock_guard<std::mutex> lk(mtvMu_); m.swap(mtv_); } }
        player_.select(-1);
        resetReceiver(activeStd_.load());
        log("multi-channel TV off");
        return;
    }
    auto m = std::make_shared<SpanTv>();
    SpanTvOptions o;
    o.stdMode = sm;
    o.spanRate = rate;
    o.widthMhz = bw;
    if (raster) o.maxChannels = mx > 0 ? mx : defaultMaxChannels();
    else { o.maxChannels = mx > 0 ? mx : 8; o.cpuBudgetCores = mx > 0 ? 0 : -1; }   // satellite "automatic": as many as the CPU budget allows
    o.decodeAll = all;
    o.autoGrid = grid == 0;
    o.gridOffsetHz = grid == 2 ? 0.5 * bw * 1e6 : 0;
    o.blocking = !(src_ && src_->realtimeHardware());
    if (!m->configure(o)) { std::lock_guard<std::mutex> lk(mtvNoteMu_); mtvNote_ = "the capture is too narrow for multi-channel TV"; return; }
    m->setSink([this](int, const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap) { onMultiPackets(pk, n, secs, snap); });
    player_.select(-1);
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        tsSnap_ = TsSnapshot();
    }
    std::shared_ptr<SpanTv> old;
    {
        std::lock_guard<std::mutex> lk(mtvMu_);
        old = std::move(mtv_);
        mtv_ = m;
    }
    old.reset();
    mtvActive_ = true;
    char b[200];
    if (raster) snprintf(b, sizeof b, "multi-channel TV: %s, %g MHz channels, up to %d at once from a %.2f Msps capture", spanTvName(sm), bw, o.maxChannels, rate / 1e6);
    else if (o.cpuBudgetCores < 0) snprintf(b, sizeof b, "whole-span TV: %s, every transponder of a %.2f Msps capture within %.1f cores", spanTvName(sm), rate / 1e6, m->budgetCores());
    else snprintf(b, sizeof b, "whole-span TV: %s, up to %d transponders at once from a %.2f Msps capture", spanTvName(sm), o.maxChannels, rate / 1e6);
    log(b);
}

} // namespace dect2
