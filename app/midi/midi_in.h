// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MIDI input ports (issue #543): the list of ports and one open port, read from the UI thread. Linux reads the raw ALSA devices (/dev/snd/midiC*D*,
// no library), Windows uses WinMM, macOS CoreMIDI. Elsewhere (Android) there are no ports. Receive only: nothing is ever sent to a controller.
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

} // namespace midi
