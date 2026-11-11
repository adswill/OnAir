// Hosting an EchoLink conference: several stations connected to this one at once. Each station is its own Session (el_client.h: SDES
// connect and keep-alive, BYE, time-out), routed by its address and ports. Every 80 ms the audio of everyone talking is decoded, mixed and
// sent to each station without its own voice (so two people talking at once both reach the rest), and the host hears the mix of all.
// Chat from one station is passed on to the others. The info text every station sees lists who is connected and is re-sent when that
// changes, as EchoLink conference servers do. Like Session it owns no sockets and takes the caller's clock, so the tests drive it.
// The conference behaviour follows SvxLink's ModuleEchoLink (several Qso objects, a list of connected stations in the info text, chat
// passed on; GPL-2.0-or-later, Tobias Blomberg SM0SVX); the code is OnAir's own.
#pragma once
#include "dect2/echolink/el_client.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace echolink {

constexpr int kMixBlockSamples = kFramesPerPacket * kGsmFrameSamples;   // 640 samples, 80 ms
constexpr int kMixBlockMs = 80;
constexpr int kConferenceMaxDefault = 10;

struct ConferenceMember {
    std::string call, name, ip;
    int audioPort = 0, ctrlPort = 0;
    bool connecting = false;     // the host called it and it has not answered yet
    bool talking = false;        // its audio was in the last mix block
    uint64_t packetsIn = 0, lost = 0;
};

class Conference {
public:
    Conference();
    void setSend(Session::SendFn f) { send_ = std::move(f); }
    void setLocal(const std::string& call, const std::string& name, const std::string& info);
    void setMaxStations(int n) { max_ = n < 1 ? 1 : n; }
    void setAcceptIncoming(bool on) { accept_ = on; }

    bool connect(const std::string& ip, int remoteAudioPort, int64_t nowMs);   // the host adds a station
    bool disconnect(const std::string& call);                                  // BYE to one station
    void disconnectAll();
    void onCtrl(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs);
    void onAudio(const std::string& ip, int srcPort, const uint8_t* p, size_t n, int64_t nowMs);
    void tick(int64_t nowMs);                     // keep-alives and time-outs of every station, then the mixer, one block per 80 ms

    // the host's own voice at 8 kHz (sent to everyone while setLocalTalk(true)) and what the host hears (everyone mixed)
    void setLocalTalk(bool on);
    void pushLocal(const int16_t* pcm, int n);
    int pullLocal(int16_t* out, int n);
    int localBuffered() const { return (int)out_.size(); }
    bool sendChat(const std::string& message);    // to every station

    int size() const { return (int)m_.size(); }
    int connectedCount() const;
    std::vector<ConferenceMember> members() const;
    std::string infoText() const;                 // what the stations see
    bool anyTalking() const;
    std::vector<std::string> takeChat();          // "CALL>message", from every station
    std::vector<std::string> takeEvents();
    uint64_t packetsOut() const { return pktOut_; }
    uint64_t mixBlocks() const { return blocks_; }

private:
    struct Member {
        Session s;
        GsmDecoder dec;
        GsmEncoder enc;
        std::deque<int16_t> q;   // decoded audio waiting for the mixer
        int64_t lastAudio = -1;
        bool talking = false;
        std::string lastCall;
    };
    Member* find(const std::string& ip, int port, bool ctrl);
    Member* add(int64_t nowMs);
    void collect(Member& m);
    void pruneIdle();
    void infoChanged();
    void mixBlock(int64_t nowMs);
    Session::SendFn send_;
    std::string call_, name_, info_;
    int max_ = kConferenceMaxDefault;
    bool accept_ = true;
    std::vector<std::unique_ptr<Member>> m_;
    std::deque<int16_t> local_, out_;
    bool localTalk_ = false;
    int64_t nextMix_ = -1;
    uint64_t pktOut_ = 0, blocks_ = 0;
    std::vector<std::string> chat_, events_;
    std::string lastInfo_;
};

} // namespace echolink
} // namespace dect2
