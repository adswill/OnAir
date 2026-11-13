// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// --list-devices / --device / --antenna: picking a radio from the list by index, serial, name part and antenna socket.
#include "dect2/source/device_pick.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static DeviceInfo mk(DeviceInfo::Kind k, const char* board, const char* name, const char* serial, const char* soapy = "") {
    DeviceInfo d; d.kind = k; d.board = board; d.name = name; d.serial = serial; d.soapyArgs = soapy; return d;
}

int main() {
    std::vector<DeviceInfo> L = {
        mk(DeviceInfo::HackRF, "HackRF One", "HackRF One 0000000000000000a06063c8234c4b1f", "0000000000000000a06063c8234c4b1f"),
        mk(DeviceInfo::Native, "sdrplay", "SDRplay RSP2 171007A210 antenna A (native, experimental)", "171007A210"),
        mk(DeviceInfo::Native, "sdrplay", "SDRplay RSP2 171007A210 antenna B (native, experimental)", "171007A210"),
        mk(DeviceInfo::Native, "sdrplay", "SDRplay RSP2 171007A210 Hi-Z (native, experimental)", "171007A210"),
        mk(DeviceInfo::Native, "sdrplay", "SDRplay RSPduo 2001000099 Tuner 2 (native, experimental)", "2001000099"),
        mk(DeviceInfo::Native, "rtlsdr", "RTL-SDR Blog V4 00000001", "00000001"),
        mk(DeviceInfo::Soapy, "lime", "LimeSDR Mini antenna LNAW", "1D3A5B", "driver=lime,serial=1D3A5B#antenna=LNAW"),
        mk(DeviceInfo::Soapy, "lime", "LimeSDR Mini antenna LNAH", "1D3A5B", "driver=lime,serial=1D3A5B#antenna=LNAH"),
    };
    std::string e;
    CHECK(deviceAntenna(L[1]) == "A" && deviceAntenna(L[2]) == "B" && deviceAntenna(L[3]) == "Hi-Z" && deviceAntenna(L[4]) == "Tuner 2");
    CHECK(deviceAntenna(L[0]).empty() && deviceAntenna(L[6]) == "LNAW" && deviceAntenna(L[7]) == "LNAH");
    CHECK(deviceDriver(L[0]) == "hackrf" && deviceDriver(L[1]) == "sdrplay" && deviceDriver(L[6]) == "lime");
    const std::string txt = deviceListText(L);
    CHECK(txt.rfind("index\tdriver\tname\tserial\tantenna\n", 0) == 0 && txt.find("1\tsdrplay\tSDRplay RSP2 171007A210 antenna A") != std::string::npos);
    CHECK(txt.find("0\thackrf\t") != std::string::npos && txt.find("\t-\n") != std::string::npos);   // no antenna: "-"

    // the example of the request
    CHECK(pickDevice(L, "SDRplay RSP2 171007A210 antenna A", "", e) == 1);
    CHECK(pickDevice(L, "sdrplay rsp2 171007a210 ANTENNA b", "", e) == 2);
    CHECK(pickDevice(L, "171007A210", "B", e) == 2 && pickDevice(L, "171007A210", "a", e) == 1 && pickDevice(L, "171007A210", "hi-z", e) == 3);
    CHECK(pickDevice(L, "RSP2", "A", e) == 1);
    CHECK(pickDevice(L, "171007A210", "", e) == 1);        // one radio, several sockets: the first (what OnAir always used)
    CHECK(pickDevice(L, "RSP2 antenna B 171007A210", "", e) == 2);   // the words in any order
    // index, serial, driver, exact name
    CHECK(pickDevice(L, "0", "", e) == 0 && pickDevice(L, "4", "", e) == 4);
    CHECK(pickDevice(L, "00000001", "", e) == 5);          // a serial that is all digits is a serial, not an index
    CHECK(pickDevice(L, "hackrf", "", e) == 0 && pickDevice(L, "rtl", "", e) == 5 && pickDevice(L, "duo", "", e) == 4);
    CHECK(pickDevice(L, "LimeSDR Mini antenna LNAH", "", e) == 7 && pickDevice(L, "lime", "LNAW", e) == 6);
    CHECK(pickDevice(L, "", "Tuner 2", e) == 4);           // an antenna alone
    // refusals say why
    CHECK(pickDevice(L, "77", "", e) == -1 && e.find("no radio matches") != std::string::npos);
    CHECK(pickDevice(L, "airspy", "", e) == -1);
    CHECK(pickDevice(L, "0", "A", e) == -1 && e.find("antenna") != std::string::npos);
    CHECK(pickDevice(L, "sdrplay", "", e) == -1 && e.find("matches") != std::string::npos && e.find("2001000099") != std::string::npos);   // not unique: lists the candidates
    CHECK(pickDevice(L, "RSP2", "C", e) == -1);
    CHECK(pickDevice({}, "0", "", e) == -1 && pickDevice(L, "  ", "", e) == -1);
    printf(fails ? "device pick: %d FAILED\n" : "device pick: all passed\n", fails);
    return fails ? 1 : 0;
}
