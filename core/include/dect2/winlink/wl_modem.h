// Winlink frames on OnAir's modems: the link frames (wl_link.h) rendered as IQ by the packet generator (1200 baud AFSK, 9600 baud
// G3RUH, FM) or the ARDOP generator (upper sideband audio as IQ), and the transmit generator of the "winlink" transmit mode that sends
// one burst of them and ends. The receiving side is the packet / ARDOP receiver with its frame tap (packet_rx.h, ardop_rx.h).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/packet/packet_ax25.h"
#include "dect2/winlink/wl_link.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
class TxGenerator;
struct TxRequest;

namespace winlink {

enum WlModem { kModemPacket1200 = 1, kModemPacket9600 = 2, kModemArdop = 3 };

// IQ of one burst at rate: the AX.25 frames back to back (gapSec between them), FM, the carrier tuneOffsetHz below the centre
// (packetTuning) plus cfoHz; snrDb >= 99: no noise
std::vector<cf32> renderAx25Burst(const std::vector<ax25::Frame>& frames, int baud, double rate, double snrDb = 99, double cfoHz = 0, double gapSec = 0.05);
// IQ of ARDOP frames, the dial tuneOffsetHz below the centre (ardopTuning) plus cfoHz, gapSec of quiet between frames
std::vector<cf32> renderArdopBurst(const std::vector<ArdopLinkFrame>& frames, double rate, double snrDb = 99, double cfoHz = 0, double gapSec = 0.15, uint32_t seed = 1);

// TxRequest::payload of the "winlink" transmit mode: modem byte, then each frame as two length bytes (big endian) and its bytes
// (AX.25: the frame without FCS; ARDOP: type, session ID, body)
std::string winlinkTxPayloadAx25(int modem, const std::vector<ax25::Frame>& frames);
std::string winlinkTxPayloadArdop(const std::vector<ArdopLinkFrame>& frames);
bool parseWinlinkTxPayload(const std::string& payload, int& modem, std::vector<ax25::Frame>& ax, std::vector<ArdopLinkFrame>& ar);

// The transmit generator: the burst of the payload once, then finished(). Empty or unreadable payload: nullptr with err.
std::unique_ptr<TxGenerator> makeWinlinkTxGenerator(const TxRequest& r, double rateHz, std::string& err);

// Seconds on the air of a burst (for the link timers)
double ax25BurstSec(const std::vector<ax25::Frame>& frames, int baud, double gapSec = 0.05);

} // namespace winlink
} // namespace dect2
