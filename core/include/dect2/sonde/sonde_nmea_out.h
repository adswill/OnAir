// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sends the NMEA text of the selected sonde (sonde_nmea.h) to a serial port (COMx, /dev/cu.*, /dev/tty*, a pty), or to host:port over TCP
// (client) or UDP. Off until switched on; the writing runs on its own thread so a slow port never blocks the screen or the receiver.
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace dect2 {

struct SondeNmeaConfig {
    bool enabled = false;
    int kind = 0;                 // 0 serial port, 1 TCP host:port, 2 UDP host:port
    std::string path;             // serial: COM3, /dev/cu.usbserial-1420, /dev/ttyUSB0, a pty
    int baud = 4800;
    std::string host = "127.0.0.1";
    int port = 10110;
};

class SondeNmeaOut {
public:
    static SondeNmeaOut& get();
    void configure(const SondeNmeaConfig& c);      // opens (or closes) the output
    void send(const std::string& text);            // the latest sentences; "" sends nothing. Never blocks.
    std::string status() const;                    // "off", "sending to ...", or why it cannot
    unsigned long long sentCount() const;
    void stop();
private:
    SondeNmeaOut() = default;
    void run(SondeNmeaConfig c, unsigned long long myGen);
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string pending_, status_ = "off";
    unsigned long long sent_ = 0, gen_ = 0;
    std::atomic<bool> quit_{false};
    std::thread th_;
};

}  // namespace dect2
