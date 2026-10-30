// MHEG-5 interpreter for the UK profile (ISO/IEC 13522-5, ETSI ES 202 184), see docs/MHEG-REDBUTTON.md. Written from the standard and its ASN.1 module; it uses no
// code of any other MHEG engine. It runs an Application and one Scene at a time from files that come out of a DSM-CC object carousel (the fetch callback), reacts to
// keys and timers through Links, and produces draw operations (MhegRender) for the TV window.
//
// Single-threaded and deterministic: the caller passes the time in step(). Everything is bounded (MhegLimits): events, actions, objects, string sizes, files; a
// damaged or hostile application can only stop itself. There is no file or network access except through the fetch callback; the interaction channel does not exist.
// Supported v1 set: Application, Scene, Rectangle, Text, Bitmap, the Variable family, Link, resident programs (Rnd, GetEngineSupport), and the actions Activate,
// Deactivate, SetVariable, Add, Subtract, Multiply, Divide, Modulo, Append, SetTimer, SetData, SetPosition, SetBoxSize, SetFillColour, SetLineColour, SetTextColour,
// BringToFront, SendToBack, SendEvent, TransitionTo, Launch, Spawn, Quit, Run, Stop, Call, GetEngineSupport. Everything else is counted as unsupported and skipped.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "dect2/tv/mheg.h"

namespace dect2 {

struct MhegLimits {
    int eventsPerStep = 5000;       // events delivered in one step() before the application is stopped as runaway
    int actionsPerStep = 50000;     // elementary actions run in one step()
    size_t queue = 4096;            // pending events; more are dropped
    size_t objects = 4000;          // objects of one group
    size_t stringBytes = 65536;     // a text or octet string variable
    size_t fileBytes = 4u << 20;    // one carousel file
    int transitionsPerStep = 8;     // scene changes in one step()
};

struct MhegKeys {                    // UK profile user input codes
    enum { Up = 1, Down = 2, Left = 3, Right = 4, Digit0 = 5, Select = 15, Cancel = 16, Red = 100, Green = 101, Yellow = 102, Blue = 103, Text = 104 };
};

struct MhegStats { uint64_t events = 0, actions = 0, linksFired = 0, unsupported = 0, transitions = 0, fetchFailures = 0; };

class MhegEngine {
public:
    // path as in the carousel ("a", "s1", "img/p.png"): false when the file is not (yet) there
    using Fetch = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;
    explicit MhegEngine(Fetch fetch, MhegLimits lim = MhegLimits());
    ~MhegEngine();
    bool start(const std::string& entry = "a", uint64_t nowMs = 0);   // load the Application and run its start-up; false when it is missing or damaged
    void key(int code);                       // a user input key code (MhegKeys); delivered at the next step()
    void step(uint64_t nowMs);                // timers, then the event queue
    bool running() const;                     // false after Quit, a fatal error or a runaway stop
    std::string scene() const;                // group identifier of the current Scene ("" when none)
    MhegRender render() const;                // the visible items now, back to front, scene coordinates
    const MhegStats& stats() const;
    const std::vector<std::string>& log() const;   // notes and errors, bounded
    std::string variableText(const std::string& group, int object) const;   // for tests: the value of a variable ("" when none)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
