// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Power policy of the app: is the machine on battery, is it hot, is the window hidden, and what that means for the update rates.
//
//   dect2::power::settings()          the user's choices (Low power mode: Off / Auto / On; Save power when hidden: Off / On battery / Always)
//   dect2::power::lowPower()          true when the reduced update rates should apply now (manual On, or Auto with battery or serious heat)
//   dect2::power::spectrumRateScale() 1.0 normally, 0.5 in Low power mode: multiply the spectrum / telemetry / waterfall rates by it
//   dect2::power::maxFftSize(n)       n, or half of it in Low power mode (only for n >= 2048)
//   dect2::power::pauseWhenHidden()   true when the window is minimised or hidden AND the user's setting asks to pause background work
//   dect2::power::setThreadQos(q)     macOS QoS class of the calling thread (Utility for DSP workers, Background for logging and scanners);
//                                     below normal priority on Windows, a no-op elsewhere. namedThread() applies it by thread name.
//   dect2::power::ReceivingActivity   RAII: while at least one exists, App Nap is held off; with none, macOS may nap the idle app
//
// The battery / thermal state is read at most every 20 s and cached, so the queries are cheap enough for the draw loop. Nothing here starts a thread.
#pragma once
#include <string>

namespace dect2::power {

enum class LowPowerMode { Off = 0, Auto = 1, On = 2 };
enum class HiddenPolicy { Off = 0, OnBattery = 1, Always = 2 };
enum class Qos { Interactive, Utility, Background };

struct Settings {
    LowPowerMode lowPower = LowPowerMode::Auto;
    HiddenPolicy saveWhenHidden = HiddenPolicy::OnBattery;
};

Settings settings();
void setSettings(const Settings& s);

// Raw machine state (cached for 20 s). onBattery is false on desktops and when the state cannot be read.
bool onBattery();
bool thermalSerious();
void refreshNow();   // forget the cache (tests, and when the user changes the settings)

bool lowPower();
double spectrumRateScale();
int maxFftSize(int n);

void setAppHidden(bool hidden);
bool appHidden();
bool pauseWhenHidden();

void setThreadQos(Qos q);
// The QoS class a thread of this name should get (by its onair-<what> name); Interactive means "leave it alone"
Qos qosForThreadName(const std::string& name);

// Platform probe (uncached), separately testable
struct Probe { bool onBattery = false; bool thermalSerious = false; };
Probe probePlatform();

class ReceivingActivity {
public:
    ReceivingActivity();
    ~ReceivingActivity();
    ReceivingActivity(const ReceivingActivity&) = delete;
    ReceivingActivity& operator=(const ReceivingActivity&) = delete;
};
int receivingActivityCount();

}  // namespace dect2::power
