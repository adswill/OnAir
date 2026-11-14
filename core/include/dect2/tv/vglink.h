// The link between the Digital video generator (onair-videogen, its own program) and OnAir (issue #458), version 1 (docs/VIDEOGEN-LINK.md).
//
// Plain RTP (RFC 3550), no connection and no buffering in the link. Two transports (vglink_transport.h):
//  - local, always on, no ports: the named pipe \\.\pipe\onair-videogen-<user> (Windows, message mode) or the Unix datagram socket
//    <runtime dir>/onair-videogen.sock (Linux, macOS). One pipe message / datagram = one RTP or RTCP packet; RTP and RTCP share it,
//    told apart by the packet type (RFC 5761). OnAir starts its generator on it.
//  - network, when OnAir's "Listen on the network" box is ticked: an even UDP port for RTP (kVgLinkDefaultPort, 127.0.0.1 or every
//    network card) and the next one for RTCP, for generators on other computers (or started with --onair host:port).
//  - the stream, generator -> OnAir: RTP payload type 33 (MPEG-2 TS, RFC 2250 / RFC 3551), 7 TS packets per datagram (1316 bytes, under
//    a 1500 byte MTU), 90 kHz time stamps, paced by the generator's mux. OnAir counts lost and late packets by the sequence numbers;
//    a late packet is dropped (the transmitter's queue only goes forward).
//  - the control, both ways: compound RTCP packets (RR + SDES + APP "OAVG", the message in the APP subtype, "key=value" lines as its
//    data; unknown keys are ignored, so either side can add some). The generator sends its State to OnAir's RTCP port about once a
//    second and at once on a change; OnAir answers each State with a Reply to the address it came from (so the back channel needs no
//    port on the generator's side and passes NAT), and sends every Reply again each second. Everything in them is a state, not an event:
//    a lost datagram only delays it. Raise and Quit are counters in the Reply (the generator acts when one goes up), a new stream is a
//    counter in the State (OnAir then empties the slot's queue, as a starting generator did in #494).
//  - a generator that has not been heard from for kVgLinkTimeoutSec is gone (its slot's rate goes to 0); the generator calls OnAir gone
//    when no Reply came for 3 s. Leaving is an RTCP BYE (sent three times): reason "closed" means the user closed the generator window.
//
//   State  (generator -> OnAir): proto, name, slot (the one it asks for), token (the one OnAir started it with), rate (its mux rate,
//          0: not sending), stream (counts its starts), easy, programmes / p0.. ("sid name")
//   Reply  (OnAir -> generator): proto, slot (the one it got), raise, quit (counters), and the transmitter's info (vgTxInfoKv)
//   Refused (OnAir -> generator): error
//
// OnAir side: VgLinkServer puts each generator's stream into the hub of videogen.h (generatorPush / generatorSetRate / generatorClear
// of slot N), where the transmitter has always read it as "generator:N". Generator side: VgLinkClient, sending from the mux thread.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2::vgen {

constexpr int kVgLinkVersion = 1;
constexpr int kVgLinkDefaultPort = 5460;   // RTP; RTCP on 5461 (5454 is SDRconnect's)
constexpr double kVgLinkTimeoutSec = 5;

// the RTCP APP subtypes (Ts: an RTP packet; Bye: the RTCP BYE)
enum class VgMsg : uint8_t { Ts = 0, State = 1, Reply = 2, Refused = 3, Bye = 31 };

// ---- packets (pure, tested) ----
struct VgPacket {
    VgMsg type = VgMsg::Ts;
    uint32_t ssrc = 0;
    uint16_t seq = 0;              // RTP
    std::vector<uint8_t> data;     // Ts: whole TS packets; APP: its text; Bye: the reason
};
// a compound RTCP packet: RR (no reports) + SDES (CNAME, and NAME when given) + APP "OAVG"; Bye: RR + BYE with text as its reason
std::vector<uint8_t> vgRtcp(VgMsg type, const std::string& text, uint32_t ssrc, const std::string& cname, const std::string& name = std::string());
// up to 7 TS packets as one RTP packet
std::vector<uint8_t> vgRtp(const uint8_t* ts, size_t n, uint16_t seq, uint32_t ts90k, uint32_t ssrc);
// false: not an RTP packet of payload type 33 (or broken)
bool vgParseRtp(const uint8_t* d, size_t n, VgPacket& out);
// our APP packets and BYEs in a compound RTCP packet; other RTCP is skipped; false: broken (nothing taken)
bool vgParseRtcp(const uint8_t* d, size_t n, std::vector<VgPacket>& out);

using VgKv = std::map<std::string, std::string>;
std::string vgKvText(const VgKv& kv);              // "a=1\nb=x\n" (newlines in values become spaces)
VgKv vgKvParse(const std::string& text);
double vgKvNum(const VgKv& kv, const char* key, double def = 0);
std::string vgKvStr(const VgKv& kv, const char* key, const std::string& def = std::string());

