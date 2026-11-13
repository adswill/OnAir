// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What each mode remembers (App::famFreq, App::famGain) is an array of kNumSlots. The family numbers of the Mode Builder (19999), Plugins (999) and the
// plugin and custom modes (from 1000, from 20000) are far past its end, so a slot number has to be checked before it is used (#465).
#pragma once
#include <cstddef>

namespace modeslot {
template <class T, std::size_t N> T* at(T (&arr)[N], int slot) { return slot >= 0 && (std::size_t)slot < N ? &arr[slot] : nullptr; }
}   // namespace modeslot
