// DTMF keypad for the analog voice transmitter (tx_dtmf.h)
#include "dect2/tx/tx_dtmf.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2 {

using dect2::math::kPi;

bool txDtmfFreqs(char c, double& rowHz, double& colHz) {
    const char u = (char)std::toupper((unsigned char)c);
    for (int i = 0; i < 16; i++)
        if (tones::kDtmfKeys[i] == u) { rowHz = tones::kDtmfRows[i / 4]; colHz = tones::kDtmfCols[i % 4]; return true; }
    return false;
}

bool txDtmfValidKey(char c) { double a, b; return txDtmfFreqs(c, a, b); }

std::string txDtmfClean(const std::string& keys) {
    std::string o;
    for (char c : keys) if (txDtmfValidKey(c)) o += (char)std::toupper((unsigned char)c);
    return o;
}

TxDtmfQueue& TxDtmfQueue::global() { static TxDtmfQueue q; return q; }

int TxDtmfQueue::push(const std::string& keys) {
    const std::string k = txDtmfClean(keys);
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (char c : k) { if (q_.size() >= 256) break; q_.push_back(c); n++; }
    n_ = q_.size();
    return n;
}

void TxDtmfQueue::clear() { std::lock_guard<std::mutex> lk(mu_); q_.clear(); n_ = 0; sent_.clear(); }

void TxDtmfQueue::setTiming(const TxDtmfTiming& t) {
    std::lock_guard<std::mutex> lk(mu_);
    t_ = t;
    t_.toneMs = std::max(10.0, std::min(2000.0, t.toneMs));
    t_.gapMs = std::max(10.0, std::min(2000.0, t.gapMs));
    t_.levelPct = std::max(1.0, std::min(50.0, t.levelPct));
    t_.twistDb = std::max(-6.0, std::min(6.0, t.twistDb));
}

TxDtmfTiming TxDtmfQueue::timing() const { std::lock_guard<std::mutex> lk(mu_); return t_; }

bool TxDtmfQueue::pop(char& key, TxDtmfTiming& t) {
    if (n_.load() == 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    key = q_.front();
    q_.pop_front();
    n_ = q_.size();
    t = t_;
    if (sent_.size() < 512) sent_ += key;
    return true;
}

std::string TxDtmfQueue::sent() const { std::lock_guard<std::mutex> lk(mu_); return sent_; }

void TxDtmfSequencer::start(char key, const TxDtmfTiming& t) {
    double r = 0, c = 0;
    if (!txDtmfFreqs(key, r, c)) return;
    toneLen_ = toneLeft_ = std::max(1L, (long)std::lround(t.toneMs * 1e-3 * fs_));
    left_ = toneLeft_ + std::max(1L, (long)std::lround(t.gapMs * 1e-3 * fs_));
    ramp_ = std::max(1L, std::min(toneLen_ / 4, (long)std::lround(2e-3 * fs_)));   // 2 ms raised-cosine edges: no clicks, no splatter
    pos_ = 0;
    w1_ = 2 * kPi * r / fs_;
    w2_ = 2 * kPi * c / fs_;
    const double tw = std::pow(10.0, t.twistDb / 20.0), lv = t.levelPct / 100.0;
    // the pair keeps the set level in total power terms: a1^2 + a2^2 = 2 lv^2
    a1_ = lv * std::sqrt(2.0 / (1.0 + tw * tw));
    a2_ = a1_ * tw;
    p1_ = p2_ = 0;
}

float TxDtmfSequencer::next() {
    if (left_ <= 0) {
        char k;
        TxDtmfTiming t;
        if (!q_ || !q_->pop(k, t)) return 0.f;
        start(k, t);
        if (left_ <= 0) return 0.f;
    }
    float y = 0;
    if (toneLeft_ > 0) {
        double env = 1;
        if (pos_ < ramp_) env = 0.5 - 0.5 * std::cos(kPi * (pos_ + 0.5) / ramp_);
        else if (toneLeft_ <= ramp_) env = 0.5 - 0.5 * std::cos(kPi * (toneLeft_ - 0.5) / ramp_);
        y = (float)(env * (a1_ * std::sin(p1_) + a2_ * std::sin(p2_)));
        p1_ += w1_; if (p1_ > 2 * kPi) p1_ -= 2 * kPi;
        p2_ += w2_; if (p2_ > 2 * kPi) p2_ -= 2 * kPi;
        toneLeft_--;
        pos_++;
    }
    left_--;
    return y;
}

std::vector<float> txDtmfAudio(const std::string& keys, double fs, const TxDtmfTiming& t) {
    TxDtmfQueue q;
    q.setTiming(t);
    q.push(keys);
    TxDtmfSequencer s(fs, &q);
    std::vector<float> a;
    do { a.push_back(s.next()); } while (s.active() || q.pending() > 0);
    return a;
}

}  // namespace dect2
