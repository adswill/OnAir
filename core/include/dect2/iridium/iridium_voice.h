// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium voice: the traffic-channel frames of voice calls, grouped into calls, handed to the user-installed AMBE-family plug-in "iridium"
// (codec_registry.h) and played / recorded. OnAir ships no vocoder; without the plug-in the calls are listed and stay silent.
//
// WHAT IS KNOWN (public iridium-toolkit descriptions): a burst whose link control word has frame type 0 is a voice slot; 312 bits follow the
// 46 bit LCW. The toolkit's bits_to_dfs.py hands its AMBE decoder (ir77_ambe_decode) those 312 bits as 39 bytes with the first bit received as
// the LEAST significant bit of each byte; OnAir hands the plug-in the same bytes. Link control word 'silent' (type 001111, third field 100..0)
// marks silence frames: counted, not sent.
// WHAT IS NOT VERIFIED: the FEC / interleaving of those 312 bits inside the vocoder and the layout of its AMBE-family frames. OnAir does no
// de-scrambling or de-interleaving of its own; a plug-in built for it does the vocoder-specific part. No public
// description known to us defines an "encrypted" flag, so none is invented: a frame whose link control word fails its check, or a call whose
// frames the plug-in refuses, is shown as not readable and stays silent. Nothing here is checked against a real Iridium recording.
#pragma once
#include "dect2/iridium/iridium_frame.h"
#include "dect2/iridium/iridium_tel.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kIridiumVoiceBytes = 39;   // the 312 bit voice field of a frame, as handed to the plug-in

// The 39 voice bytes of a burst (frame type 0 link control word); false when the burst is too short.
bool iridiumVoicePayload(const IridiumBurstBits& b, std::vector<uint8_t>& out);
// Test signal: a voice slot carrying `payload` (padded / cut to 39 bytes), bits after the unique word.
std::vector<uint8_t> iridiumBuildVoice(const std::vector<uint8_t>& payload, int lcw2 = 0, int lcw3 = 0);

class IridiumVoice {
public:
    IridiumVoice();
    ~IridiumVoice();
    // Every decoded burst, in time order: ring / broadcast frames give the satellite and beam of a frequency; voice frames feed the calls.
    void feed(const IridiumBurstBits& b, const IridiumFrame& f);
    void expire(double timeSec);          // closes calls silent for more than a second (saves the recording)
    void closeAll();                      // every call ends (stop, retune, clear)
    std::vector<IridiumCall> calls() const;   // newest last, at most 100
    void clear();
    uint64_t framesToCodec() const;

    // Listen: 0 = the first call with audio, -1 = nothing, otherwise the call id. Sound as in the other voice modes.
    void setListenCall(int id);
    // Mixer, as in the Inmarsat voice tab: any soloed call is heard (several together) and the Listen choice is ignored; a muted call is
    // recorded but not played. Unknown ids are ignored.
    void setMute(int id, bool on);
    void setSolo(int id, bool on);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);               // decode but do not open the sound device (tests)
    void setAudioTap(std::function<void(int callId, const float* mono48k, size_t n)> cb);
    void setRecordFolder(const std::string& dir);   // "" = no recordings; one 16 bit WAV per call at the codec's rate
    bool codecReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