// What the transmitter tells the generators (TxInfo), set by OnAir's transmit panel
struct VgTxInfo {
    bool running = false;          // the transmitter is on air (or in its test run)
    bool digitalTv = false;        // a mode that carries a transport stream
    double channelBps = 0;         // the channel's transport-stream rate (0: unknown, e.g. no TV mode chosen)
    std::string system;            // "DVB-T", "DVB-T2", "DVB-S2" ...
    std::string params;            // "8 MHz, 64-QAM, code rate 2/3, guard 1/4": shown to the user
    std::vector<int> slots;        // the generators the transmitter's multiplex uses now (empty: none)
};
// The rate generator `slot` may use: 97 % of the channel, shared equally by the generators in the multiplex (the whole channel's
// 97 % when it is not one of them yet, as a hint). 0 when the channel is unknown.
double vgUsableBps(const VgTxInfo& t, int slot);
VgKv vgTxInfoKv(const VgTxInfo& t, int slot);
VgTxInfo vgTxInfoFromKv(const VgKv& kv, double& usableBps);

// One connected generator, as OnAir sees it
struct VgPeer {
    int slot = 0;
    std::string name, host, token;
    bool easy = true;
    double rateBps = 0;            // > 0 while it sends (Start .. Stop)
    uint64_t packets = 0;          // received
    uint64_t lost = 0, late = 0;   // RTP datagrams missing / out of order (dropped)
    std::vector<std::string> programmes;   // "sid name" of each programme of its multiplex (from Status)
    bool local = true;             // over the pipe (false: UDP from host)
};

// ---- OnAir side ----
class VgLinkServer {
public:
    VgLinkServer();
    ~VgLinkServer();   // stops
    // listens on the local pipe (localName: "" = this user's) and, with network, on UDP port (RTP) and port + 1 (RTCP) too (port 0:
    // a free pair). false with err when one of them could not start (in use ...); the other one runs then.
    bool start(bool network, int port, bool anyAddress, std::string& err, const std::string& localName = std::string());
    void stop();                                               // forgets every generator, joins its thread, empties the slots
    bool listening() const;                  // on one of them
    bool listeningLocal() const;
    int port() const;                        // the RTP port (0: not on the network)
    std::string localName() const;           // the pipe / socket
    std::vector<VgPeer> peers() const;
    void setTxInfo(const VgTxInfo& t);       // in every Reply; sent at once when it changes
    bool raise(const std::string& token);    // asks the generator started with this token to come to the front; false: none has it
    // the token OnAir started its own generator with: that generator's Reply says ours=1, so only it follows the transmitter's rate (#458)
    void setOwnToken(const std::string& token);
    bool quit(const std::string& token);     // asks it to quit
    bool startStreaming(const std::string& token);   // asks it to start streaming to the transmitter in Easy mode (#458: Start in the Transmit panel)
    void quitAll();
    // the user closed the generator window (BYE "closed"): its token. Called on the link thread.
    std::function<void(const std::string& token)> onBye;

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};
VgLinkServer& vgLinkServer();   // the one OnAir runs (the transmit panel and the Tools menu use it)

// ---- generator side ----
class VgLinkClient {
public:
    VgLinkClient();
    ~VgLinkClient();   // stops
    // Talks to OnAir at host:port (UDP: RTP; RTCP port + 1) until stop. name: the generator's name shown in OnAir; wantSlot: the slot
    // (generator number) it asks for; token: the one OnAir started it with ("" when started by itself).
    void start(const std::string& host, int port, const std::string& name, int wantSlot, const std::string& token);
    // the same over OnAir's local pipe / socket on this computer ("": this user's)
    void startLocal(const std::string& localName, const std::string& name, int wantSlot, const std::string& token);
    void stop();                             // BYE "stop": OnAir frees the slot at once
    bool connected() const;                  // a Reply came in the last 3 s
    int slot() const;                        // the slot OnAir gave it (0: not connected)
    std::string status() const;              // "linked to OnAir on this computer as generator:1", "... at host:5460 ...", "OnAir not found at ...", "refused: ..."
    VgTxInfo txInfo(double& usableBps) const;   // the last TxInfo (running false while not connected)
    bool ours() const;                       // OnAir's Reply says OnAir started this generator (its token is the one OnAir spawned it with)
    void setStatus(bool easy, const std::vector<std::string>& programmes);   // in the State
    void startStream(double muxBps);         // a new stream at this mux rate (OnAir empties the slot's queue)
    void setRate(double muxBps);
    void stopStream();
    // n whole packets as RTP; false when OnAir is not there (they are dropped). Never waits: the caller paces.
    bool sendTs(const uint8_t* packets, size_t n);
    void bye();                              // the window was closed by the user (BYE "closed"): OnAir clears its "open" setting
    // OnAir asked: taken (and cleared) by the window each frame
    bool takeRaise();
    bool takeStart();                        // OnAir asked: start streaming in Easy mode
    bool takeQuit();
    uint64_t sentPackets() const;

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::vgen
