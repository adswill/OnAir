// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Motorola Type II (SmartNet, SmartZone) control channel receiver with call following (trunk pool + CaptureVoice): analog calls as FM, digital ones through the P25 receiver and the
// user-installed IMBE plug-in. Encrypted calls are logged and not followed. Receive only. A band plan is set for you when the frequency says which one it is; VHF / UHF (OBT) sites need a custom plan.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/smartnet/smartnet_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SmartnetTelemetry;

class SmartnetReceiver {
public:
    SmartnetReceiver();
    ~SmartnetReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);                // the frequency at the centre of the capture, 0 when not known (file input)
    bool ready() const;
    void reset();
    void clearMessages();
    void feed(const cf32* x, size_t n);
    bool telemetry(SmartnetTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setBand(smartnet::Band b, const smartnet::CustomPlan& c = smartnet::CustomPlan());   // Auto: from the frequency and the control channel broadcast
    void setFollow(bool on);                    // follow calls inside the capture
    void setFollowPrivate(bool on);             // ... also unit-to-unit calls (off by default)
    void setFollowGroups(const std::vector<int>& tgs);   // only these talkgroups (empty: all)
    void setListenTg(int tg);                   // the talkgroup whose sound is played; -1 the newest call
    void setVoiceSlots(int n);                  // calls followed at once inside the capture, 1 .. 16 (default 4; single-radio trunking, docs/TRUNK1-3.9.md)
    void setRecordDir(const std::string& dir);  // one WAV + JSON per call; empty: no files
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                     // decode but never open the sound device
    void setOswTap(std::function<void(const smartnet::Osw&)> cb);                                      // tests
    void setAudioTap(std::function<void(int tg, const float* x, size_t n, int rate)> cb);              // tests: the sound of each followed call
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning smartnetTuning();

} // namespace dect2
