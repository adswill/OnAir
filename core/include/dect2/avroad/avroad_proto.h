// Pure protocol helpers of the aviation, rail and road modes: parsers and checks, tested on their own.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::avroad {

// ---- AAR end of train
struct EotFrame { int chaining = 0, batteryCond = 0, messageType = 0; uint32_t unitAddress = 0; int pressurePsi = 0, batteryPercent = 0; int valve = 0, confirm = 0, turbine = 0, motion = 0, markerBattery = 0, markerLight = 0; };
bool eotCheck(uint8_t* body63, int* fixed);       // d0 .. d44, c0 .. c17 as received; repairs up to two bits; false if it is not a codeword
EotFrame eotParse(const uint8_t* d45);
std::string eotText(const EotFrame& f, bool hot);

} // namespace dect2::avroad
