// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// macOS parts of power.h: whether the window can be seen, and the App Nap activity while a receiver runs.
#include "power.h"
#import <Cocoa/Cocoa.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace power {

bool windowOccluded(GLFWwindow* window) {
    if (!window) return false;
    NSWindow* w = glfwGetCocoaWindow(window);
    if (!w) return false;
    return ([w occlusionState] & NSWindowOcclusionStateVisible) == 0;
}

// App Nap slows the timers of an app whose windows cannot be seen; a receiver must keep pace with the radio, so while one runs the app
// holds a user-initiated activity (the screen and the computer may still sleep when idle). Without a receiver the app may nap.
void setReceiving(bool on) {
    static id activity = nil;
    if (on == (activity != nil)) return;
    if (on) activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep reason:@"Receiving a radio signal"];
    else { [[NSProcessInfo processInfo] endActivity:activity]; activity = nil; }
}

}   // namespace power
