// Hosting an EchoLink conference: see el_conference.h.
#include "dect2/echolink/el_conference.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace echolink {

namespace {
constexpr size_t kMemberMaxQueue = 8000;     // 1 s of a station's audio waiting for the mixer; older is dropped
constexpr size_t kOutMaxSamples = 16000;     // 2 s for the host's speaker
constexpr int kTailMs = 2 * kMixBlockMs;     // a station silent this long: the rest of its audio is mixed, padded with silence
inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
} // namespace

Conference::Conference() {}

void Conference::setLocal(const std::string& call, const std::string& name, const std::string& info) {
    call_ = upperCall(call);
    name_ = name;
    info_ = info;
    for (auto& m : m_) m->s.setLocal(call_, name_, infoText());
    infoChanged();
}

int Conference::connectedCount() const {
    int n = 0;
    for (const auto& m : m_) if (m->s.state() == QsoState::Connected) n++;
    return n;
}

std::string Conference::infoText() const {
    std::string t = info_.empty() ? std::string("OnAir conference") : info_;
    t += "\nConference " + call_ + "\n";
    std::vector<std::string> rows;
    for (const auto& m : m_) {
        if (m->s.state() != QsoState::Connected) continue;
        std::string r = m->s.peerCall();
        if (!m->s.peerName().empty()) r += "  " + m->s.peerName();
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end());
    t += "Connected stations (" + std::to_string(rows.size()) + "):";
    for (const std::string& r : rows) t += "\n" + r;
    return t;
}

void Conference::infoChanged() {
    const std::string t = infoText();
    if (t == lastInfo_) return;
    lastInfo_ = t;
    for (auto& m : m_) m->s.setInfo(t);
}

std::vector<ConferenceMember> Conference::members() const {
    std::vector<ConferenceMember> v;
    for (const auto& m : m_) {
        ConferenceMember c;
        c.call = m->s.peerCall(); c.name = m->s.peerName(); c.ip = m->s.peerIp();
        c.audioPort = m->s.peerAudioPort(); c.ctrlPort = m->s.peerCtrlPort();
        c.connecting = m->s.state() == QsoState::Connecting;
        c.talking = m->talking;
        c.packetsIn = m->s.packetsIn(); c.lost = m->s.packetsLost();
        v.push_back(c);
    }
    return v;
}

bool Conference::anyTalking() const {
    for (const auto& m : m_) if (m->talking) return true;
    return false;
}

Conference::Member* Conference::add(int64_t) {
    std::unique_ptr<Member> m(new Member);
    m->s.setLocal(call_, name_, infoText());
    m->s.setAcceptIncoming(true);
    m->s.setSend([this](bool ctrl, const std::string& ip, int port, const std::vector<uint8_t>& pkt) { if (send_) send_(ctrl, ip, port, pkt); });
    m_.push_back(std::move(m));
    return m_.back().get();
}

// ctrl: a control-port packet from ip:port. The exact port first (two stations may share one public address), then a station the host is
// still calling at that address (it may answer from another port), and for audio the one connected station at that address.
Conference::Member* Conference::find(const std::string& ip, int port, bool ctrl) {
    for (auto& m : m_) {
        if (m->s.state() == QsoState::Idle || m->s.peerIp() != ip) continue;
        if ((ctrl ? m->s.peerCtrlPort() : m->s.peerAudioPort()) == port) return m.get();
    }
    Member* only = nullptr;
    int count = 0;
    for (auto& m : m_) {
        if (m->s.state() == QsoState::Idle || m->s.peerIp() != ip) continue;
        if (ctrl && m->s.state() == QsoState::Connecting) return m.get();
        if (!ctrl && m->s.state() == QsoState::Connected) { only = m.get(); count++; }
    }
    return count == 1 ? only : nullptr;
}

bool Conference::connect(const std::string& ip, int remoteAudioPort, int64_t nowMs) {
    if (ip.empty() || call_.empty()) return false;
    for (auto& m : m_) if (m->s.peerIp() == ip && m->s.peerAudioPort() == remoteAudioPort) return false;   // already here
    if ((int)m_.size() >= max_) { events_.push_back("the conference is full (" + std::to_string(max_) + " stations)"); return false; }
    Member* m = add(nowMs);
    if (!m->s.connect(ip, remoteAudioPort, nowMs)) { m_.pop_back(); return false; }
    for (std::string& e : m->s.takeEvents()) events_.push_back(std::move(e));
    return true;
}

