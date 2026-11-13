// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// State that exists once per receiver (RX 1 .. RX 4): one object for each id, shared by nothing else. An id outside the range gets a slot of
// its own rather than another receiver's. Pure, tested by tests/state/test_per_receiver.cpp (#450).
#pragma once

template <class T, int N> T& perReceiver(int rxId) {
    static T slots[N + 1];
    return slots[rxId >= 1 && rxId <= N ? rxId : 0];
}
