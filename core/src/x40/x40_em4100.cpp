// EM4100 / EM4102 LF RFID tags (125 kHz): a reader holds the carrier, the tag answers by loading it, an amplitude (ASK) change of a few percent. The tag repeats
// a 64-bit Manchester frame at RF/64 (1953 bit/s; RF/32 is read too): 9 header ones, ten rows of 4 data bits plus an even row parity, four column parities
// (even) and a stop bit 0. The 40 data bits are an 8-bit version / customer code and a 32-bit ID. The envelope is sliced with a hysteresis comparator, the
// time between level changes gives one or two half-bits, and every 128 half-bits are tested against the frame rules (parity makes a chance hit about 1 in 2^20).
// Receive only: nothing is written to a tag, nothing is cloned or replayed.
#include "x40_int.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 16000, kCut = 5000;
constexpr int kHalves = 128;

class EmDec {
public:
    // Three timing classes (RF/64, RF/32); each keeps its own half-bit clock estimate
    struct Lane {
        double nominal = 0, est = 0;      // seconds of one half-bit
        std::vector<uint8_t> h;           // half-bit levels
        double lastEdge = 0; uint8_t level = 0; bool valid = false; uint64_t absEnd = 0, lastFrameEnd = 0;
    };
    bool configure(double fs) {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate();
        lane_[0].nominal = 64.0 / 125000 / 2; lane_[1].nominal = 32.0 / 125000 / 2;
        reset(); return true;
    }
    void reset() {
        dec_.reset(); n_ = 0; y1_ = 0; hi_ = lo_ = 0; init_ = false; state_ = 0; mean_ = 0; swing_ = 0; noiseAcc_ = 0;
        for (auto& l : lane_) { l.est = l.nominal; l.h.clear(); l.lastEdge = 0; l.valid = false; l.absEnd = 0; l.lastFrameEnd = 0; l.level = 0; }
        lastId_ = ~0ull; lastRow_ = -1e9; reads_ = 0; tags_ = 0;
    }
    void setSink(Sink* s) { out_ = s; }
    void feed(const cf32* x, size_t n) {
        dec_.process(x, n, [&](cf32 a) { sample(std::abs(a)); });
    }
    void fill(X40Telemetry& t) const {
        if (!init_) { t.headline = "waiting for data"; return; }
        t.headline = tags_ ? fmt("%d tag reads, last %s", reads_, lastText_.c_str()) : "carrier found, no tag read yet";
        t.facts.push_back({"carrier level", fmt("%.1f dB (relative)", toDb(mean_ * mean_))});
        t.facts.push_back({"modulation depth", swing_ > 0 && mean_ > 0 ? fmt("%.1f %% of the carrier", 100.0 * swing_ / mean_) : std::string("none seen")});
        t.facts.push_back({"tags seen", fmt("%d different, %d reads", tags_, reads_)});
        t.facts.push_back({"bit rate", fmt("RF/%d", (int)std::lround(lane_[bestLane_].est * 2 * 125000))});
    }
private:
    void sample(float e) {
        n_++;
        const double t = n_ / rate_;
        // smoothing over about one eighth of a half-bit, a slow mean for the carrier level
        const float a = 0.5f;
        y1_ += a * (e - y1_);
        mean_ += 0.0005f * (e - mean_);
        if (!init_) { init_ = true; hi_ = lo_ = y1_; mean_ = e; }
        // hi / lo trackers: follow the extremes up at once, decay slowly (about 60 ms) toward the middle
        const float d = 1.f / (float)(0.06 * rate_);
        if (y1_ > hi_) hi_ = y1_; else hi_ -= d * (hi_ - y1_);
        if (y1_ < lo_) lo_ = y1_; else lo_ += d * (y1_ - lo_);
        const float span = hi_ - lo_, mid = 0.5f * (hi_ + lo_);
        // noise floor of the envelope: the sample-to-sample change; a swing is only believed well over it
        const float dd = e - prevE_; prevE_ = e;
        if (std::fabs(dd) < 0.25f * span + 1e-9f) noiseAcc_ += 0.001f * (dd * dd - noiseAcc_);   // edges of the tag are left out
        const float sigma = std::sqrt(std::max(noiseAcc_, 1e-20f) * 0.5f);
        if (span < 5 * sigma + 1e-6f * std::max(mean_, 1e-6f)) { state_ = 0; return; }
        const float hyst = 0.22f * span;
        int ns = state_;
        if (y1_ > mid + hyst) ns = 1; else if (y1_ < mid - hyst) ns = 0;
        if (ns != state_) { edge(t, state_, ns); state_ = ns; swing_ = 0.99f * swing_ + 0.01f * span; }
    }
    void edge(double t, int prev, int) {
        for (int li = 0; li < 2; li++) {
            Lane& l = lane_[li];
            if (!l.valid) { l.valid = true; l.lastEdge = t; l.level = (uint8_t)(prev ? 0 : 1); continue; }
            const double dt = t - l.lastEdge; l.lastEdge = t;
            const double r = dt / l.est;
            int nh = 0;
            if (r > 0.6 && r < 1.45) { nh = 1; l.est += 0.05 * (dt - l.est); }
            else if (r > 1.6 && r < 2.45) { nh = 2; l.est += 0.02 * (dt * 0.5 - l.est); }
            else { l.h.clear(); l.est = l.nominal; l.level = (uint8_t)(prev ? 0 : 1); continue; }
            l.est = std::min(l.nominal * 1.15, std::max(l.nominal * 0.85, l.est));
            const uint8_t lv = (uint8_t)prev;      // the level that lasted for dt
            for (int k = 0; k < nh; k++) { l.h.push_back(lv); l.absEnd++; }
            if (l.h.size() > 400) l.h.erase(l.h.begin(), l.h.begin() + 200);
            l.level = (uint8_t)(prev ? 0 : 1);
            tryFrame(li);
        }
    }
    static bool frame(const uint8_t* h, int pol, uint64_t& data40) {
        uint8_t b[64];
        for (int i = 0; i < 64; i++) {
            const uint8_t a = h[2 * i], c = h[2 * i + 1];
            if (a == c) return false;
            b[i] = pol ? a : (uint8_t)!a;
        }
        for (int i = 0; i < 9; i++) if (!b[i]) return false;
        if (b[63]) return false;
        int col[4] = {0, 0, 0, 0}; uint64_t v = 0;
        for (int r = 0; r < 10; r++) {
            int p = 0;
            for (int c = 0; c < 4; c++) { const int bit = b[9 + 5 * r + c]; p ^= bit; col[c] ^= bit; v = v << 1 | (uint64_t)bit; }
            if (p != b[9 + 5 * r + 4]) return false;
        }
        for (int c = 0; c < 4; c++) if (col[c] != b[59 + c]) return false;
        data40 = v; return true;
    }
    void tryFrame(int li) {
        Lane& l = lane_[li];
        for (int lag = 0; lag < 4; lag++) {
            if ((int)l.h.size() < kHalves + lag) return;
            const uint8_t* h = l.h.data() + l.h.size() - kHalves - lag;
            for (int pol = 0; pol < 2; pol++) {
                uint64_t v;
                if (!frame(h, pol, v)) continue;
                const uint64_t end = l.absEnd - lag;
                if (l.lastFrameEnd && end < l.lastFrameEnd + 100) return;
                l.lastFrameEnd = end; bestLane_ = li;
                got(v, li);
                return;
            }
        }
    }
    void got(uint64_t v, int li) {
        const double now = out_->now();
        reads_++;
        const unsigned ver = (unsigned)(v >> 32) & 0xFF; const uint32_t id = (uint32_t)(v & 0xFFFFFFFFu);
        lastText_ = fmt("%02X %08X", ver, id);
        if (v == lastId_ && now - lastRow_ < 3.0) { lastRow_ = now; out_->good(depthDb(), 0); return; }
        if (v != lastId_) tags_++;
        lastId_ = v; lastRow_ = now;
        const unsigned cust = (unsigned)(v >> 24) & 0xFF;
        out_->good(depthDb(), 0);
        out_->row("Tag", fmt("%02X%08X", ver, id),
                  fmt("version / customer %02X, ID %08X (%u), upper 16 bits of the ID %u, lower 16 bits %u, RF/%d", ver, id, id, (id >> 16) & 0xFFFF, id & 0xFFFF, li ? 32 : 64), toDb(mean_ * mean_));
        (void)cust;
    }
    float depthDb() const { return mean_ > 0 && swing_ > 0 ? 20.f * std::log10(std::max(swing_ / mean_, 1e-6f)) : -99.f; }
public:
    Sink* out_ = nullptr;
private:
    Decimator dec_; double rate_ = 1; uint64_t n_ = 0;
    float y1_ = 0, hi_ = 0, lo_ = 0, mean_ = 0, swing_ = 0, noiseAcc_ = 0, prevE_ = 0; bool init_ = false; int state_ = 0;
    Lane lane_[2]; int bestLane_ = 0;
    uint64_t lastId_ = ~0ull; double lastRow_ = -1e9; int reads_ = 0, tags_ = 0; std::string lastText_;
};

class EmWrap : public Decoder {
public:
    bool configure(double fs) override { d_.out_ = out; return d_.configure(fs); }
    void reset() override { d_.reset(); }
    void feed(const cf32* x, size_t n) override { d_.out_ = out; d_.feed(x, n); }
    void fill(X40Telemetry& t) const override { d_.fill(t); }
private:
    EmDec d_;
};
}

std::unique_ptr<Decoder> makeEm4100() { return std::make_unique<EmWrap>(); }

} // namespace dect2::x40
