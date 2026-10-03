#include "dect2/tsout.h"

#include "netcompat.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

using Clock = std::chrono::steady_clock;

struct OutputManager::Impl {
    mutable std::mutex mu;
    OutputConfig cfg;
    FILE* file = nullptr;
    sock_t sock = kBadSock;
    sockaddr_in dst{};
    OutputStats st;
    // service filter
    ServiceFilter filter;
    // udp pacing
    struct Dgram { std::vector<uint8_t> data; Clock::time_point due; };
    std::deque<Dgram> queue;
    std::vector<uint8_t> building;   // datagram under construction (payload packets)
    std::vector<std::vector<uint8_t>> burst; // datagrams of the current frame
    std::condition_variable cv;
    std::thread sender;
    bool running = false;
    uint16_t rtpSeq = 0;
    uint32_t rtpTs = 0;
    Clock::time_point lastDue = Clock::now();

    void startSender() {
        running = true;
        sender = std::thread([this] {
            std::unique_lock<std::mutex> lk(mu);
            while (running) {
                if (queue.empty()) { cv.wait_for(lk, std::chrono::milliseconds(50)); continue; }
                auto due = queue.front().due;
                if (Clock::now() < due) { cv.wait_until(lk, due); continue; }
                Dgram d = std::move(queue.front());
                queue.pop_front();
                if (sock != kBadSock) {
                    long long n = sockSendTo(sock, d.data.data(), d.data.size(), dst);
                    if (n > 0) { st.udpDatagrams++; st.udpBytes += (uint64_t)n; }
                }
                if (!queue.empty()) st.udpQueueMs = std::chrono::duration<double, std::milli>(queue.back().due - Clock::now()).count();
                else st.udpQueueMs = 0;
            }
        });
    }
    void stopSender() {
        { std::lock_guard<std::mutex> lk(mu); running = false; }
        cv.notify_all();
        if (sender.joinable()) sender.join();
    }
};

OutputManager::OutputManager() : p_(new Impl) { p_->startSender(); }
OutputManager::~OutputManager() { close(); p_->stopSender(); delete p_; }

OutputConfig OutputManager::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }

void OutputManager::close() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->file) { fclose(p_->file); p_->file = nullptr; }
    if (p_->sock != kBadSock) { sockClose(p_->sock); p_->sock = kBadSock; }
    p_->queue.clear();
    p_->building.clear();
    p_->burst.clear();
    p_->st.fileOpen = p_->st.udpOpen = false;
}

void OutputManager::configure(const OutputConfig& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    OutputConfig old = I.cfg;
    I.cfg = c;
    I.st.error.clear();
    if (I.file && (!c.file || c.path != old.path)) { fclose(I.file); I.file = nullptr; I.st.fileOpen = false; }
    if (c.file && !I.file && !c.path.empty()) {
        I.file = fopen(c.path.c_str(), "wb");
        if (!I.file) I.st.error = "cannot open " + c.path;
        else { I.st.fileOpen = true; I.st.fileBytes = I.st.filePackets = 0; }
    }
    if (I.sock != kBadSock && (!c.udp || c.host != old.host || c.port != old.port)) { sockClose(I.sock); I.sock = kBadSock; I.st.udpOpen = false; I.queue.clear(); }
    if (c.udp && I.sock == kBadSock) {
        netInit();
        I.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (I.sock == kBadSock) I.st.error = "socket failed";
        else {
            memset(&I.dst, 0, sizeof I.dst);
            I.dst.sin_family = AF_INET;
            I.dst.sin_port = htons((uint16_t)c.port);
            if (inet_pton(AF_INET, c.host.c_str(), &I.dst.sin_addr) != 1) { I.st.error = "bad UDP address " + c.host; sockClose(I.sock); I.sock = kBadSock; }
            else {
                int ttl = c.ttl;
                setsockopt(I.sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
                I.st.udpOpen = true;
                I.st.udpDatagrams = I.st.udpBytes = I.st.udpDropped = 0;
            }
        }
    }
    if (c.serviceId != old.serviceId) I.filter.select(c.serviceId);
}

void OutputManager::packet(const uint8_t* pkt, const TsSnapshot* snap) { packets(pkt, 1, snap); }

void OutputManager::packets(const uint8_t* data, size_t n, const TsSnapshot* snap) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    if (!I.file && I.sock == kBadSock) return;
    for (size_t i = 0; i < n; i++) {
    const uint8_t* pkt = data + i * 188;
    const int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    uint8_t rewritten[188];
    const uint8_t* out = pkt;
    if (I.cfg.dropNull && pid == 0x1FFF) continue;
    if (I.cfg.serviceId >= 0) {
        if (I.filter.selected() != I.cfg.serviceId) I.filter.select(I.cfg.serviceId);
        if (!I.filter.process(pkt, snap, rewritten)) continue;
        out = rewritten;
    }
    if (I.file) { fwrite(out, 1, 188, I.file); I.st.fileBytes += 188; I.st.filePackets++; }
    if (I.sock != kBadSock) {
        I.building.insert(I.building.end(), out, out + 188);
        if (I.building.size() >= 7 * 188) {
            I.burst.push_back(std::move(I.building));
            I.building.clear();
        }
    }
    }
}

void OutputManager::burstDone(double frameSeconds) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    if (I.file) fflush(I.file);
    if (I.sock == kBadSock) { I.burst.clear(); I.building.clear(); return; }
    if (!I.building.empty()) { // pad the tail datagram with nulls so nothing waits for the next frame
        while (I.building.size() < 7 * 188) { uint8_t nul[188]; memset(nul, 0xFF, 188); nul[0] = 0x47; nul[1] = 0x1F; nul[2] = 0xFF; nul[3] = 0x10; I.building.insert(I.building.end(), nul, nul + 188); }
        I.burst.push_back(std::move(I.building));
        I.building.clear();
    }
    if (I.burst.empty()) return;
    const double span = std::max(0.02, frameSeconds);
    const auto now = Clock::now();
    auto base = std::max(now, I.lastDue);
    // keep the queue bounded: if more than ~3 frames are waiting, drop what is oldest
    if (!I.queue.empty() && std::chrono::duration<double>(I.queue.back().due - now).count() > 3.0 * span + 0.2) {
        I.st.udpDropped += I.queue.size();
        I.queue.clear();
        base = now;
    }
    // if a backlog has built up, drain it a little faster instead of letting the latency grow
    const double backlog = std::chrono::duration<double>(base - now).count();
    const double eff = backlog > span ? span * 0.6 : span;
    const double step = eff / I.burst.size();
    for (size_t i = 0; i < I.burst.size(); i++) {
        Impl::Dgram d;
        if (I.cfg.rtp) {
            d.data.resize(12);
            d.data[0] = 0x80; d.data[1] = 33;
            d.data[2] = I.rtpSeq >> 8; d.data[3] = I.rtpSeq & 0xFF; I.rtpSeq++;
            I.rtpTs += (uint32_t)(step * 90000);
            d.data[4] = I.rtpTs >> 24; d.data[5] = I.rtpTs >> 16; d.data[6] = I.rtpTs >> 8; d.data[7] = I.rtpTs;
            d.data[8] = 0x44; d.data[9] = 0x54; d.data[10] = 0x32; d.data[11] = 0x00;
            d.data.insert(d.data.end(), I.burst[i].begin(), I.burst[i].end());
        } else d.data = std::move(I.burst[i]);
        d.due = base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(step * i));
        I.queue.push_back(std::move(d));
    }
    I.lastDue = base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(span));
    I.burst.clear();
    I.cv.notify_all();
}

OutputStats OutputManager::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

} // namespace dect2
