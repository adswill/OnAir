// See one_radio.h.
#include "dect2/trunkpool/one_radio.h"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace dect2 {
namespace tpool {

OneRadio::OneRadio(const std::string& system, int slots) : system_(system), voice_(new CaptureVoice(slots)) { rebuild(); }
OneRadio::~OneRadio() = default;

void OneRadio::rebuild() {
    PoolConfig c;
    c.hangSec = 2.0;
    c.maxSetupSec = 2.5;
    c.systemName = system_;
    c.recordDir = dir_; c.record = !dir_.empty(); c.recordMetaOnly = false;
    c.promoteControl = false;
    c.maxCalls = std::max(8, voice_->slots());
    pool_ = std::make_unique<TrunkPool>(c);
    pool_->wallBase = (int64_t)std::time(nullptr) - (int64_t)nowSec();
    pool_->addDeviceRef(voice_.get());
    pool_->setControl(0);
    if (cc_ > 0) pool_->setControlChannel(cc_);
    voice_->setAudioSink([this](int slice, const float* x, size_t n, int rate) {
        if (!tap_ || !pool_) return;
        int tg = -1;
        for (const CallRecord* r : pool_->nowTalking()) if (r->slice == slice) { tg = r->tg; break; }
        if (listenTg_ >= 0 && tg != listenTg_) return;
        tap_(tg, x, n, rate);
    });
}

void OneRadio::configure(double rateHz) {
    fs_ = rateHz; n_ = 0;
    voice_->configure(rateHz);
    voice_->setCentreHz(centre_);
    rebuild();
}

void OneRadio::setCentreHz(double hz) {
    if (std::fabs(hz - centre_) < 1) return;
    centre_ = hz;
    voice_->configure(fs_);   // the slices were cut at the old offsets: they close, the calls end
    voice_->setCentreHz(hz);
    rebuild();
}

void OneRadio::setControlHz(double hz) { cc_ = hz; if (pool_) pool_->setControlChannel(hz); }
void OneRadio::setSlots(int n) { voice_->setSlots(n); PoolConfig c = pool_->config(); c.maxCalls = std::max(8, voice_->slots()); pool_->setConfig(c); }
int OneRadio::slots() const { return voice_->slots(); }
void OneRadio::setRecordDir(const std::string& dir) {
    dir_ = dir;
    PoolConfig c = pool_->config();
    c.recordDir = dir; c.record = !dir.empty();
    pool_->setConfig(c);
}
void OneRadio::setEnabled(bool on) { on_ = on; }
void OneRadio::setMonitorPrefix(const std::string& p) { voice_->setMonitorPrefix(p); }
void OneRadio::setP25p2System(int wacn, int sysId, int nac) { voice_->setP25p2System(wacn, sysId, nac); }
void OneRadio::setDcMode(int m) { voice_->setDcMode(m); }
void OneRadio::setAudioTap(std::function<void(int, const float*, size_t, int)> cb) { tap_ = std::move(cb); }

double OneRadio::nowSec() const { return fs_ > 0 ? (double)n_ / fs_ : 0; }

void OneRadio::onControlSeen() { pool_->onControlSeen(nowSec()); }

void OneRadio::onGrant(const GrantEvent& g) {
    const double now = nowSec();
    use_.add(g.freqHz, now);
    pool_->onControlSeen(now);
    if (!on_ || !(fs_ > 0) || !(centre_ > 0)) return;
    pool_->onGrant(g, now);
}

void OneRadio::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    n_ += n;
    if (on_ && centre_ > 0) voice_->feed(x, n);
    pool_->tick(nowSec());
}

std::vector<OneRadioCall> OneRadio::calls(size_t max) const {
    std::vector<OneRadioCall> out;
    const auto& v = pool_->calls();
    auto row = [&](const CallRecord& r) {
        OneRadioCall c;
        c.id = r.id; c.tg = r.tg; c.src = r.src; c.slot = r.slot; c.voiceSlot = r.active() && r.followed ? r.slice : -1;
        c.freqHz = r.freqHz; c.durSec = std::max(0.0, r.durationSec());
        c.active = r.active(); c.followed = r.followed; c.encrypted = r.encrypted; c.emergency = r.emergency;
        c.state = r.missed ? "not followed" : callStateName(r.state);
        c.note = r.note.empty() ? r.endReason : r.note;
        c.audioPath = r.audioPath;
        return c;
    };
    for (size_t k = v.size(); k-- > 0 && out.size() < max;) if (v[k].active() && !v[k].missed) out.push_back(row(v[k]));
    for (size_t k = v.size(); k-- > 0 && out.size() < max;) if (!v[k].active() || v[k].missed) out.push_back(row(v[k]));
    return out;
}

int OneRadio::activeCalls() const { int n = 0; for (const CallRecord* r : pool_->nowTalking()) n += r->followed; return n; }
std::string OneRadio::status() const { return pool_->statusLine(nowSec()); }
std::string OneRadio::lastNote() const { return pool_->messages.empty() ? std::string() : pool_->messages.back(); }

} // namespace tpool
} // namespace dect2
