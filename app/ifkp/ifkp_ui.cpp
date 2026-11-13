// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IFKP screens (the shared ones are in app/fsq/kbd33_ui.h).
#include "../fsq/kbd33_ui.h"
#include "dect2/ifkp/ifkp_rx.h"

namespace {

struct Tr {
    using Preset = dect2::IfkpPreset;
    static const char* name() { return "IFKP"; }
    static const char* key() { return "ifkp"; }
    static constexpr bool ifkpFlag = true;
    static bool ifkp() { return ifkpFlag; }
    static int standard() { return 118; }
    static const dect2::Kbd33Telemetry& tel(const App& a) { return a.rx.ifkp; }
    static const Preset* presets(int& n) { return dect2::ifkpPresets(n); }
    static const char* speedTip() { return tr("IFKP speeds: 0.5, 1.0 and 2.0 (1.95, 3.9 and 7.8 baud). Automatic looks for all three; the band is 387 Hz wide at every speed."); }
    static void setSpeed(App& a, int s) { a.engine.ifkp().setSpeed(s); }
    static void setCall(App& a, const char* c) { a.engine.ifkp().setMyCall(c); }
    static void setListen(App& a, bool on) { a.engine.ifkp().setListen(on); }
    static void setSound(App& a, float v, bool m) { a.engine.ifkp().setVolume(v); a.engine.ifkp().setMuted(m); }
    static void clear(App& a) { a.engine.ifkp().clearResults(); }
};

} // namespace

extern const ModeUi kIfkpUi;
const ModeUi kIfkpUi = kbd33ui::Ui<Tr>::make();
