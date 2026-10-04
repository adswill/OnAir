// AirPlay through AVFoundation: an AVPlayer plays the HLS address and the system hands it to the AirPlay device chosen in its device list
// (AVRoutePickerView). The player stays silent on this Mac: when a device takes over, the device plays the stream itself.
#include "airplay.h"
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <Cocoa/Cocoa.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <chrono>

namespace {
AVPlayer* gPlayer = nil;
AVRoutePickerView* gPicker = nil;
std::chrono::steady_clock::time_point gStart;
bool gEver = false;   // a device has been playing at least once during this cast
std::string gMessage;
}

namespace airplay {

bool available() { return true; }

void stop() {
    if (gPlayer) { [gPlayer pause]; gPlayer.allowsExternalPlayback = NO; [gPlayer replaceCurrentItemWithPlayerItem:nil]; gPlayer = nil; }
    if (gPicker) gPicker.player = nil;
    gEver = false;
}

void choose(GLFWwindow* window, float x, float y, float w, float h, const std::string& url) {
    stop();
    gMessage.clear();
    NSWindow* win = glfwGetCocoaWindow(window);
    if (!win) { gMessage = "no window"; return; }
    NSView* cv = win.contentView;
    if (!gPicker) {
        gPicker = [[AVRoutePickerView alloc] initWithFrame:NSMakeRect(0, 0, 28, 28)];
        [cv addSubview:gPicker];
    }
    // the picker is an ordinary view; it sits where the Cast button is, so that the device list opens right there
    gPicker.frame = NSMakeRect(x, cv.bounds.size.height - y - h, std::max(w, 28.f), std::max(h, 28.f));
    gPicker.alphaValue = 0.02;
    NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    AVPlayerItem* item = [AVPlayerItem playerItemWithURL:nsurl];
    gPlayer = [AVPlayer playerWithPlayerItem:item];
    gPlayer.allowsExternalPlayback = YES;
    gPlayer.volume = 0;   // nothing should come out of this Mac: a device that takes over plays the stream with its own sound
    gPicker.player = gPlayer;
    [gPlayer play];
    gStart = std::chrono::steady_clock::now();
    for (NSView* v in gPicker.subviews)
        if ([v isKindOfClass:[NSButton class]]) { [(NSButton*)v performClick:nil]; break; }
}

State state() {
    if (!gPlayer) return gMessage.empty() ? State::Idle : State::Failed;
    if (gPlayer.currentItem.status == AVPlayerItemStatusFailed) {
        NSString* d = gPlayer.currentItem.error.localizedDescription;
        gMessage = d ? d.UTF8String : "the stream could not be played";
        stop();
        return State::Failed;
    }
    if (gPlayer.externalPlaybackActive) { gEver = true; return State::Casting; }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
    if (gEver) { stop(); return State::Idle; }          // the device was switched away from this stream
    if (s > 45) { stop(); return State::Idle; }         // the list was closed without a choice
    return State::Choosing;
}

std::string message() { return gMessage; }

} // namespace airplay