bool Conference::disconnect(const std::string& call) {
    const std::string c = upperCall(call);
    for (auto& m : m_) {
        if (m->s.peerCall() != c) continue;
        m->s.disconnect();
        for (std::string& e : m->s.takeEvents()) events_.push_back(std::move(e));
        pruneIdle();
        return true;
    }
    return false;
}

void Conference::disconnectAll() {
    for (auto& m : m_) {
        m->s.disconnect();
        for (std::string& e : m->s.takeEvents()) events_.push_back(std::move(e));
    }
    m_.clear();
    lastInfo_.clear();
    out_.clear();
}

void Conference::pruneIdle() {
    m_.erase(std::remove_if(m_.begin(), m_.end(), [](const std::unique_ptr<Member>& m) { return m->s.state() == QsoState::Idle; }), m_.end());
    infoChanged();   // sends only when the list really changed
}

// What one station's session produced: audio for the mixer, chat passed on to the others, events
void Conference::collect(Member& m) {
    for (const RxPacket& p : m.s.takeAudio()) {
        for (int f = 0; f < p.nframes; f++) {
            int16_t pcm[kGsmFrameSamples];
            if (!m.dec.decode(p.frames[f], pcm)) std::memset(pcm, 0, sizeof pcm);
            m.q.insert(m.q.end(), pcm, pcm + kGsmFrameSamples);
        }
    }
    while (m.q.size() > kMemberMaxQueue) m.q.pop_front();
    for (std::string& c : m.s.takeChat()) {
        const size_t gt = c.find('>');
        const std::string who = gt == std::string::npos ? m.s.peerCall() : c.substr(0, gt);
        const std::string msg = gt == std::string::npos ? c : c.substr(gt + 1);
        for (auto& o : m_) {
            if (o.get() == &m || o->s.state() != QsoState::Connected || !send_) continue;
            send_(false, o->s.peerIp(), o->s.peerAudioPort(), makeChatPacket(who, msg));
        }
        if (chat_.size() < 256) chat_.push_back(std::move(c));
    }
    for (std::string& e : m.s.takeEvents()) events_.push_back(std::move(e));
}

void Conference::onCtrl(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs) {
    Member* m = find(ip, srcPort, true);
    if (!m) {
        if (!isRtcpSdes(p, n)) return;                       // a BYE or anything else from a stranger
        std::string nameItem, call, name;
        const bool named = sdesItem(p, n, 2, nameItem) && splitSdesName(nameItem, call, name);
        if (!named) return;
        if (!accept_ || (int)m_.size() >= max_) {
            if (send_) send_(true, ip, srcPort, makeBye());
            events_.push_back("refused a call from " + call + (accept_ ? ": the conference is full" : ""));
            return;
        }
        for (auto& o : m_) if (o->s.peerCall() == call && o->s.state() == QsoState::Connected) {   // the same station from a new port
            o->s.disconnect();
            o->s.takeEvents();
        }
        pruneIdle();
        m = add(nowMs);
    }
    const QsoState before = m->s.state();
    m->s.onCtrl(ip, srcPort, p, n, nowMs);
    collect(*m);
    if (m->s.state() != before) pruneIdle();
}

void Conference::onAudio(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs) {
    Member* m = find(ip, srcPort, false);
    if (!m) return;
    const uint64_t in = m->s.packetsIn();
    m->s.onAudio(ip, srcPort, p, n, nowMs);
    if (m->s.packetsIn() != in) m->lastAudio = nowMs;
    collect(*m);
}

void Conference::setLocalTalk(bool on) {
    if (localTalk_ && !on && !local_.empty() && local_.size() % kMixBlockSamples)
        local_.insert(local_.end(), kMixBlockSamples - local_.size() % kMixBlockSamples, 0);
    if (!localTalk_ && on) local_.clear();
    localTalk_ = on;
}

void Conference::pushLocal(const int16_t* pcm, int n) {
    if (!localTalk_ || n <= 0) return;
    local_.insert(local_.end(), pcm, pcm + n);
    while (local_.size() > kMemberMaxQueue * 4) local_.pop_front();
}

int Conference::pullLocal(int16_t* out, int n) {
    int k = 0;
    while (k < n && !out_.empty()) { out[k++] = out_.front(); out_.pop_front(); }
    return k;
}

