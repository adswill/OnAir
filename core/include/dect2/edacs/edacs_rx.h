// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EDACS control channel receiver: FM discriminator at 76.8 kHz (8 samples per bit), dotting search, three-copy majority voting; the message layer on top (assumed
// layout, edacs_msg.h): calls, talkgroups and site information; and, with a table of the system's voice channel frequencies and a capture wide enough to hold
// them, a power scan of those channels and the following of one analog voice channel (FM, the sound of the call that the control channel announced, or of a
// carrier seen on a channel). ProVoice digital calls go through the user-installed IMBE plug-in (provoice.h); nothing encrypted is handled.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/edacs/edacs_msg.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct EdacsTelemetry;

class EdacsReceiver {
public:
    EdacsReceiver();
    ~EdacsReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    void setInverted(bool inv);   // no effect: the polarity is found from the sync word and the check bits (kept for the callers)
    bool ready() const;
    void reset();
    void clearMessages();
    void feed(const cf32* x, size_t n);
    bool telemetry(EdacsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // ---- voice channels. The table lists the frequency of LCN 1, 2, 3 ... in Hz (absolute); the capture must contain them (the radio's centre is setCenterHz)
    void setLcnTable(const std::vector<double>& freqHz);
    void setLayout(const edacs::Layout& l);              // Standard / Networked or Extended Addressing, ESK mask
    void setFollow(bool on);                              // listen to the voice channel of an announced call
    void setFollowActivity(bool on);                      // ... also to a carrier on a channel of the table that no message announced
    void setFollowGroups(const std::vector<int>& gids);   // only these groups (empty: every call)
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                               // follow and decode but never open the sound device
    void setAudioTap(std::function<void(int lcn, const float* x, size_t n)> cb);   // tests: the 48 kHz sound of the followed channel (and which one it is)
    // ---- single-radio trunking (docs/TRUNK1-3.9.md): with following on, every analog call inside the capture is also decoded in a voice slot of its
    // own at the same time (its own sound, recording and call row); the listened channel above stays the one played
    void setVoiceSlots(int n);                            // calls decoded at once, 1 .. 16 (default 4)
    void setRecordDir(const std::string& dir);            // one WAV + JSON per call; empty: no files
    void setSlotAudioTap(std::function<void(int gid, const float* x, size_t n, int rate)> cb);   // tests: the sound of every call in a voice slot
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning edacsTuning();

} // namespace dect2
