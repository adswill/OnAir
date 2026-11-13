// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Helpers for setting up a Hamlib rig: the model list (searchable by name or number) and the connection choices (serial ports found on
// this computer, baud rates). The list-building functions are pure so they can be tested; the scanning ones are thin.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

struct RigModelInfo {
    int id = 0;
    std::string mfg;           // "Icom"
    std::string model;         // "IC-R8600"
    int status = 3;            // Hamlib: 0 alpha, 1 untested, 2 beta, 3 stable, 4 buggy
    bool network = false;      // talks over a socket (rigctld / netrigctl), not a serial port
    int baudMin = 0, baudMax = 0;   // serial_rate_min / max (0 when unknown)
    int dataBits = 8, stopBits = 1;
};

// Every model the linked Hamlib knows (empty when OnAir was built without Hamlib), sorted by manufacturer then model.
std::vector<RigModelInfo> hamlibModelList();

// "Icom IC-R8600 (id 3085)"
std::string rigModelLabel(const RigModelInfo& m);
// "stable", "beta", ...
const char* rigStatusName(int status);

// The models matching what the user typed, best first. The query is split at spaces and every word must appear in "manufacturer model id"
// (case-insensitive, dashes and spaces inside a name are ignored, so "ic r8600", "icr8600" and "IC-R8600" all match). A query that is only
// digits also matches the id as a whole (listed first) or by its start. An empty query returns every model. Returns indices into `all`.
std::vector<int> searchRigModels(const std::vector<RigModelInfo>& all, const std::string& query);
// Index in `all` of the model with this id, -1 when none.
int findRigModel(const std::vector<RigModelInfo>& all, int id);

// The baud rates worth offering for a model: the standard ones inside [baudMin, baudMax] (all standard ones when the range is unknown).
std::vector<int> rigBaudChoices(const RigModelInfo& m);
// The speed to start with: the model's usual choice (the middle of the range when it is wide, the only value when it is fixed), 0 when unknown.
int rigDefaultBaud(const RigModelInfo& m);

// ---- connections ------------------------------------------------------------------------------------------------------------------
struct SerialPortInfo {
    std::string path;       // what Hamlib is given: /dev/cu.usbserial-1420, /dev/ttyUSB0, COM3
    std::string display;    // what the list shows: path plus the friendly name when there is one
    bool usb = false;       // looks like a USB serial adapter (listed first)
};

enum class PortPlatform { Mac, Linux, Windows };

// Pure filter: takes the entries of /dev (names only, e.g. "cu.usbserial-1420"), the names in /dev/serial/by-id (Linux, may be empty) and
// for Windows the (COM name, friendly name) pairs, and returns the ports to show. Mac: only cu.* (the tty.* twin blocks on carrier), Bluetooth,
// debug and wlan ports dropped. Linux: ttyUSB*, ttyACM*, ttyS* (ttyS last), by-id links with their names. Windows: COMn, numeric order.
std::vector<SerialPortInfo> filterSerialPorts(PortPlatform platform, const std::vector<std::string>& devEntries,
                                              const std::vector<std::pair<std::string, std::string>>& named = {});
// Scans this computer (/dev, /dev/serial/by-id, or the Windows registry) and applies the filter.
std::vector<SerialPortInfo> listSerialPorts();

// "host:port" (an IPv4 or IPv6 literal in brackets, or a name) as used for rigctld; true when the text looks like one.
bool looksLikeNetworkRigPath(const std::string& path);

} // namespace dect2
