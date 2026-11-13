// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB announcements (ETSI EN 300 401 clause 8.1.6): which announcement, if any, a listener of a service should be switched to.
#pragma once
#include "dab_tel.h"

namespace dect2 {

struct DabAnnouncement {
    int cluster = 0;
    int type = 0;                  // announcement type (bit number in the flags): 0 alarm, 1 road traffic ... see dabAnnouncementTypeName
    unsigned flags = 0;            // all the types announced on the cluster
    int subId = -1;                // the sub-channel that carries it
};

const char* dabAnnouncementTypeName(int type);

// The types a listener may want to be told about, in the order of priority: the alarm first (type 0)
constexpr int kDabAnnTypes = 11;

// The announcement a listener of sub-channel `listenSub` should hear now: one of the types in `enabled` (bit n = type n), announced on a cluster
// the listened service belongs to (FIG 0/18) and supports that type for. The alarm clusters 0xFF and 0xFE (the test) reach every service.
// The regions of an announcement are not evaluated. False when there is none, or when it is on the sub-channel already listened to.
bool dabActiveAnnouncement(const DabEnsemble& e, int listenSub, unsigned enabled, DabAnnouncement& out);

} // namespace dect2
