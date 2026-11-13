// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The engine side of FSQ and IFKP: the front end (the radio's samples to 8 kHz upper sideband audio), the text, the chat log, the messages
// of FSQ (sender, addressee, trigger), Listen, and the telemetry the app shows. FsqReceiver and IfkpReceiver are this with the right modem.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/fsq/kbd33.h"
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kKbd33TextMax = 4000;
constexpr size_t kKbd33LinesMax = 80;
constexpr size_t kKbd33MsgMax = 60;

struct Kbd33Line {
    double timeSec = 0;          // signal seconds of its first character
    std::string text;
};

// A line of FSQ text taken apart: "LY2ABC:5f LY2XYZ? how are you" -> from LY2ABC (checksum 5f good), to LY2XYZ, trigger '?'
struct Kbd33Msg {
    double timeSec = 0;
    std::string from;            // the sender when the line starts with "CALL:" (empty when not)
    bool crcOk = false;          // the two hex digits after the colon are the CRC of the callsign
    std::string to;              // the addressee ("allcall" and "cqcqcq" for everybody), empty when the text is not directed
    char trigger = ' ';          // the command character after the addressee (' ' for plain text)
    std::string text;            // what follows
    bool toMe = false;
};

// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
struct Kbd33Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 2 decoding text
    double cfoHz = 0;            // how far the AFC moved the tone grid since it locked
    float snrDb = 0;            // symbol energy against the noise, in the band of the 33 tones
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // locked and the tones are clear
    uint64_t blocksOk = 0;       // characters decoded
    uint64_t blocksBad = 0;      // symbols too weak to use
    // details
    int speed = -1;              // index into the modem's speeds once found, else -1
    bool autoSpeed = true;
    double baud = 0, spacingHz = 0, bandHz = 0;
    double gridHz = 0;           // audio frequency of grid position 0
    double occLoHz = 0, occHiHz = 0;   // the lowest and highest tone heard so far (Hz above the dial)
    double loHz = 0, hiHz = 0;   // the audio window searched
    float quality = 0;           // share of a symbol's energy in its strongest tone, smoothed (about 0.07 for noise)
    float floorDb = -200;        // noise floor of the window spectrum, dB per bin
    double timeSec = 0;
    double inputRate = 0;
    int syncs = 0;
    uint64_t chars = 0;
    std::string text;            // the newest kKbd33TextMax characters
    std::vector<Kbd33Line> lines;
    std::vector<std::string> calls;
    std::vector<Kbd33Msg> msgs;  // FSQ only
    std::vector<float> toneUse;  // how often each of the grid positions was the strongest tone
    std::vector<float> spectrum; // dB per bin, loHz .. hiHz, for the waterfall
    uint64_t specSeq = 0;
};

// The command characters of FSQ and what they ask for (a short English phrase), empty for an unknown one
const char* fsqTriggerMeaning(char c);

// Takes one line of FSQ text apart (myCall: for toMe, may be empty)
Kbd33Msg fsqParseLine(const std::string& line, const std::string& myCall);

inline std::string kbd33Summary(const Kbd33Telemetry& t, const char* name) {
    char b[200];
    if (t.state == 0 || t.speed < 0) { snprintf(b, sizeof b, "searching for %s", name); return b; }
    snprintf(b, sizeof b, "%s %.2g baud: decoding, %.0f Hz band, %.0f dB, %llu chars", name, t.baud, t.bandHz, t.snrDb, (unsigned long long)t.chars);
    return b;
}

class Kbd33Station {
public:
    explicit Kbd33Station(bool ifkp, double minSampleRate);
    ~Kbd33Station();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(Kbd33Telemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSpeed(int speed);                  // -1 automatic
    void setMyCall(const std::string& call);   // FSQ: messages to this call are marked
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
