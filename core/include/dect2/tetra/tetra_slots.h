// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA traffic-slot selection (pure logic, no radio): which of the four timeslots of a carrier the voice path follows.
// Encrypted or unknown-state slots are never decoded whatever is chosen here (tetra_voice.h gates every burst first); this only
// says which slot is asked for and how the slots are described to the user.
#pragma once
#include <cstdint>

namespace dect2::tetra {

enum class SlotState : uint8_t { Idle = 0, Control, Voice, Encrypted };

// What is known of one downlink timeslot. talkgroup (GSSI) / issi are 0 when unknown; they come from the set-up PDU of a call that is not
// encrypted and are never set for an encrypted call. Local only (call table, slot display, this selector). lastHeardSec < 0: never heard.
struct SlotInfo {
    SlotState state = SlotState::Idle;
    uint32_t talkgroup = 0, issi = 0;
    double lastHeardSec = -1;
};

enum class SlotMode : uint8_t { Auto = 0, Slot, Talkgroup, Issi };

// The user's choice. Auto: the first slot with clear voice. Slot: slot 1..4. Talkgroup / Issi: whichever slot carries that id.
struct SlotSelect {
    SlotMode mode = SlotMode::Auto;
    int slot = 1;          // 1..4 for Slot
    uint32_t id = 0;       // talkgroup or ISSI for the other modes
};

constexpr int kFollowFirstClear = -1;   // result of pickSlot: let the receiver take the first slot with clear voice
constexpr int kFollowNothing = -2;      // result of pickSlot: nothing matches now, stay silent

// The timeslot (1..4) the voice path is asked for. Slot n: n, even while it is idle or encrypted (it is then simply not decoded, and the
// UI says why). Talkgroup / Issi: the slot that was heard most recently with the id and is not encrypted; kFollowNothing if none within
// holdSec of `nowSec`. A slot whose id matches but is encrypted is never returned.
inline int pickSlot(const SlotSelect& s, const SlotInfo (&slots)[4], double nowSec, double holdSec = 20.0) {
    switch (s.mode) {
    case SlotMode::Auto: return kFollowFirstClear;
    case SlotMode::Slot: return s.slot >= 1 && s.slot <= 4 ? s.slot : kFollowFirstClear;
    case SlotMode::Talkgroup:
    case SlotMode::Issi: {
        if (s.id == 0) return kFollowNothing;
        int best = kFollowNothing; double bestT = -1;
        for (int i = 0; i < 4; i++) {
            const SlotInfo& k = slots[i];
            const uint32_t have = s.mode == SlotMode::Talkgroup ? k.talkgroup : k.issi;
            if (have != s.id || k.state != SlotState::Voice || k.lastHeardSec < 0) continue;
            if (nowSec - k.lastHeardSec > holdSec || nowSec < k.lastHeardSec) continue;
            if (k.lastHeardSec > bestT) { bestT = k.lastHeardSec; best = i + 1; }
        }
        return best;
    }
    }
    return kFollowFirstClear;
}

// Folds one capture's view of a slot into its record: Idle and Control never erase a recent Voice / Encrypted record for holdSec (a talk
// spurt pause), and lastHeardSec moves only for Voice and Encrypted.
inline void updateSlot(SlotInfo& k, SlotState seen, double nowSec, double holdSec = 6.0) {
    if (seen == SlotState::Voice || seen == SlotState::Encrypted) { k.state = seen; k.lastHeardSec = nowSec; return; }
    if ((k.state == SlotState::Voice || k.state == SlotState::Encrypted) && k.lastHeardSec >= 0 && nowSec - k.lastHeardSec <= holdSec) return;
    k.state = seen;
}

inline const char* slotStateText(SlotState s) {
    switch (s) {
    case SlotState::Idle: return "idle";
    case SlotState::Control: return "control";
    case SlotState::Voice: return "voice";
    case SlotState::Encrypted: return "encrypted, muted";
    }
    return "idle";
}

} // namespace dect2::tetra
