// Airband clips per transmission: see airband_clips.h.
#include "dect2/airband/airband_clips.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace dect2 {

namespace {
int64_t sysNow() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::filesystem::path P(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }
void le16(std::string& o, uint16_t v) { o += (char)(v & 0xff); o += (char)(v >> 8); }
void le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; i++) o += (char)((v >> (8 * i)) & 0xff); }
}  // namespace

AirbandClipWriter::AirbandClipWriter(std::string folder, int rate) : folder_(std::move(folder)), rate_(rate), clock_(sysNow) {}

void AirbandClipWriter::setEnabled(bool on) { std::lock_guard<std::mutex> lk(mu_); enabled_ = on; }
bool AirbandClipWriter::enabled() const { std::lock_guard<std::mutex> lk(mu_); return enabled_; }
void AirbandClipWriter::setClock(std::function<int64_t()> c) { std::lock_guard<std::mutex> lk(mu_); clock_ = std::move(c); }
void AirbandClipWriter::setChannels(const std::vector<AirbandClipChannel>& ch) {
    std::lock_guard<std::mutex> lk(mu_);
    bool same = ch.size() == chans_.size();
    for (size_t i = 0; same && i < ch.size(); i++) same = ch[i].freqHz == chans_[i].freqHz;
    chans_ = ch;
    if (!same) st_.assign(ch.size(), Ch());   // other channels: the open clips and the replay belong to the old list
}

std::string AirbandClipWriter::fileName(const std::string& name, double freqHz, int64_t utc) {
    std::string n;
    for (unsigned char c : name) n += (std::isalnum(c) || c == '.') ? (char)std::toupper(c) : '_';
    if (n.size() > 24) n.resize(24);
    const time_t t = (time_t)utc;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char ts[24];
    std::strftime(ts, sizeof ts, "%Y%m%dT%H%M%SZ", &g);
    char b[128];
    std::snprintf(b, sizeof b, "airband-%s-%.3f-%s.wav", n.empty() ? "CH" : n.c_str(), freqHz / 1e6, ts);
    return b;
}

std::string AirbandClipWriter::writeWav(const std::string& path, const std::vector<float>& x, std::string& err) const {
    std::error_code ec;
    std::filesystem::create_directories(P(folder_), ec);
    std::string o;
    o += "RIFF"; le32(o, (uint32_t)(36 + x.size() * 2)); o += "WAVEfmt "; le32(o, 16); le16(o, 1); le16(o, 1); le32(o, (uint32_t)rate_); le32(o, (uint32_t)rate_ * 2); le16(o, 2); le16(o, 16);
    o += "data"; le32(o, (uint32_t)(x.size() * 2));
    for (float v : x) { const int s = (int)std::lround(std::max(-1.f, std::min(1.f, v)) * 32767.f); le16(o, (uint16_t)(int16_t)s); }
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write " + path; return ""; }
    f.write(o.data(), (std::streamsize)o.size());
    return f ? path : "";
}

void AirbandClipWriter::finish(int chan, Ch& c) {
    c.open = false;
    if (c.zeros < c.clip.size()) c.clip.resize(c.clip.size() - c.zeros);   // the trailing silence out
    c.zeros = 0;
    const double sec = (double)c.clip.size() / rate_;
    if (sec >= minSec_ && enabled_) {
        const AirbandClipChannel& ch = (size_t)chan < chans_.size() ? chans_[(size_t)chan] : AirbandClipChannel();
        const std::string path = folder_ + "/" + fileName(ch.name, ch.freqHz, c.startUtc);
        std::string err;
        if (!writeWav(path, c.clip, err).empty()) {
            voice::Clip k;
            k.path = path; k.mode = "airband"; k.wallStart = c.startUtc; k.seconds = sec; k.bytes = 44 + c.clip.size() * 2;
            char lb[96]; std::snprintf(lb, sizeof lb, "%s %.3f MHz", ch.name.c_str(), ch.freqHz / 1e6);
            k.label = lb;
            done_.push_back(k);
            if (done_.size() > 500) done_.erase(done_.begin());
        }
    }
    c.clip.clear();
}

void AirbandClipWriter::feed(int chan, const float* x, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (chan < 0) return;
    if ((size_t)chan >= st_.size()) st_.resize((size_t)chan + 1);
    Ch& c = st_[(size_t)chan];
    const size_t ringMax = (size_t)(kReplaySec * rate_);
    const size_t closeN = (size_t)(closeSec_ * rate_);
    for (size_t i = 0; i < n; i++) {
        const float v = x[i];
        const bool sound = v != 0.f;
        if (sound || c.open) { c.ring.push_back(v); if (c.ring.size() > ringMax) c.ring.pop_front(); }   // the replay holds the transmissions, not the gaps
        if (!c.open) {
            if (!sound) continue;
            c.open = true;
            c.startUtc = clock_() - (int64_t)std::floor((double)(n - i) / rate_);
            c.clip.clear(); c.zeros = 0;
        }
        c.clip.push_back(v);
        if (sound) c.zeros = 0;
        else if (++c.zeros >= closeN) finish(chan, c);
        if (c.open && c.clip.size() > (size_t)(600 * rate_)) finish(chan, c);   // a stuck carrier: at most 10 minutes per clip
    }
}

void AirbandClipWriter::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < st_.size(); i++) if (st_[i].open) finish((int)i, st_[i]);
}

std::vector<voice::Clip> AirbandClipWriter::takeFinished() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<voice::Clip> v;
    v.swap(done_);
    return v;
}

std::string AirbandClipWriter::saveReplay(int chan, std::string& err) {
    std::vector<float> x;
    AirbandClipChannel ch;
    int64_t now;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (chan < 0 || (size_t)chan >= st_.size() || st_[(size_t)chan].ring.empty()) { err = "nothing heard on this channel yet"; return ""; }
        x.assign(st_[(size_t)chan].ring.begin(), st_[(size_t)chan].ring.end());
        if ((size_t)chan < chans_.size()) ch = chans_[(size_t)chan];
        now = clock_();
    }
    const std::string path = folder_ + "/" + fileName("replay-" + ch.name, ch.freqHz, now);
    return writeWav(path, x, err);
}

}  // namespace dect2
