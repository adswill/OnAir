// LBJ message layer: numeric POCSAG pages on the railway warning channel (821.2375 MHz) turned into train number, speed,
// kilometre post, direction and (in the longer messages) locomotive and route. Original implementation; the field layout comes
// from the public descriptions and the recordings listed in docs/lbj.md.
#pragma once
#include "lbj_pocsag.h"
#include "lbj_tel.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2::lbj {

constexpr double kBaud = 1200.0;
constexpr double kDefaultMhz = 821.2375;

constexpr uint32_t kAddrBasic = 1234000;       // train number, speed, kilometre post
constexpr uint32_t kAddrBasicAlt = 1233999;
constexpr uint32_t kAddrDetail = 1234002;      // locomotive and route (long messages)
constexpr uint32_t kAddrDetailAlt = 1234001;
constexpr uint32_t kAddrTime = 1234008;

bool isLbjAddress(uint32_t ric);

// What one train has told us in the last seconds (the long message arrives right after the short one).
struct LbjSessionState {
    LbjMessage pendingBasic, pendingDetail;
    double basicSec = -1e9, detailSec = -1e9;
    uint64_t emptyFrames = 0;                  // basic messages with every field blank ("----- --- -----")
    uint64_t rejected = 0;                     // LBJ-addressed pages that failed the plausibility checks
};

// Parses one page; true (and `out`) when it carries a message to show.
bool parsePage(const Page& p, double timeSec, LbjSessionState& st, LbjMessage& out);

// The basic message text as the transmitter builds it: train(5), a space, speed km/h (3, right aligned), a space, km post in 0.1 km (5, right aligned): 15 digits = 3 codewords.
std::string formatBasic(const std::string& train, int speedKmh, int kmTenths);

} // namespace dect2::lbj
