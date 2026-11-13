// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// macOS side of the power policy: the power source (IOKit), the thermal state (NSProcessInfo) and the App Nap hold.
#include "dect2/power/power.h"
#import <Foundation/Foundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mutex>

namespace dect2::power {

Probe probePlatform() {
    Probe p;
    @autoreleasepool {
        CFTypeRef info = IOPSCopyPowerSourcesInfo();
        if (info) {
            CFStringRef type = IOPSGetProvidingPowerSourceType(info);   // "Battery Power" / "AC Power" / "UPS Power"
            if (type && CFStringCompare(type, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo) p.onBattery = true;
            CFRelease(info);
        }
        const NSProcessInfoThermalState t = [[NSProcessInfo processInfo] thermalState];
        p.thermalSerious = (t == NSProcessInfoThermalStateSerious || t == NSProcessInfoThermalStateCritical);
    }
    return p;
}

namespace {
std::mutex gMu;
id gActivity = nil;
}

// While receiving: the app is doing user-visible work and must not be napped or have its timers coalesced hard. The system may still sleep
// the display and the machine (the AllowingIdleSystemSleep variant). With no receiver running nothing is held, so App Nap can apply.
void activityBegin() {
    std::lock_guard<std::mutex> lk(gMu);
    if (gActivity) return;
    gActivity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"OnAir is receiving"];
}
void activityEnd() {
    std::lock_guard<std::mutex> lk(gMu);
    if (!gActivity) return;
    [[NSProcessInfo processInfo] endActivity:gActivity];
    gActivity = nil;
}

}  // namespace dect2::power
