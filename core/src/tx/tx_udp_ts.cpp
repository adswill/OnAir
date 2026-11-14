// The transmitter's UDP input (tx.h txUdpTsSource, #458): a transport stream that arrives over UDP or RTP, as the Transmit panel's
// "UDP stream" source. The receiving is the IPTV player's (iptv::Stream: udp:// and rtp://, multicast, RTP headers stripped, whole
// 188-byte packets); this file puts the packets in a queue the transmitter takes one packet at a time from, at the channel's rate.
#include "dect2/tx/tx.h"
#include "dect2/tv/iptv.h"
#include <algorithm>
#include <array>
#include <deque>
#include <mutex>

namespace dect2 {

namespace {

constexpr size_t kMaxQueued = 40000;   // 7.5 MB, about 2.5 s of a 24 Mbit/s stream: more and the oldest go (the stream is faster than the channel)

std::mutex gStatsMu;
TxUdpTsStats gStats;   // of the input running now (or the last one)

struct UdpTsInput {
    iptv::Stream stream;
    std::mutex mu;
    std::deque<std::array<uint8_t, 188>> q;
    uint64_t received = 0, dropped = 0, nulls = 0;
    std::string url;

    ~UdpTsInput() {
        stream.stop();
        publish(false);
    }
    void push(const uint8_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < n; i++) {
            std::array<uint8_t, 188> pk{};
            std::copy_n(p + i * 188, 188, pk.begin());
            q.push_back(pk);
            received++;
        }
        while (q.size() > kMaxQueued) { q.pop_front(); dropped++; }
    }
    void pull(uint8_t* out) {
        std::lock_guard<std::mutex> lk(mu);
        if (q.empty()) {   // nothing waits: a null packet keeps the channel's rate
            static const std::array<uint8_t, 4> kNullHead = {0x47, 0x1F, 0xFF, 0x10};
            std::fill_n(out, 188, (uint8_t)0xFF);
            std::copy(kNullHead.begin(), kNullHead.end(), out);
            nulls++;
        } else {
            std::copy(q.front().begin(), q.front().end(), out);
            q.pop_front();
        }
        if ((received + nulls) % 2048 == 0) publish(true);
    }
    void publish(bool running) {   // under mu, or at the end
        const iptv::StreamInfo inf = stream.info();
        std::lock_guard<std::mutex> lk(gStatsMu);
        gStats.running = running;
        gStats.url = url;
        gStats.state = inf.state;
        gStats.message = inf.message;
        gStats.received = received;
        gStats.dropped = dropped;
        gStats.nulls = nulls;
        gStats.queued = q.size();
    }
};

} // namespace

bool isTxUdpSpec(const std::string& s) { return s.rfind("udp://", 0) == 0 || s.rfind("rtp://", 0) == 0; }

bool txUdpTsSource(const std::string& spec, std::function<void(uint8_t*)>& outTs, std::string& note) {
    outTs = {};
    const iptv::Url u = iptv::parseUrl(spec);
    if (!isTxUdpSpec(spec) || !u.ok || u.port <= 0 || u.port > 65535) {
        note = "not a UDP address: write it as VLC does, udp://@:5004 (every network card), udp://@127.0.0.1:5004 or udp://@239.1.1.1:5004";
        return false;
    }
    auto in = std::make_shared<UdpTsInput>();
    in->url = spec;
    UdpTsInput* raw = in.get();
    iptv::StreamOptions o;
    o.liveEdgeStart = true;
    in->stream.start(spec, o, [raw](const uint8_t* p, size_t n) { raw->push(p, n); });
    { std::lock_guard<std::mutex> lk(raw->mu); raw->publish(true); }
    outTs = [in](uint8_t* pk) { in->pull(pk); };
    note = "the transport stream that arrives on " + spec + " (raw TS or RTP), sent as it is; null packets while none arrives";
    return true;
}

TxUdpTsStats txUdpTsStats() {
    std::lock_guard<std::mutex> lk(gStatsMu);
    return gStats;
}

} // namespace dect2
