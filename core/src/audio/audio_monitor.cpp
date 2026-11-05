// "Monitor audio": see dect2/audio/audio_monitor.h.
#include "dect2/common/thread_name.h"
#include "dect2/audio/audio_monitor.h"
#include "dect2/audio/audioout.h"
#include "dect2/audio/hf_demod.h"
#include "dect2/dsp/exact_resampler.h"
#include "ham_listen.h"
#include "../net/netcompat.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#endif

namespace dect2 {

namespace {
constexpr int kOutRate = 48000;

void setNonBlock(sock_t s) {
#ifdef _WIN32
    u_long m = 1; ioctlsocket(s, FIONBIO, &m);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)(v >> (8 * i))); }
void put16(std::string& s, uint16_t v) { s.push_back((char)v); s.push_back((char)(v >> 8)); }
// A WAV header for a stream of unknown length (the sizes are the maximum, which players read as "until the end")
std::string wavHeader() {
    std::string h = "RIFF";
    put32(h, 0xFFFFFFFFu); h += "WAVEfmt "; put32(h, 16); put16(h, 1); put16(h, 2); put32(h, kOutRate); put32(h, kOutRate * 4); put16(h, 4); put16(h, 16);
    h += "data"; put32(h, 0xFFFFFFFFu);
    return h;
}
}  // namespace

struct AudioMonitor::Impl {
    struct Block { std::vector<cf32> x; double rate = 0, shift = 0; };
    mutable std::mutex cfgMu;
    MonitorConfig cfg;
    std::atomic<bool> on{false};
    std::atomic<bool> muted{false};   // the app's mute (setMuted): output 1 only
    // queue and thread
    std::mutex qMu;
    std::condition_variable qCv;
    std::deque<Block> q;
    size_t qN = 0;
    std::thread th;
    bool quit = false;
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> fed{false};
    std::atomic<int64_t> lastPushMs{0};
    // DSP (processBlock only)
    hfdig::UsbAudio usb;
    hfdig::Upsample6 up;
    ExactResampler rs;
    double rsIn = 0;
    std::complex<double> ph{1, 0};
    std::vector<cf32> mixed, dec;
    std::vector<float> aud, pcm;
    // outputs (under outMu)
    std::mutex outMu;
    std::unique_ptr<AudioOut> o1, o2;
    std::string dev1, dev2, outError;
    bool wasYield = false;
    std::function<void(const float*, int)> sink;
    // network
    sock_t ls = kBadSock;
    int lport = 0; bool lall = false, lwav = true;
    std::vector<sock_t> clients;
    std::atomic<int> nClients{0};
    std::atomic<float> levelDb{-120};
    std::atomic<bool> yielded{false};

