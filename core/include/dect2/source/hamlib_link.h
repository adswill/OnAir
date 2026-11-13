// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One open Hamlib CAT connection (serial or rigctld network), safe to call from several threads. Only built with DECT2_HAVE_HAMLIB;
// without it every call fails with a message. Receiving only ever tunes (frequency, mode); setPtt() is used by the transmit sink alone.
#pragma once
#include <memory>
#include <string>

namespace dect2 {

struct HamlibConn {
    int model = 0;             // Hamlib model number (1 = dummy, 2 = rigctld over the network)
    std::string path;          // serial device, COM port, or host:port for a network rig
    int baud = 0;              // 0 = the model's default
    int timeoutMs = 1000;      // per command; keeps a dead rig from freezing the program
    int retries = 1;
    int pttType = 0;           // 0 CAT command, 1 RTS line, 2 DTR line, 3 none (VOX: the rig keys itself from the audio)
    std::string pttPath;       // the serial port whose RTS / DTR keys the rig ("" = the CAT port itself)
};

class HamlibLink {
public:
    HamlibLink();
    ~HamlibLink();
    HamlibLink(const HamlibLink&) = delete;
    HamlibLink& operator=(const HamlibLink&) = delete;

    bool open(const HamlibConn& c, std::string& err);   // closes a link already open first
    void close();                                        // safe to call any time, any thread, any number of times
    bool isOpen() const;

    bool setFreq(double hz, std::string& err);           // the rig's current VFO
    bool getFreq(double& hz, std::string& err);
    bool setMode(const std::string& mode, int passbandHz, std::string& err);   // Hamlib mode name ("USB", "WFM"...); passband 0 = the rig's normal
    bool getMode(std::string& mode, int& passbandHz, std::string& err);
    bool setPtt(bool on, std::string& err);              // transmit sink only
    // Split: transmit on txHz while the receive VFO stays where it is (on = false: back to simplex). getSplitFreq reads the TX VFO back.
    bool setSplit(bool on, double txHz, std::string& err);
    bool getSplitFreq(double& hz, std::string& err);
    std::string info() const;                            // rig_get_info text ("" when the rig has none)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Hamlib mode name -> true when Hamlib knows it ("USB", "LSB", "AM", "FM", "WFM", "CW", "CWR", "PKTUSB", ... ; case-insensitive).
bool hamlibModeKnown(const std::string& name);
// True when both names are the same Hamlib mode: a rig reports some modes under another spelling ("PKTFM" comes back as "FM-D").
bool hamlibSameMode(const std::string& a, const std::string& b);
// The mode names of Hamlib's own spelling for what OnAir's mode list offers (identity for the names it knows, "" for an unknown one).
std::string hamlibCanonicalMode(const std::string& name);

} // namespace dect2
