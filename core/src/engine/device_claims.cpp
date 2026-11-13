// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/engine/device_claims.h"
#include "dect2/source/synth_group.h"
#include <map>
#include <mutex>

namespace dect2 {
namespace {
std::mutex gMu;
std::map<std::string, int> gHeld;
}

std::string deviceClaimKey(const DeviceInfo& d) {
    if (!d.isRadio() && !d.isAudio()) return "";
    std::string id = !d.serial.empty() ? d.serial : !d.nativeArgs.empty() ? d.nativeArgs : !d.soapyArgs.empty() ? d.soapyArgs : d.name;
    // RTL-SDR dongles are sold with one shared serial (00000001): the second one then looked "in use by RX 1" (#328); the listing position tells them apart
    if (d.board == "rtlsdr" && !d.serial.empty() && !d.nativeArgs.empty()) id += "@" + d.nativeArgs;
    return std::to_string((int)d.kind) + "|" + d.board + "|" + id;
}

bool claimDevice(const DeviceInfo& d, int owner, int* holder) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return true;
    // a radio group holds its members too: none of them may be open elsewhere, and none can be opened while the group runs
    std::vector<std::string> keys{k};
    if (d.kind == DeviceInfo::Group) for (const auto& m : synthGroupMembers(d)) keys.push_back(deviceClaimKey(m));
    std::lock_guard<std::mutex> lk(gMu);
    for (const auto& kk : keys) {
        auto it = gHeld.find(kk);
        if (it != gHeld.end() && it->second != owner) { if (holder) *holder = it->second; return false; }
    }
    for (const auto& kk : keys) gHeld[kk] = owner;
    return true;
}

void releaseDevice(int owner) {
    std::lock_guard<std::mutex> lk(gMu);
    for (auto it = gHeld.begin(); it != gHeld.end();) it = it->second == owner ? gHeld.erase(it) : std::next(it);
}

std::string deviceHolderText(int holder) { return holder >= kTxClaimOwner ? "the transmitter" : "RX " + std::to_string(holder); }

int deviceHolder(const DeviceInfo& d) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return 0;
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gHeld.find(k);
    return it == gHeld.end() ? 0 : it->second;
}

}