    void closeNet() {
        for (sock_t c : clients) sockClose(c);
        clients.clear(); nClients = 0;
        if (ls != kBadSock) { sockClose(ls); ls = kBadSock; }
    }
    void openNet(const MonitorConfig& c) {
        if (ls != kBadSock && lport == c.port && lall == c.bindAll && lwav == c.wav) return;
        closeNet();
        lport = c.port; lall = c.bindAll; lwav = c.wav;
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) { outError = "network stream: no socket"; return; }
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)c.port);
        a.sin_addr.s_addr = htonl(c.bindAll ? INADDR_ANY : INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) {
            outError = "network stream: port " + std::to_string(c.port) + " is in use or not allowed";
            sockClose(ls); ls = kBadSock;
            return;
        }
        setNonBlock(ls);
    }
    void sendNet(const float* s, int frames) {
        if (ls == kBadSock) return;
        for (;;) {   // new listeners
            sockaddr_in pa{}; socklen_t pl = sizeof pa;
            const sock_t k = accept(ls, (sockaddr*)&pa, &pl);
            if (k == kBadSock) break;
            setNonBlock(k);
#ifdef SO_NOSIGPIPE
            { int one = 1; setsockopt(k, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof one); }
#endif
            if (lwav) { const std::string h = wavHeader(); if (sockSend(k, h.data(), h.size()) != (long long)h.size()) { sockClose(k); continue; } }
            clients.push_back(k);
        }
        if (!clients.empty() && frames > 0) {
            std::vector<int16_t> b((size_t)frames * 2);
            for (size_t i = 0; i < b.size(); i++) b[i] = (int16_t)std::lrintf(std::max(-1.f, std::min(1.f, s[i])) * 32767.f);
            for (size_t i = 0; i < clients.size();) {   // a listener that cannot keep up loses the block; one that is gone is dropped
#ifdef MSG_NOSIGNAL
                const long long r = send(clients[i], (const char*)b.data(), b.size() * 2, MSG_NOSIGNAL);
#else
                const long long r = sockSend(clients[i], b.data(), b.size() * 2);
#endif
                bool dead = false;
                if (r < 0) {
#ifdef _WIN32
                    dead = WSAGetLastError() != WSAEWOULDBLOCK;
#else
                    dead = errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
#endif
                }
                if (dead) { sockClose(clients[i]); clients.erase(clients.begin() + (long)i); } else i++;
            }
        }
        nClients = (int)clients.size();
    }

    void closeOutputs() {
        std::lock_guard<std::mutex> lk(outMu);
        o1.reset(); o2.reset(); dev1.clear(); dev2.clear();
        closeNet();
        yielded = false; wasYield = false;
    }
    static void ensureOut(std::unique_ptr<AudioOut>& o, std::string& cur, const std::string& dev, const MonitorConfig& c, std::string& err) {
        if (o && cur == dev) return;
        o.reset();
        auto n = std::make_unique<AudioOut>();
        n->setMonitorRole(true);
        n->setDevice(dev);
        if (!n->start(kOutRate)) { err = "audio output \"" + (dev.empty() ? std::string("default") : dev) + "\" could not be opened"; return; }
        n->setStartThreshold(kOutRate * c.latencyMs / 1000);
        o = std::move(n); cur = dev;
    }
    void output(const float* s, int frames, const MonitorConfig& c) {
        std::lock_guard<std::mutex> lk(outMu);
        outError.clear();
        const bool yield = c.yield && AudioOut::msSinceModeWrite() < 400;
        yielded = yield;
        if (yield) {   // the mode plays: nothing from here; the queued audio goes
            if (!wasYield) { if (o1) o1->flush(); if (o2) o2->flush(); }
            wasYield = true;
            return;
        }
        wasYield = false;
        if (sink) {   // tests: the sink stands in for the speaker; the network stream is real
            if (!muted.load()) sink(s, frames);
            if (c.net) { openNet(c); sendNet(s, frames); }
            return;
        }
        const int maxLag = kOutRate * std::max(c.latencyMs, 20) * 2 / 1000;
        auto play = [&](std::unique_ptr<AudioOut>& o, std::string& cur, const std::string& dev, float vol, bool mute) {
            ensureOut(o, cur, dev, c, outError);
            if (!o) return;
            o->setVolume(vol); o->setMuted(mute);
            o->setStartThreshold(kOutRate * c.latencyMs / 1000);
            if (o->bufferedFrames() > maxLag) o->flush();   // the radio's clock runs ahead of the sound card: catch up
            o->write(s, frames);
        };
        play(o1, dev1, c.device, c.volume, muted.load());
        if (c.out2) play(o2, dev2, c.device2, c.volume2, false); else { o2.reset(); dev2.clear(); }
        if (c.net) { openNet(c); sendNet(s, frames); } else if (ls != kBadSock) closeNet();
    }

    void process(const cf32* x, size_t n, double rate, double shift) {
        const MonitorConfig c = [&] { std::lock_guard<std::mutex> lk(cfgMu); return cfg; }();
        if (n == 0 || rate <= 0) return;
        usb.setDemod(c.demod);
        mixed.assign(x, x + n);
        if (shift != 0) {   // the wanted channel to 0 Hz
            const std::complex<double> st = std::polar(1.0, -2 * M_PI * shift / rate);
            std::complex<double> p = ph;
            for (size_t i = 0; i < n; i++) {
                mixed[i] *= std::complex<float>((float)p.real(), (float)p.imag());
                p *= st;
                if ((i & 1023) == 1023) p /= std::abs(p);
            }
            ph = p / std::abs(p);
        }
        if (rsIn != rate) { rsIn = rate; if (!rs.configure(rate, marine::kBaseRate)) rs.configure(1, 1); }
        const cf32* src = mixed.data(); size_t m = n;
        if (!rs.passthrough()) { dec.clear(); rs.process(mixed.data(), n, dec); src = dec.data(); m = dec.size(); }
        aud.clear(); pcm.clear();
        double pw = 0;
        usb.process(src, m, aud, pw);
        if (aud.empty()) return;
        up.process(aud.data(), aud.size(), pcm);
        double e = 0;
        for (float v : pcm) e += (double)v * v;
        levelDb = (float)(10 * std::log10(e / std::max<size_t>(1, pcm.size()) + 1e-12));
        output(pcm.data(), (int)(pcm.size() / 2), c);
    }

    void loop() {
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(qMu);
                qCv.wait(lk, [&] { return quit || !q.empty(); });
                if (quit) return;
                b = std::move(q.front()); q.pop_front(); qN -= b.x.size();
            }
            if (on.load()) process(b.x.data(), b.x.size(), b.rate, b.shift);
        }
    }
};

