// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The RF camera section of the direction finder window (rfcam_ui.cpp): the bearing spectrum as a heat map over a picture.
#pragma once
#include "dect2/df/rfcam.h"
struct App;
bool rfCamEnabled();                                   // the section is switched on: the direction finder then reads 4096 snapshots at about 8 Hz
// Offer a new calibrated frame (called by the direction finder after every read); no-op when the camera is off
void rfCamFeed(const dect2::df::Frame& x, const dect2::df::Array& arr, double centerHz, double sampleRate, const dect2::df::Options& df);
void rfCamSection(App& a);                             // inside the direction finder window
