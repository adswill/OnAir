// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MIDI input ports (issue #543): the list of ports and one open port, read from the UI thread. Linux reads the raw ALSA devices (/dev/snd/midiC*D*,
// no library), Windows uses WinMM, macOS CoreMIDI. Elsewhere (Android) there are no ports. Output sends the light messages of mapped buttons back to the same controller.
#pragma once
#include "midi_logic.h"
#include <memory>
#include <string>
#include <vector>

namespace midi {

struct PortInfo { std::string id; std::string name; };

std::vector<PortInfo> listPorts();

class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    bool open(const PortInfo& port);          // false: error() says why
    void close();
    bool isOpen() const;
    const std::string& portId() const;
    void poll(std::vector<Msg>& out);         // the messages that arrived since the last call
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The output side of the controller `port` came from (found by its name), to light its buttons. Linux writes the same raw device.
class Output {
public:
    Output();
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    bool open(const PortInfo& inPort);        // false: the controller has no output (or it cannot be opened)
    void close();
    bool isOpen() const;
    void send(const uint8_t* p, size_t n);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace midi
