#include "dect2/session/session.h"
#include <map>
#include <mutex>

namespace dect2::sess {

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Call: return "call";
    case Kind::Thread: return "thread";
    case Kind::Track: return "track";
    case Kind::Pass: return "pass";
    case Kind::Programme: return "programme";
    case Kind::Device: return "device";
    case Kind::Reception: return "reception";
    case Kind::Observation: return "observation";
    }
    return "session";
}

const char* kindEventType(Kind k) {
    switch (k) {
    case Kind::Call: return "session.call";
    case Kind::Thread: return "session.thread";
    case Kind::Track: return "session.track";
    case Kind::Pass: return "session.pass";
    case Kind::Programme: return "session.programme";
    case Kind::Device: return "session.device";
    case Kind::Reception: return "session.reception";
    case Kind::Observation: return "session.observation";
    }
    return "session.call";
}

const char* kindTitle(Kind k) {
    switch (k) {
    case Kind::Call: return "Calls";
    case Kind::Thread: return "Threads";
    case Kind::Track: return "Tracks";
    case Kind::Pass: return "Passes";
    case Kind::Programme: return "Programmes";
    case Kind::Device: return "Devices";
    case Kind::Reception: return "Services";
    case Kind::Observation: return "Heard";
    }
    return "Sessions";
}

const char* verdictName(Verdict v) {
    switch (v) {
    case Verdict::Clear: return "clear";
    case Verdict::Encrypted: return "encrypted";
    case Verdict::Unreadable: return "unreadable";
    case Verdict::Test: return "test";
    }
    return "clear";
}

const char* linkTypeName(LinkType t) {
    switch (t) {
    case LinkType::Audio: return "audio";
    case LinkType::Image: return "image";
    case LinkType::Message: return "message";
    case LinkType::Position: return "position";
    case LinkType::Transcript: return "transcript";
    case LinkType::Iq: return "iq";
    case LinkType::Telemetry: return "telemetry";
    }
    return "audio";
}

struct IdAllocator::Impl {
    std::mutex mu;
    std::map<std::pair<uint32_t, uint16_t>, uint32_t> last;   // (day, mode) -> last sequence
};

IdAllocator::IdAllocator() : p_(new Impl) {}
IdAllocator::~IdAllocator() = default;

uint64_t IdAllocator::next(int64_t startWallMs, uint16_t mode) {
    const uint32_t day = dayOfWall(startWallMs);
    std::lock_guard<std::mutex> lk(p_->mu);
    uint32_t& s = p_->last[{day, mode}];
    if (s == 0xFFFFFFFFu) s = 0;   // 4 billion sessions of one mode in one day: wrap rather than overflow into the mode bits
    return makeId(day, mode, ++s);
}

void IdAllocator::raise(uint32_t day, uint16_t mode, uint32_t seq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    uint32_t& s = p_->last[{day, mode}];
    if (seq > s) s = seq;
}

void IdAllocator::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->last.clear();
}

}  // namespace dect2::sess
