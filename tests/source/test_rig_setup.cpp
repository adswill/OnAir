// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Rig setup helpers: the model search, the serial-port filter for every platform, baud choices, host:port detection, and (with Hamlib)
// the real model list.
#include "dect2/source/hamlib_rig.h"
#include "dect2/source/rig_setup.h"
#include <algorithm>
#include <cstdio>

using namespace dect2;

static RigModelInfo mk(int id, const char* mfg, const char* model, int bmin = 4800, int bmax = 19200, bool net = false) {
    RigModelInfo m;
    m.id = id; m.mfg = mfg; m.model = model; m.baudMin = bmin; m.baudMax = bmax; m.network = net;
    return m;
}

static bool hasPath(const std::vector<SerialPortInfo>& v, const std::string& p) {
    for (auto& x : v) if (x.path == p) return true;
    return false;
}

int main() {
    int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, m); fails++; } } while (0)

    // ---- model search ----
    const std::vector<RigModelInfo> all = {
        mk(1, "Hamlib", "Dummy"), mk(2, "Hamlib", "NET rigctl", 0, 0, true), mk(3073, "Icom", "IC-7300", 4800, 19200),
        mk(3085, "Icom", "IC-R8600", 4800, 19200), mk(3011, "Icom", "IC-R75"), mk(1035, "Yaesu", "FT-991"),
        mk(2028, "Kenwood", "TS-590S"), mk(30850, "Other", "Rig 30850"), mk(3, "Foo", "Bar 3085 x"),
    };
    auto names = [&](const std::string& q) {
        std::vector<int> ids;
        for (int i : searchRigModels(all, q)) ids.push_back(all[(size_t)i].id);
        return ids;
    };
    CHECK(searchRigModels(all, "").size() == all.size(), "empty query lists all");
    CHECK(names("r8600") == std::vector<int>({3085}), "r8600");
    CHECK(names("R8600") == std::vector<int>({3085}), "case");
    CHECK(names("ic-r8600") == std::vector<int>({3085}), "with dash");
    CHECK(names("icr8600") == std::vector<int>({3085}), "squashed across mfg and model");
    CHECK(names("ic r8600") == std::vector<int>({3085}), "two words");
    CHECK(names("icom").size() == 3, "icom: three Icoms");
    {
        auto v = names("3085");
        CHECK(v.size() == 3 && v[0] == 3085 && v[1] == 30850, "id: exact first, id prefix next, then a name that holds it");
    }
    CHECK(names("zzz").empty(), "no match");
    CHECK(names("  icom   7300 ") == std::vector<int>({3073}), "extra spaces");
    CHECK(names("ft991") == std::vector<int>({1035}) && names("ts 590") == std::vector<int>({2028}), "yaesu and kenwood");
    CHECK(findRigModel(all, 3085) == 3 && findRigModel(all, 99999) == -1, "findRigModel");
    CHECK(rigModelLabel(all[3]) == "Icom IC-R8600 (id 3085)", "label");

    // ---- baud ----
    {
        auto b = rigBaudChoices(mk(1, "x", "y", 4800, 19200));
        CHECK(b == std::vector<int>({4800, 9600, 19200}), "baud range");
        CHECK(rigDefaultBaud(mk(1, "x", "y", 4800, 19200)) == 9600, "default 9600 when in range");
        CHECK(rigDefaultBaud(mk(1, "x", "y", 38400, 38400)) == 38400, "fixed rate");
        CHECK(rigDefaultBaud(mk(1, "x", "y", 19200, 115200)) == 19200, "range above 9600 starts at its lowest");
        CHECK(rigDefaultBaud(mk(2, "x", "y", 0, 0, true)) == 0, "network rig has no baud");
        auto fixed = rigBaudChoices(mk(1, "x", "y", 31250, 31250));
        CHECK(fixed == std::vector<int>({31250}), "an unusual fixed speed is still offered");
        CHECK(rigBaudChoices(mk(1, "x", "y", 0, 0)).size() >= 8, "unknown range: all standard rates");
    }

    // ---- network paths ----
    CHECK(looksLikeNetworkRigPath("localhost:4532") && looksLikeNetworkRigPath("192.168.1.5:4532") && looksLikeNetworkRigPath("[::1]:4532"), "host:port");
    CHECK(!looksLikeNetworkRigPath("/dev/cu.usbserial-1420") && !looksLikeNetworkRigPath("COM3") && !looksLikeNetworkRigPath("") &&
              !looksLikeNetworkRigPath("localhost:") && !looksLikeNetworkRigPath("localhost:99999") && !looksLikeNetworkRigPath("fe80::1"), "not host:port");

    // ---- serial ports: macOS ----
    {
        auto v = filterSerialPorts(PortPlatform::Mac, {"cu.Bluetooth-Incoming-Port", "cu.debug-console", "cu.wlan-debug", "tty.usbserial-1420",
                                                       "cu.usbserial-1420", "cu.usbmodem14101", "cu.BLTH", "null", "cu.SLAB_USBtoUART", "zero"});
        CHECK(v.size() == 4, "mac: cu.* minus bluetooth, debug-console and wlan-debug");
        CHECK(hasPath(v, "/dev/cu.usbserial-1420") && hasPath(v, "/dev/cu.usbmodem14101") && !hasPath(v, "/dev/tty.usbserial-1420"), "mac: cu twin only");
        CHECK(v.front().usb && v.back().path == "/dev/cu.BLTH" && !v.back().usb, "mac: USB first");
    }
    // ---- Linux ----
    {
        auto v = filterSerialPorts(PortPlatform::Linux, {"ttyS0", "ttyS1", "ttyUSB0", "ttyUSB1", "ttyACM0", "tty0", "tty", "console", "ttyprintk", "ttyS"},
                                   {{"usb-Silicon_Labs_CP2102-if00-port0", "/dev/ttyUSB1"}});
        CHECK(hasPath(v, "/dev/ttyUSB0") && hasPath(v, "/dev/ttyACM0") && hasPath(v, "/dev/ttyS0") && !hasPath(v, "/dev/tty0") && !hasPath(v, "/dev/console") &&
                  !hasPath(v, "/dev/ttyprintk") && !hasPath(v, "/dev/ttyS"), "linux: usb, acm and numbered ttyS only");
        CHECK(hasPath(v, "/dev/serial/by-id/usb-Silicon_Labs_CP2102-if00-port0"), "linux: by-id entry");
        CHECK(v.back().path.find("/dev/ttyS") == 0, "linux: ttyS last");
        bool named = false;
        for (auto& x : v) if (x.path == "/dev/ttyUSB1" && x.display.find("Silicon_Labs") != std::string::npos) named = true;
        CHECK(named, "linux: the ttyUSB entry shows the by-id name");
    }
    // ---- Windows ----
    {
        auto v = filterSerialPorts(PortPlatform::Windows, {"COM10", "COM3", "com1", "LPT1", "COM2"}, {{"COM3", "USB serial"}});
        CHECK(v.size() == 4 && v[0].path == "COM3" && v[0].display == "COM3  (USB serial)", "windows: usb first with name");
        CHECK(v[1].path == "COM1" && v[2].path == "COM2" && v[3].path == "COM10", "windows: numeric order, 10 after 2");
    }
    // the live scan must not crash and returns only what the filter lets through
    {
        auto live = listSerialPorts();
        for (auto& p : live) CHECK(!p.path.empty() && p.path.find("Bluetooth") == std::string::npos, "live ports sane");
    }

    // ---- prefs: 4-field entries (older version) still load, baud round-trips ----
    {
        auto old = hamlibRigsFromString("3085,/dev/cu.usbserial-1,R8600,aud");
        CHECK(old.size() == 1 && old[0].baud == 0 && old[0].model == 3085, "old entry without baud");
        HamlibRig r; r.model = 2; r.path = "localhost:4532"; r.baud = 38400; r.label = "a,b;c%d";
        auto back = hamlibRigsFromString(hamlibRigsToString({r}));
        CHECK(back.size() == 1 && back[0] == r, "round trip with baud and awkward label");
        int m = 0, bd = 0; std::string p;
        CHECK(parseHamlibSerial(hamlibRigSerial(3085, "/dev/ttyUSB0", 19200), m, p, bd) && m == 3085 && bd == 19200 && p == "/dev/ttyUSB0", "serial with baud");
        CHECK(parseHamlibSerial(hamlibRigSerial(2, "localhost:4532"), m, p, bd) && m == 2 && bd == 0 && p == "localhost:4532", "network serial keeps the colon");
    }

    // ---- the real list ----
    if (hamlibSupported() && hamlibLibraryReady()) {   // a run-time Hamlib build without the library (Windows CI) skips it
        const auto list = hamlibModelList();
        CHECK(list.size() > 200, "hamlib lists hundreds of models");
        const int d = findRigModel(list, 1), n = findRigModel(list, 2), r = findRigModel(list, 3079);
        CHECK(d >= 0 && n >= 0 && list[(size_t)n].network && !list[(size_t)d].network, "dummy serial-less, netrigctl is network");
        if (r >= 0) {
            CHECK(list[(size_t)r].mfg == "Icom" && list[(size_t)r].baudMax >= 9600, "IC-R8600 is an Icom with a serial range");
            const auto hit = searchRigModels(list, "r8600");
            CHECK(!hit.empty() && list[(size_t)hit[0]].id == 3079, "search r8600 in the real list finds the IC-R8600 (3079) before the AR8600");
            CHECK(rigDefaultBaud(list[(size_t)r]) > 0, "R8600 default baud");
        } else printf("note: no model 3079 in this Hamlib\n");
        const auto ic = searchRigModels(list, "icom");
        CHECK(ic.size() > 20, "many Icom models");
        const auto num = searchRigModels(list, "1025");
        CHECK(!num.empty() && list[(size_t)num[0]].id == 1025, "numeric id first");
    }
#undef CHECK
    if (!fails) printf("rig setup: all ok\n");
    return fails ? 1 : 0;
}
