// The first-run tour, pure part (no ImGui): the step list, the anchor registry (screen rectangles the interface reports each
// frame) and the rules for which steps are shown and when the tour starts by itself. Drawn by tour.cpp; tested by tests/app/test_tour.cpp.
#pragma once
#include <map>
#include <string>

namespace tour {

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid() const { return x1 > x0 && y1 > y0; }
};

// `anchor` names the part of the window the step points at (see tour::mark in the interface code). `text` may hold one %d: the number of modes.
struct Step {
    const char* anchor;
    const char* title;
    const char* text;
};

// A short guided tour from Help > Guided tours: its own steps, drawn the same way as the first-run tour.
struct Tour {
    const char* title;
    const Step* steps;
    int n;
};

#ifndef N_
#define N_(s) s
#define TOUR_UNDEF_N_
#endif
inline const Step* steps(int* n) {
    static const Step kSteps[] = {
        {"source", N_("Source"),
         N_("Choose the radio or recording that supplies samples. Record IQ saves the raw signal for later playback.")},
        {"tuning", N_("Tuning and gain"),
         N_("Set the frequency, band and sample rate here. Gain and AGC set the input level; Second mode decodes another mode from the same capture.")},
        {"modes", N_("Modes"),
         N_("%d modes, grouped by family. Use the search box to find one by name.")},
        {"spectrum", N_("Spectrum and waterfall"),
         N_("Click to tune and scroll to zoom. Peak hold keeps the strongest level seen on each frequency.")},
        {"panel", N_("Mode panel"),
         N_("Each mode has its own tabs for decoded output, settings and tools. They change with the selected mode.")},
        {"memory", N_("Memory and frequencies"),
         N_("Memory stores frequencies with their mode and settings. Frequencies and band plans list what is on air, per mode.")},
        {"receivers", N_("Receivers"),
         N_("Each receiver pairs a radio with a mode. This strip switches between them and chooses which one plays sound.")},
        {"audio", N_("Audio"),
         N_("Listen plays the demodulated signal. Volume and mute are here; Record audio saves it to a file.")},
        {"server", N_("Server and support"),
         N_("Server sends decoded positions to the public map. Support helps fund development.")},
        {"view", N_("Interface and windows"),
         N_("View switches the layout (Sidebar, Scope, Tiles, Faceplate, Panel), the classic interface, text size and palette; Touch interface is under Tools. Windows such as Frequencies, Sweep scanner, Direction finder and Automations open from here.")},
        {"help", N_("Help"),
         N_("Documentation, troubleshooting, credits and licences are here. Guided tour starts this tour again.")},
    };
    if (n) *n = (int)(sizeof kSteps / sizeof *kSteps);
    return kSteps;
}

// The four guided tours of the Help menu, in menu order. Each step points at a part the interface already reports with tourMark.
inline const Tour* tours(int* n) {
    static const Step kFirst[] = {
        {"source", N_("Source"),
         N_("Pick the radio or recording that supplies the samples. Nothing is received until a source is chosen.")},
        {"modes", N_("Mode"),
         N_("The mode decides what is decoded. Use the search box to find one by name.")},
        {"tuning", N_("Tuning"),
         N_("Enter the frequency and sample rate here. Gain and AGC set the input level.")},
        {"audio", N_("Listen"),
         N_("Listen plays the demodulated sound. Volume and mute are on this strip.")},
    };
    static const Step kSpectrum[] = {
        {"spectrum", N_("Spectrum and waterfall"),
         N_("The top shows the spectrum, and the waterfall scrolls its history below. Click a signal to tune to it.")},
        {"tuning", N_("Sample rate"),
         N_("The sample rate sets how wide the spectrum is. A wider view shows more signals at once.")},
        {"view", N_("Layout"),
         N_("View switches the layout and the text size. Each layout arranges the spectrum and the panels differently.")},
    };
    static const Step kRecording[] = {
        {"source", N_("Record the samples"),
         N_("Record IQ saves the raw signal to a file for later playback.")},
        {"audio", N_("Record the sound"),
         N_("Record audio saves the sound you hear to a file. Volume and mute are on the same strip.")},
    };
    static const Step kScanning[] = {
        {"view", N_("Open a scanner"),
         N_("View opens the Sweep scanner for a fast wideband sweep, and the Scanner, which is experimental, that steps through a list of frequencies.")},
        {"memory", N_("Frequencies"),
         N_("Memory stores frequencies with their mode and settings. Frequencies lists what is on air, per mode.")},
        {"spectrum", N_("Peak hold"),
         N_("Peak hold keeps the strongest level seen on each frequency, so short bursts stay visible while scanning.")},
    };
    static const Tour kTours[] = {
        {N_("First steps"), kFirst, (int)(sizeof kFirst / sizeof *kFirst)},
        {N_("Waterfall and spectrum"), kSpectrum, (int)(sizeof kSpectrum / sizeof *kSpectrum)},
        {N_("Recording"), kRecording, (int)(sizeof kRecording / sizeof *kRecording)},
        {N_("Scanning"), kScanning, (int)(sizeof kScanning / sizeof *kScanning)},
    };
    if (n) *n = (int)(sizeof kTours / sizeof *kTours);
    return kTours;
}
#ifdef TOUR_UNDEF_N_
#undef N_
#undef TOUR_UNDEF_N_
#endif

