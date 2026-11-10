// The engine's side of multi-channel TV (multitv.h): when it is asked for and the capture holds more than one channel of the chosen width, the
// DVB-T / DVB-T2 receiver is replaced by one receiver per occupied channel; the selected channel's packets go the way the single receiver's did.
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

void Engine::selectTvChannel(int slot) {
    auto m = mtvGet();
    if (!m || m->selected() == slot) return;
    const auto rows = m->runningSlots();
    if (std::find(rows.begin(), rows.end(), slot) == rows.end()) return;
    const TsSnapshot snap = m->snapshot(slot);
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        player_.select(-1);   // the picture of the other channel stops; the interface picks the service again
        m->setSelected(slot);
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
    const bool on = mtvOn_.load();
    const int sm = stdMode_.load(), mx = mtvMax_.load(), grid = mtvGrid_.load();
    const double rate = rate_.load(), bw = bwMhz_;
    if (on == mtvKeyOn_ && sm == mtvKeyStd_ && mx == mtvKeyMax_ && grid == mtvKeyGrid_ && rate == mtvKeyRate_ && bw == mtvKeyBw_) return;
    const bool wasActive = mtvActive_.load();
    mtvKeyOn_ = on; mtvKeyStd_ = sm; mtvKeyMax_ = mx; mtvKeyGrid_ = grid; mtvKeyRate_ = rate; mtvKeyBw_ = bw;
    std::string note;
    bool want = on;
    if (on && !multiChannelSupported(sm)) { want = false; note = "Whole span works with a TV standard selected (not automatic detection)"; }
    else if (on && sm == 8 && !wholeSpanFits(sm, rate, bw, grid)) { want = false; note = "the capture is too narrow for Whole span: DVB-S/S2 needs at least 3 Msps"; }
    else if (on && sm != 8 && slotsInSpan(rate, bw, grid) < 2) {
        want = false;
        char b[200];
        snprintf(b, sizeof b, "the capture (%.2f Msps) holds only one %g MHz channel: capture at least %g MHz for two %g MHz channels", rate / 1e6, bw, 2 * bw, bw);
        note = b;
    }
    { std::lock_guard<std::mutex> lk(mtvNoteMu_); mtvNote_ = note; }
    if (!want) {
        if (!wasActive) return;
        mtvActive_ = false; mtvSat_ = false; mtvOther_ = -1;
        { std::shared_ptr<MultiTv> m; { std::lock_guard<std::mutex> lk(mtvMu_); m.swap(mtv_); } }
        player_.select(-1);
        resetReceiver(activeStd_.load());
        log("multi-channel TV off");
        return;
    }
    auto m = std::make_shared<MultiTv>();
    MultiTvOptions o;
    o.dvbt = sm == 2;
    o.sat = sm == 8;
    o.otherStd = (sm == 3 || sm == 5 || sm == 6 || sm == 9 || sm == 42) ? sm - 1 : -1;
    if (o.sat) o.minRsHz = std::max(1e6, dvbsMinAutoSymbolRate(rate));
    o.spanRate = rate;
    o.widthMhz = bw;
    o.maxChannels = mx > 0 ? mx : defaultMaxChannels();
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
    std::shared_ptr<MultiTv> old;
    {
        std::lock_guard<std::mutex> lk(mtvMu_);
        old = std::move(mtv_);
        mtv_ = m;
    }
    old.reset();
    mtvSat_ = o.sat;
    mtvOther_ = o.otherStd;
    mtvActive_ = true;
    char b[200];
    if (o.sat) snprintf(b, sizeof b, "whole span: DVB-S/S2 transponders, up to %d decoded at once from a %.2f Msps capture", o.maxChannels, rate / 1e6);
    else snprintf(b, sizeof b, "multi-channel TV: %s, %g MHz channels, up to %d at once from a %.2f Msps capture", o.dvbt ? "DVB-T" : "DVB-T2", bw, o.maxChannels, rate / 1e6);
    log(b);
}

// "Decode everything in view" (setWideAll): configures the session for the capture on the analysis thread; a new rate or centre starts the search again
void Engine::syncWideAll() {
    const bool on = wideAllOn_.load();
    const double rate = rate_.load(), centre = wideAllCentre_.load();
    if (on == wideAllKeyOn_ && (!on || (rate == wideAllKeyRate_ && centre == wideAllKeyCentre_))) { if (on) wideAll_.setBudget(wideAllBudget_.load()); return; }
    wideAllKeyOn_ = on; wideAllKeyRate_ = rate; wideAllKeyCentre_ = centre;
    if (!on) { wideAll_.reset(); return; }
    WsConfig c;
    c.spanRate = rate; c.centerHz = centre; c.cpuBudget = wideAllBudget_.load();
    c.blockingTv = !(src_ && src_->realtimeHardware());
    wideAll_.configure(c);
    wideAll_.setAudioSink([this](const float* st, size_t frames) {
        if (!vfoSoundOn_.load()) return;
        std::lock_guard<std::mutex> lk(vfoOutMu_);
        if (!vfoOut_) { vfoOut_.reset(new AudioOut()); if (!vfoOut_->start(VfoBank::kAudioRate)) { vfoOut_.reset(); return; } }
        vfoOut_->write(st, (int)frames);
    });
    char b[160];
    snprintf(b, sizeof b, "decode everything in view: %.2f Msps capture, centre %.4f MHz", rate / 1e6, centre / 1e6);
    log(b);
}

// Smart CubeSat: configured on the analysis thread for the capture; a new rate or centre starts the search again
void Engine::syncSmartCube() {
    const bool on = smartCubeOn_.load();
    const double rate = rate_.load(), centre = wideAllCentre_.load();
    if (on == smartKeyOn_ && (!on || (rate == smartKeyRate_ && centre == smartKeyCentre_))) return;
    smartKeyOn_ = on; smartKeyRate_ = rate; smartKeyCentre_ = centre;
    if (!on) { smartCube_.reset(); return; }
    smartCube_.configure(rate);
    smartCube_.setCenterHz(lastDev_.kind != DeviceInfo::File ? centre : 0.0);
    smartCube_.setWallClock(lastDev_.kind != DeviceInfo::File);
    smartCube_.setLogCallback([this](const std::string& s) { log(s); });
    char b[160];
    snprintf(b, sizeof b, "Smart CubeSat: %.2f Msps capture, centre %.4f MHz", rate / 1e6, centre / 1e6);
    log(b);
}

} // namespace dect2