AudioMonitor::AudioMonitor() : p_(new Impl) {}
AudioMonitor::~AudioMonitor() { stop(); }

const char* AudioMonitor::demodName(int d) { return hfDemodName(d); }

void AudioMonitor::configure(const MonitorConfig& c) {
    bool turnedOff, changed;
    {
        std::lock_guard<std::mutex> lk(p_->cfgMu);
        turnedOff = p_->cfg.on && !c.on;
        changed = p_->cfg.device != c.device || p_->cfg.device2 != c.device2 || p_->cfg.out2 != c.out2;
        p_->cfg = c;
    }
    p_->on = c.on;
    if (turnedOff || changed) {
        { std::lock_guard<std::mutex> lk(p_->qMu); p_->q.clear(); p_->qN = 0; }
        p_->closeOutputs();
        p_->usb.reset(); p_->up.reset();
    }
}

void AudioMonitor::setMuted(bool m) { p_->muted = m; }
bool AudioMonitor::enabled() const { return p_->on.load(std::memory_order_relaxed); }
MonitorConfig AudioMonitor::config() const { std::lock_guard<std::mutex> lk(p_->cfgMu); return p_->cfg; }

void AudioMonitor::push(const cf32* x, size_t n, double rate, double shiftHz) {
    if (!p_->on.load(std::memory_order_relaxed) || n == 0) return;
    {
        std::lock_guard<std::mutex> lk(p_->qMu);
        if (!p_->th.joinable()) { p_->quit = false; p_->th = dect2::namedThread("onair-audmon", [this] { p_->loop(); }); }
        const size_t cap = std::max<size_t>(1u << 18, (size_t)(rate * 0.5));
        if (p_->qN + n > cap) { p_->dropped += n; return; }   // a live radio cannot wait
        p_->q.push_back({std::vector<cf32>(x, x + n), rate, shiftHz});
        p_->qN += n;
        p_->fed = true;
    }
    p_->qCv.notify_one();
}

void AudioMonitor::processBlock(const cf32* x, size_t n, double rate, double shiftHz) { p_->process(x, n, rate, shiftHz); }

void AudioMonitor::setSink(std::function<void(const float*, int)> sink) { std::lock_guard<std::mutex> lk(p_->outMu); p_->sink = std::move(sink); }

MonitorStats AudioMonitor::stats() const {
    MonitorStats s;
    s.running = p_->on.load() && p_->th.joinable();
    s.yielded = p_->yielded.load();
    s.levelDb = p_->levelDb.load();
    s.dropped = p_->dropped.load();
    s.clients = p_->nClients.load();
    { std::lock_guard<std::mutex> lk(p_->outMu); s.error = p_->outError; }
    return s;
}

void AudioMonitor::stop() {   // the thread and the outputs end; the setting stays, and the next push() starts them again
    { std::lock_guard<std::mutex> lk(p_->qMu); p_->quit = true; p_->q.clear(); p_->qN = 0; }
    p_->qCv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    p_->closeOutputs();
}

}  // namespace dect2
