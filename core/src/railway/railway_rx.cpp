#include "dect2/railway/railway_rx.h"
#include "dect2/railway/railway_modem.h"
#include <cmath>
#include <ctime>
#include <mutex>
#include <sstream>

namespace dect2 {
namespace {
ModeTuning tuning(bool tdcs) {
    ModeTuning t;
    t.stdMode = tdcs ? 149 : 148; t.id = tdcs ? "tdcs" : "klw"; t.name = tdcs ? "TDCS" : "KLW";
    t.minMhz = 400; t.maxMhz = 900; t.defMhz = tdcs ? 457.550 : 866.2375;
    t.sampleRate = 960000; t.basebandHz = 150000; t.bandwidthMhz = 0.035; t.minSampleRate = 192000; t.tuneOffsetHz = 50000;
    return t;
}
}
ModeTuning klwTuning() { return tuning(false); }
ModeTuning tdcsTuning() { return tuning(true); }
struct RailwayReceiver::Impl {
    explicit Impl(railway::Protocol p) : protocol(p) {}
    mutable std::mutex mu;
    railway::Protocol protocol;
    RailwayTelemetry tel;
    railway::Modem modem;
    bool ready = false;
    double offset = 0, lastPublish = 0, lastMessage = -1e9;
    std::function<void(const std::string&)> log;
};
RailwayReceiver::RailwayReceiver(railway::Protocol p) : p_(std::make_unique<Impl>(p)) {}
RailwayReceiver::~RailwayReceiver() = default;
void RailwayReceiver::configure(double rate) {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->ready = std::isfinite(rate) && rate >= klwTuning().minSampleRate;
    if (p_->ready) p_->modem.configure(rate);
    p_->tel.inputRate = rate; p_->tel.state = p_->ready ? 1 : 0; p_->tel.seq++;
    p_->lastPublish = 0; p_->lastMessage = -1e9;
}
void RailwayReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lock(p_->mu); p_->offset = hz; p_->modem.setOffset(hz); }
void RailwayReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lock(p_->mu); p_->tel.centerHz = hz; }
bool RailwayReceiver::ready() const { std::lock_guard<std::mutex> lock(p_->mu); return p_->ready; }
void RailwayReceiver::reset() {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->modem.reset(); p_->modem.setOffset(p_->offset);
    const auto seq = p_->tel.seq; const double rate = p_->tel.inputRate, center = p_->tel.centerHz;
    p_->tel = RailwayTelemetry();
    p_->tel.seq = seq + 1; p_->tel.inputRate = rate; p_->tel.centerHz = center; p_->tel.state = p_->ready ? 1 : 0;
    p_->lastPublish = 0; p_->lastMessage = -1e9;
}
void RailwayReceiver::clearMessages() {
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->tel.messages.clear(); p_->tel.dataValid = false; p_->tel.state = p_->ready ? 1 : 0; p_->lastMessage = -1e9; p_->tel.seq++;
}
void RailwayReceiver::feed(const cf32* iq, size_t n) {
    if (!iq || !n) return;
    std::lock_guard<std::mutex> lock(p_->mu);
    if (!p_->ready) return;
    std::vector<railway::Message> found; p_->modem.feed(iq, n, found);
    for (auto& m : found) {
        if (m.protocol != p_->protocol) continue;
        m.wallTime = (int64_t)std::time(nullptr);
        if (p_->tel.messages.size() >= 400) p_->tel.messages.pop_back();
        p_->tel.messages.insert(p_->tel.messages.begin(), std::move(m)); p_->tel.blocksOk++;
        p_->lastMessage = p_->modem.timeSec(); p_->tel.dataValid = true;
        if (p_->log) p_->log(railwaySummary(p_->tel, p_->protocol == railway::Protocol::Tdcs ? "TDCS" : "KLW"));
    }
    const double now = p_->modem.timeSec();
    if (!found.empty() || now - p_->lastPublish >= 0.25) {
        p_->lastPublish = now; p_->tel.timeSec = now; p_->tel.levelDb = p_->modem.levelDb();
        p_->tel.cfoHz = p_->modem.frequencyErrorHz();
        p_->tel.blocks = p_->modem.syncs(); p_->tel.blocksBad = p_->modem.failures();
        p_->tel.state = now - p_->lastMessage < 30 ? 2 : 1; p_->tel.seq++;
    }
}
bool RailwayReceiver::telemetry(RailwayTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lock(p_->mu); if (p_->tel.seq <= lastSeq) return false; out = p_->tel; return true;
}
std::string RailwayReceiver::saveResults() const {
    std::lock_guard<std::mutex> lock(p_->mu); std::string s = "cir1\n";
    const char* h = "0123456789abcdef";
    for (const auto& m : p_->tel.messages) {
        s += std::to_string(m.wallTime) + '\t' + std::to_string(m.correctedBits) + '\t';
        for (auto b : m.raw) { s += h[b >> 4]; s += h[b & 15]; } s += '\n';
    }
    return s;
}
bool RailwayReceiver::loadResults(const std::string& blob) {
    if (blob.compare(0, 5, "cir1\n") != 0) return false;
    std::istringstream input(blob.substr(5)); std::string line; std::vector<railway::Message> messages;
    while (std::getline(input, line) && messages.size() < 400) {
        std::istringstream row(line); int64_t wall; int corrected; std::string raw, extra;
        if (!(row >> wall >> corrected >> raw) || (row >> extra) || raw.size() > 514 || raw.size() % 2 || raw.find_first_not_of("0123456789abcdef") != std::string::npos || corrected < 0 || corrected > 258) return false;
        std::vector<uint8_t> frame;
        for (size_t i = 0; i < raw.size(); i += 2) frame.push_back(uint8_t(std::stoul(raw.substr(i, 2), nullptr, 16)));
        railway::Message m;
        if (!railway::parseFrame(frame, m) || m.protocol != p_->protocol) return false;
        m.wallTime = wall; m.correctedBits = corrected; messages.push_back(std::move(m));
    }
    std::lock_guard<std::mutex> lock(p_->mu);
    p_->tel.messages = std::move(messages); p_->tel.dataValid = !p_->tel.messages.empty(); p_->tel.seq++; return true;
}
void RailwayReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lock(p_->mu); p_->log = std::move(cb); }
} // namespace dect2
