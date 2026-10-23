// Mercury monitor: what the decoded frames say (see mercury_proto.h), as a passive listener hears both stations of a link.
//
// Connects: a CALL names the caller and a CRC of the called station, the ACCEPT the session's nonce and the caller's start rung. The
// called callsign is never on the air in full: it is named when a callsign heard elsewhere (a CQ, a CALL of its own, the user's list)
// has that CRC. With both callsigns the session's CRC seed is computed as the stations compute it; without them it is learned from the
// frames themselves (an LDPC-decoded frame whose CRC differs from the computed one by the same value as other frames of the session).
// Inside a session every frame's sender is told apart by its frequency offset when the two radios differ by more than 1.5 Hz, and
// otherwise by the turns (a gap of more than 0.6 s between bursts is a new keydown, and keydowns alternate). Data rounds are
// reassembled per direction: the Reed-Solomon blocks rebuilt from any K of their pieces, checked by their keyed CRC-32, then shown.
// A session the connect marks as encrypted is listed, its payload never decoded ("encrypted").
// Broadcast: messages are shown; the RaptorQ file carousel is reassembled into files that the user may view or save (nothing is
// written or opened by itself).
#pragma once
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_tel.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace mercury {

class Monitor {
public:
    Monitor();
    ~Monitor();
    // a burst from the receiver; utc of its start, its start in signal seconds and its length
    void onFrame(const OfdmFrame& f, double utc, double startSec, double airSec);
    void setKnownCalls(const std::vector<std::string>& calls);   // callsigns the user expects (to name called stations by their CRC)
    void clear();
    void fill(MercuryTelemetry& t) const;                         // frames, sessions, stations, files, messages, counters
    uint64_t version() const;                                     // changes whenever fill() would give something new
    bool fileBytes(int id, std::vector<uint8_t>& data, std::string& name) const;
    bool sessionBytes(int key, int dir, std::vector<uint8_t>& data) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace mercury
} // namespace dect2
