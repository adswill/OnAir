// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Which receiver (Engine) has which radio open, so one radio is never opened twice in a process (multi-SDR, docs/multi-sdr.md).
#pragma once
#include "dect2/source/source.h"
#include <string>

namespace dect2 {

// "" for the test signal and files (any number of receivers may use them); else a key that is the same for the same physical radio
std::string deviceClaimKey(const DeviceInfo& d);
// Claims the radio for receiver `owner` (1-based). False when another receiver has it: *holder is that receiver's number.
bool claimDevice(const DeviceInfo& d, int owner, int* holder = nullptr);
void releaseDevice(int owner);                          // everything the receiver holds
int deviceHolder(const DeviceInfo& d);                  // 0 = free, else the receiver number
constexpr int kTxClaimOwner = 1000;                     // the transmitter (tx.h) holds its radio under this number; receivers are 1 to 99
std::string deviceHolderText(int holder);               // "RX 2" or "the transmitter", for messages

}
