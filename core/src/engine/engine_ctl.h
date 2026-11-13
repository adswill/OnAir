// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One lock around Engine::start, stop, retune and retuneReset (all engines): the app's tuning thread (app/state/tune_worker.cpp) retunes
// while the UI may stop or start the radio, and a stop must never meet a retune half done. Recursive: a retune that changes the sample
// rate restarts the engine from inside itself.
#pragma once
#include <mutex>

namespace dect2 {
inline std::recursive_mutex& engineControlMutex() { static std::recursive_mutex m; return m; }
}
