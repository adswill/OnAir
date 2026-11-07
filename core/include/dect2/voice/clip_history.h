// Voice clip history: the list of WAV clips a voice mode saved (one per call), with automatic retention so the folder does not grow
// for ever. Shared by every voice mode that saves calls as WAV (Iridium, and the digital voice monitor: DMR, P25, NXDN, dPMR, D-STAR,
// YSF, TETRA, M17). No GUI here: app/voice/voice_clips_ui.cpp draws it. docs/VOICE-CLIPS.md.
//
// Retention (ClipPolicy): keep the newest `keepLast` clips (0 = no count limit), delete clips older than `maxAgeDays` (0 = off), keep the
// unpinned clips within `maxMB` (0 = off). A pinned (starred) clip is never deleted and does not count towards any limit.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dect2::voice {

struct Clip {
    std::string path;      // the WAV file
    std::string mode;      // "iridium", "dmr", ...
    std::string label;     // what the user sees: frequency, talkgroup, ID ("1621.250 MHz down", "TG 91 from 3120123")
    int64_t wallStart = 0; // Unix time the clip began
    double seconds = 0;    // length of the sound
    uint64_t bytes = 0;    // size of the file
    bool pinned = false;   // kept for good
};

struct ClipPolicy {
    int keepLast = 20;     // 0 = keep all (no count limit); range 1..1000 otherwise
    int maxAgeDays = 0;    // 0 = never by age
    int maxMB = 0;         // 0 = no size cap
    static ClipPolicy clamped(ClipPolicy p);   // keepLast 0 or 1..1000, days 0..3650, MB 0..100000
};

// Which clips the policy deletes (indexes into `clips`, any order). Pure: touches nothing.
std::vector<size_t> clipsToDelete(const std::vector<Clip>& clips, const ClipPolicy& p, int64_t now);

// 16 bit mono PCM WAV helpers (the clips every mode writes).
bool wavInfo(const std::string& path, int& rate, double& seconds);   // false when not a readable WAV
bool wavReadMono16(const std::string& path, std::vector<int16_t>& pcm, int& rate);
// true when the samples hold sound: peak at least `minPeak` (a plug-in that fills zeros makes only silence)
bool pcmHasSound(const std::vector<int16_t>& pcm, int minPeak = 64);

class ClipHistory {
public:
    explicit ClipHistory(std::string mode = {}) : mode_(std::move(mode)) {}
    const std::string& mode() const { return mode_; }
    void setPolicy(const ClipPolicy& p) { policy_ = ClipPolicy::clamped(p); }
    const ClipPolicy& policy() const { return policy_; }
    // Adds a clip, or updates the one with the same path (pin and the richer fields are kept). A path the user removed, or that is not
    // a file any more, is not added.
    void add(Clip c);
    // Reads every .wav under `dir` (recursive, only names starting with `prefix` when given) that is not listed yet; the label is the file
    // name, the time the file's modification time minus its length. Returns the number added.
    size_t scanFolder(const std::string& dir, const std::string& prefix = {});
    std::vector<Clip> clips() const;                 // newest first
    size_t size() const { return clips_.size(); }
    bool setPinned(const std::string& path, bool on);
    // Removes the entry; the file too when deleteFile (never a pinned one unless force). True when the entry existed.
    bool remove(const std::string& path, bool deleteFile = true, bool force = false);
    // Applies the policy: deletes files and entries. Returns the number deleted.
    size_t prune(int64_t now);
    // Pinned paths, one per line: the app keeps them in its settings so a pin survives a restart.
    std::string savePins() const;
    void loadPins(const std::string& text);
    uint64_t totalBytes() const;

private:
    std::string mode_;
    ClipPolicy policy_;
    std::vector<Clip> clips_;
    std::set<std::string> pins_, removed_;
};

} // namespace dect2::voice
