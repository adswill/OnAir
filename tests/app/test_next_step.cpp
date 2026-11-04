// The "what do I do next" line (app/ui/next_step_logic.h): every state, the priority order, and the honesty rule for modes without a lock.
#include "../../app/ui/next_step_logic.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
using namespace nextstep;

static In run(double sec = 20) { In i; i.running = true; i.runSec = sec; i.radioHasGain = true; i.lockKnown = true; i.rmsDbfs = -40; return i; }

int main() {
    { In i; Out o = advise(i); check(o.state == State::Stopped && o.fix == Fix::Start, "stopped offers Start"); }
    { In i; i.hasRadio = false; Out o = advise(i); check(o.state == State::Stopped && o.fix == Fix::None, "no radio: no Start button"); }
    { Out o = advise(run(1)); check(o.state == State::Starting && !o.needsAttention(), "starting is quiet"); }
    { In i = run(); i.locked = true; i.snrDb = 20; Out o = advise(i); check(o.state == State::Good && !o.needsAttention(), "locked and clean is good"); }
    { In i = run(); i.locked = true; i.snrDb = 3; Out o = advise(i); check(o.state == State::Weak && o.fix == Fix::AutoGain, "locked but weak"); }
    { In i = run(); i.locked = true; i.snrDb = 3; i.autoGain = true; Out o = advise(i); check(o.state == State::Weak && o.fix == Fix::None, "weak with Auto gain on: no repeat offer"); }
    { In i = run(); i.locked = true; Out o = advise(i); check(o.state == State::Good, "locked, SNR unknown: good"); }
    { In i = run(); i.rmsDbfs = -90; Out o = advise(i); check(o.state == State::TooLow && o.fix == Fix::AutoGain, "no lock and a very low level"); }
    { In i = run(); Out o = advise(i); check(o.state == State::NoSignal && o.fix == Fix::AutoGain, "no lock after the search time"); }
    { In i = run(5); Out o = advise(i); check(o.state == State::Starting, "too early for no signal"); }
    { In i = run(); i.radioHasGain = false; Out o = advise(i); check(o.state == State::NoSignal && o.fix == Fix::None, "no gain control: text only"); }
    { In i = run(); i.overload = true; Out o = advise(i); check(o.state == State::Overload && o.fix == Fix::AutoGain, "overload"); }
    { In i = run(); i.clipFraction = 0.01; i.locked = true; Out o = advise(i); check(o.state == State::Overload, "clipping beats a lock"); }
    { In i = run(); i.overload = true; i.autoGain = true; Out o = advise(i); check(o.state == State::Overload && o.fix == Fix::None, "overload with Auto gain: no repeat offer"); }
    { In i = run(); i.radioLoss = true; i.overload = true; Out o = advise(i); check(o.state == State::LosingSamples, "lost samples come first"); }
    { In i = run(); i.cpuLoss = true; Out o = advise(i); check(o.state == State::TooSlow, "slow computer"); }
    { In i = run(); i.lockKnown = false; Out o = advise(i); check(o.state == State::Unknown && !o.needsAttention(), "unknown mode: never says no signal"); }
    { In i = run(); i.lockKnown = false; i.overload = true; Out o = advise(i); check(o.state == State::Overload, "unknown mode still reports a measured overload"); }
    { In i = run(); i.lockKnown = false; i.rmsDbfs = -95; Out o = advise(i); check(o.state == State::TooLow, "unknown mode still reports a very low level"); }
    // every text is plain words: no exclamation mark, ends with a full stop or dots
    const In cases[] = {run(), run(1), run(20)};
    (void)cases;
    for (int k = 0; k < 8; k++) {
        In i = run(); if (k & 1) i.overload = true; if (k & 2) i.locked = true; if (k & 4) i.radioHasGain = false;
        Out o = advise(i);
        check(o.text && std::strchr(o.text, '!') == nullptr, "no exclamation marks");
        check((o.fix == Fix::None) == (o.fixLabel[0] == 0), "a fix has a label, no fix has none");
    }
    if (!fails) std::printf("next_step: OK\n");
    return fails ? 1 : 0;
}
