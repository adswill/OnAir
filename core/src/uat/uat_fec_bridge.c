// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Reed-Solomon FEC for UAT downlink frames (from dump978/fec.c, GPL).
#include <stdint.h>
#include "fec.h"
#include "dect2/uat/uat_frame.h"

void uat_fec_init(void) { init_fec(); }

int uat_correct_downlink(uint8_t* to, int* rs_errors) { return correct_adsb_frame(to, rs_errors); }

int uat_correct_uplink(const uint8_t* from, uint8_t* to, int* rs_errors) { return correct_uplink_frame((uint8_t*)from, to, rs_errors); }
