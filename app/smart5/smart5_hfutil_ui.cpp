// HF utility decodes as automation events (docs/SMART5-UI-3.9.md, smart5b leftover): the decoders below know only their audio offset, not the
// radio frequency, so the interface thread sends "msg.<kind>" with the tuned frequency when a transmission starts or an event is decoded.
// The insight hub learns the broadcast schedules from them (overdue alerts, insight/schedule_watch.h); rules and integrations see them too.
// WEFAX charts, NAVTEX and ALE calls are sent by their receivers (marine_rx.cpp, ale_rx.cpp).
#include "smart5_ui.h"
#include "dect2/automation/engine.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <cmath>

namespace au = dect2::automation;

namespace {
struct St { int sitorState = 0, pactorState = 0; int64_t hfuUtc = 0; int hfuKind = -1; } H;
}

void smart5HfUtilTick(App& a) {
    if (!a.engine.running() || !au::wants(au::FMsg)) return;
    const double hz = a.freqMhz * 1e6;
    if (hz < 1e5 || hz >= 30e6) return;
    if (a.rx.standard == 120) {   // SITOR-B: a locked stream starts
        const int s = a.rx.sitor.state;
        if (s == 2 && H.sitorState != 2) au::emitMessage("sitor", "SITOR-B", hz, "", a.rx.sitor.text.size() > 80 ? a.rx.sitor.text.substr(a.rx.sitor.text.size() - 80) : a.rx.sitor.text, a.rx.sitor.snrDb);
        H.sitorState = s;
    }
    if (a.rx.standard == 204) {   // PACTOR-I: packets decoded again after a quiet spell
        const int s = a.rx.pactor.state;
        if (s == 2 && H.pactorState != 2) au::emitMessage("pactor", "PACTOR-I", hz, "", "transmission", NAN);
        H.pactorState = s;
    }
    const auto& u = a.rx.hfutil;
    if (a.rx.standard > dect2::kHfuFamilyFirst && a.rx.standard <= dect2::kHfuFamilyFirst + dect2::kHfuKinds) {   // STANAG, MIL-STD, ARQ ...: each new event
        if (u.kind != H.hfuKind) { H.hfuKind = u.kind; H.hfuUtc = u.events.empty() ? 0 : u.events.front().utc; }
        int64_t newest = H.hfuUtc;
        for (const auto& e : u.events) {   // newest first
            if (e.utc <= H.hfuUtc) break;
            au::emitMessage("hfutil", dect2::hfuName(u.kind), hz, "", e.text, NAN);
            newest = std::max(newest, e.utc);
        }
        H.hfuUtc = newest;
    }
}
