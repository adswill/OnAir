// Telemetry parsing for the Smart CubeSat decoder: a validated frame (AX.25, AX100 / CSP, AO-40 FEC, text beacon) turned into named fields.
// FUNcube-1 (AO-73) / FUNcube-2 / Nayif-1 realtime telemetry follows the frame definitions of gr-satellites (python/telemetry/funcube.py,
// Daniel Estevez EA4GPZ, GPL-3.0-or-later) and the AMSAT-UK / FUNcube data warehouse conversions it uses.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

struct TelField {
    std::string group;      // "EPS", "RF", ... ("" none)
    std::string name;
    double num = 0;
    bool numeric = false;
    std::string text;       // the value as shown ("8.12 V", "deployed")
};

struct TelResult {
    std::string kind;       // "FUNcube realtime WO3", "text beacon", ...
    std::string satellite;  // when the frame says (FUNcube satellite id), else ""
    std::vector<TelField> fields;
};

// The 256 byte frame of the AO-40 FEC framing used by FUNcube-1 / AO-73, FUNcube-2 and Nayif-1. False when the header is not a FUNcube one.
bool parseFuncube(const std::vector<uint8_t>& frame, TelResult& out);
// The text of a frame when it is a plain ASCII beacon (a long enough run of printable characters), else "".
std::string beaconText(const std::vector<uint8_t>& info);
// What can be said about any decoded frame: FUNcube for the AO-40 frames, the text, the CSP header. `framing` is cubesat::Framing or kFramingAo40 / kFramingText.
bool parseTelemetry(int framing, const std::vector<uint8_t>& data, const std::vector<uint8_t>& payload, TelResult& out);

constexpr int kFramingAo40 = 100, kFramingText = 101, kFramingCw = 102;

} // namespace cubesat
} // namespace dect2
