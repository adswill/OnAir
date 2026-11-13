// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/source/antenna_ports.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static DeviceInfo sdr(const std::string& model, const std::string& ser, int n, const char* port) {
    DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "sdrplay"; d.serial = ser;
    d.nativeArgs = n < 0 ? ser : ser + "#" + std::to_string(n);
    d.name = "SDRplay " + model + " " + ser + (*port ? std::string(" ") + port : "") + " (native, experimental)";
    return d;
}
static DeviceInfo soapy(const std::string& args, const char* ant) {
    DeviceInfo d; d.kind = DeviceInfo::Soapy; d.board = "soapy"; d.soapyArgs = args + (*ant ? std::string("#antenna=") + ant : ""); d.name = d.soapyArgs; return d;
}
int main() {
    std::vector<DeviceInfo> v;
    v.push_back(DeviceInfo());
    v.push_back(sdr("RSP1A", "S1", -1, ""));
    v.push_back(sdr("RSPdx", "DX1", 0, "antenna A"));
    v.push_back(sdr("RSPdx", "DX1", 1, "antenna B"));
    v.push_back(sdr("RSPdx", "DX1", 2, "antenna C"));
    v.push_back(sdr("RSPdx", "DX2", 0, "antenna A"));
    v.push_back(sdr("RSPdx", "DX2", 1, "antenna B"));
    v.push_back(soapy("driver=lime,serial=1", "LNAH"));
    v.push_back(soapy("driver=lime,serial=1", "LNAW"));
    v.push_back(soapy("driver=lime,serial=2", "LNAH"));
    v.push_back(soapy("driver=rtlsdr", ""));
    auto s = antennaSiblings(v, 3);
    CHECK(s.size() == 3 && s[0].devIdx == 2 && s[2].devIdx == 4);
    CHECK(s.size() == 3 && s[0].label == "A" && s[1].label == "B" && s[2].label == "C");
    CHECK(antennaSiblings(v, 5).size() == 2);   // the other serial is its own radio
    CHECK(antennaSiblings(v, 1).empty());
    s = antennaSiblings(v, 8);
    CHECK(s.size() == 2 && s[0].devIdx == 7 && s[1].label == "LNAW");
    CHECK(antennaSiblings(v, 9).empty());       // one port of serial 2
    CHECK(antennaSiblings(v, 10).empty());
    CHECK(antennaSiblings(v, 0).empty() && antennaSiblings(v, 99).empty());
    CHECK(!antennaPortHint(v[4], "C").empty());
    if (!fails) puts("antenna ports OK");
    return fails ? 1 : 0;
}
