// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ screens (the shared ones are in app/fsq/kbd33_ui.h).
#include "../fsq/kbd33_ui.h"
#include "dect2/fsq/fsq_rx.h"

namespace {

struct Tr {
    using Preset = dect2::FsqPreset;
    static const char* name() { return "FSQ"; }
    static const char* key() { return "fsq"; }
    static constexpr bool ifkpFlag = false;
    static bool ifkp() { return ifkpFlag; }
    static int standard() { return 115; }
    static const dect2::Kbd33Telemetry& tel(const App& a) { return a.rx.fsq; }
    static const Preset* presets(int& n) { return dect2::fsqPresets(n); }
    static const char* speedTip() { return tr("FSQ speeds: 1.5, 2, 3, 4.5 and 6 baud. Automatic looks for all five; the band is 290 Hz wide at every speed."); }
    static void setSpeed(App& a, int s) { a.engine.fsq().setSpeed(s); }
    static void setCall(App& a, const char* c) { a.engine.fsq().setMyCall(c); }
    static void setListen(App& a, bool on) { a.engine.fsq().setListen(on); }
    static void setSound(App& a, float v, bool m) { a.engine.fsq().setVolume(v); a.engine.fsq().setMuted(m); }
    static void clear(App& a) { a.engine.fsq().clearResults(); }
};

} // namespace

extern const ModeUi kFsqUi;
const ModeUi kFsqUi = kbd33ui::Ui<Tr>::make();