// the steps of a tour, or of the first-run tour when t is null
inline const Step* stepsOf(const Tour* t, int* n) { if (!t) return steps(n); if (n) *n = t->n; return t->steps; }
inline int count(const Tour* t = nullptr) { int n = 0; stepsOf(t, &n); return n; }

// Rectangles reported by the interface; a rectangle counts as present when it was reported this frame or the one before.
class Anchors {
public:
    void set(const std::string& id, Rect r, long frame) { m_[id] = {r, frame}; }
    bool has(const std::string& id, long frame) const {
        auto it = m_.find(id);
        return it != m_.end() && it->second.first.valid() && frame - it->second.second <= 1;
    }
    Rect get(const std::string& id) const { auto it = m_.find(id); return it == m_.end() ? Rect() : it->second.first; }
    void clear() { m_.clear(); }
private:
    std::map<std::string, std::pair<Rect, long>> m_;
};

inline bool shown(const Anchors& a, long frame, int i, const Tour* t = nullptr) { return i >= 0 && i < count(t) && a.has(stepsOf(t, nullptr)[i].anchor, frame); }

// The step after (dir = 1) or before (dir = -1) `cur` that can be shown; -1 when there is none. cur = -1 starts from the beginning.
inline int next(const Anchors& a, long frame, int cur, int dir, const Tour* t = nullptr) {
    const int n = count(t);
    for (int i = cur + (dir > 0 ? 1 : -1); i >= 0 && i < n; i += dir) if (shown(a, frame, i, t)) return i;
    return -1;
}
inline int visibleCount(const Anchors& a, long frame, const Tour* t = nullptr) { int c = 0; for (int i = 0; i < count(t); i++) c += shown(a, frame, i, t); return c; }
// 1-based position of step `cur` among the steps that are shown
inline int position(const Anchors& a, long frame, int cur, const Tour* t = nullptr) { int c = 0; for (int i = 0; i <= cur && i < count(t); i++) c += shown(a, frame, i, t); return c; }

// Preference key of "the tour has been seen". It keeps the key of the earlier tour, so nobody sees it again after an update.
inline const char* prefKey() { return "wizardDone"; }
// The guided tour last left (its index in tours(), -1 once it is finished) and the step it was on.
inline const char* prefKeyTour() { return "guidedTour"; }
inline const char* prefKeyStep() { return "guidedStep"; }

// Whether the tour opens by itself at start-up: first start only (not after an update), not with command-line arguments, and after the setup
inline bool autoStart(bool noArgs, bool done, bool setupPending) { return noArgs && !done && !setupPending; }

// The first-run guides are shown one at a time (#491): a fresh install with no radio opened the tour and "Get me listening" together; the
// wizard sat over the tour and the tour's Next and Skip did nothing. Order: the setup, then the tour, then "Get me listening", then a lesson
// card; the others wait (they stay open and appear when the one in front closes).
enum class Guide { None, Setup, Tour, Listen, Lesson };
inline Guide guideInFront(bool setupOpen, bool tourOpen, bool listenOpen, bool lessonOn) {
    return setupOpen ? Guide::Setup : tourOpen ? Guide::Tour : listenOpen ? Guide::Listen : lessonOn ? Guide::Lesson : Guide::None;
}

}  // namespace tour