bool Conference::sendChat(const std::string& message) {
    if (message.empty() || !send_) return false;
    bool any = false;
    for (auto& m : m_) {
        if (m->s.state() != QsoState::Connected) continue;
        send_(false, m->s.peerIp(), m->s.peerAudioPort(), makeChatPacket(call_, message));
        any = true;
    }
    return any;
}

void Conference::tick(int64_t nowMs) {
    bool changed = false;
    for (auto& m : m_) {
        const QsoState before = m->s.state();
        m->s.tick(nowMs);
        collect(*m);
        if (m->s.state() != before) changed = true;
    }
    if (changed) pruneIdle();
    if (nextMix_ < 0 || nowMs - nextMix_ > 5 * kMixBlockMs) nextMix_ = nowMs;   // first block, or the caller stalled: no catch-up burst
    while (nowMs >= nextMix_) { mixBlock(nowMs); nextMix_ += kMixBlockMs; }
}

// One 80 ms block: what each talker said, mixed for every station without its own voice, and for the host
void Conference::mixBlock(int64_t nowMs) {
    blocks_++;
    const size_t N = m_.size();
    std::vector<std::vector<int16_t>> part(N);
    std::vector<int16_t> mine;
    for (size_t i = 0; i < N; i++) {
        Member& m = *m_[i];
        m.talking = false;
        if (m.s.state() != QsoState::Connected || m.q.empty()) continue;
        const bool tail = m.lastAudio >= 0 && nowMs - m.lastAudio >= kTailMs;
        if (m.q.size() < (size_t)kMixBlockSamples && !tail) continue;   // wait for the rest of the block (network jitter)
        part[i].assign((size_t)kMixBlockSamples, 0);
        const size_t take = std::min(m.q.size(), (size_t)kMixBlockSamples);
        for (size_t k = 0; k < take; k++) { part[i][k] = m.q.front(); m.q.pop_front(); }
        m.talking = true;
    }
    if (localTalk_ || !local_.empty()) {
        if (local_.size() >= (size_t)kMixBlockSamples) {
            mine.assign(local_.begin(), local_.begin() + kMixBlockSamples);
            local_.erase(local_.begin(), local_.begin() + kMixBlockSamples);
        }
    }
    // the host's speaker: every station
    {
        std::vector<int32_t> acc((size_t)kMixBlockSamples, 0);
        bool any = false;
        for (size_t i = 0; i < N; i++) {
            if (part[i].empty()) continue;
            any = true;
            for (int k = 0; k < kMixBlockSamples; k++) acc[(size_t)k] += part[i][(size_t)k];
        }
        if (any) {
            for (int k = 0; k < kMixBlockSamples; k++) out_.push_back(sat16(acc[(size_t)k]));
            while (out_.size() > kOutMaxSamples) out_.pop_front();
        }
    }
    // each station: everyone else and the host
    for (size_t j = 0; j < N; j++) {
        Member& m = *m_[j];
        if (m.s.state() != QsoState::Connected) continue;
        std::vector<int32_t> acc((size_t)kMixBlockSamples, 0);
        bool any = false;
        for (size_t i = 0; i < N; i++) {
            if (i == j || part[i].empty()) continue;
            any = true;
            for (int k = 0; k < kMixBlockSamples; k++) acc[(size_t)k] += part[i][(size_t)k];
        }
        if (!mine.empty()) { any = true; for (int k = 0; k < kMixBlockSamples; k++) acc[(size_t)k] += mine[(size_t)k]; }
        if (!any) continue;
        uint8_t frames[kFramesPerPacket][kGsmFrameBytes];
        for (int f = 0; f < kFramesPerPacket; f++) {
            int16_t pcm[kGsmFrameSamples];
            for (int k = 0; k < kGsmFrameSamples; k++) pcm[k] = sat16(acc[(size_t)(f * kGsmFrameSamples + k)]);
            m.enc.encode(pcm, frames[f]);
        }
        if (m.s.sendAudio(frames)) pktOut_++;
    }
}

std::vector<std::string> Conference::takeChat() { std::vector<std::string> v; v.swap(chat_); return v; }
std::vector<std::string> Conference::takeEvents() { std::vector<std::string> v; v.swap(events_); return v; }

} // namespace echolink
} // namespace dect2
