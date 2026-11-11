// Audio capture for the transmitter: CoreAudio on macOS, miniaudio elsewhere (the same split as audioout_*).
// The microphone is never opened until startMic() — TxSession does that only on START TRANSMITTING.
#pragma once
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AudioInputDevice {
    std::string id;     // empty = the default input
    std::string name;
};

std::vector<AudioInputDevice> listAudioInputs();   // the default input first
// The device id for what a user typed or picked as a sound card: an id as it is, "Default input" / "default" -> "" (the default input), or a
// device name, with or without the "Sound card: " of the source list, matched without case (#358: a Hamlib rig's sound card was typed as its
// name, "USB Audio CODEC", and never found). Returns false (id untouched) when nothing matches.
inline bool resolveAudioInputId(const std::string& typed, const std::vector<AudioInputDevice>& devices, std::string& id) {
    auto low = [](std::string v) {
        size_t b = 0, e = v.size();
        while (b < e && (v[b] == ' ' || v[b] == '\t')) b++;
        while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        v = v.substr(b, e - b);
        for (char& c : v) c = (char)std::tolower((unsigned char)c);
        return v;
    };
    std::string t = low(typed);
    if (t.rfind("sound card:", 0) == 0) t = low(t.substr(11));
    if (t.empty() || t == "default" || t == "default input") { id.clear(); return true; }
    for (const AudioInputDevice& d : devices) if (!d.id.empty() && d.id == typed) { id = d.id; return true; }
    for (const AudioInputDevice& d : devices) if (low(d.name) == t) { id = d.id; return true; }
    return false;
}

class AudioIn {
public:
    enum Kind { None, Mic, File, Tone, Silence };
    AudioIn();
    ~AudioIn();
    AudioIn(const AudioIn&) = delete;
    AudioIn& operator=(const AudioIn&) = delete;

    // kind Mic: deviceId empty = default. kind File: path is WAV/MP3 (FFmpeg) or 16-bit PCM WAV. Never starts the mic for File/Tone/Silence.
    bool start(Kind k, int sampleRate, const std::string& pathOrDevice, double toneHz, std::string& err);
    void stop();
    void setMuted(bool m);
    void setGain(float g);
    // Interleaved stereo float at sampleRate(). Zeros if nothing is ready (mic underrun) or muted. Returns frames written (= n).
    int read(float* stereo, int n);
    // A microphone ring only: up to n frames that have actually been captured, none invented (0 when nothing is ready). The sound-card source
    // reads this way so the sample clock is the card's own and a late block is a wait, not a gap of zeros in the signal.
    int readAvailable(float* stereo, int n);
    uint64_t overruns() const;     // frames the capture ring had no room for (the reader was too slow)
    float peak() const;            // 0 .. 1, decays; for a meter
    double fileSeconds() const;    // kind File: the length of the loaded file in seconds (0 for the other kinds)
    int sampleRate() const { return rate_; }
    bool running() const { return running_; }
    Kind kind() const { return kind_; }

    struct Impl;
    std::unique_ptr<Impl> p_;      // backends push captured frames here

private:
    int rate_ = 48000;
    bool running_ = false;
    Kind kind_ = None;
};

} // namespace dect2
