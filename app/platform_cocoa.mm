#include "platform.h"
#import <Cocoa/Cocoa.h>

namespace plat {

std::string openFileDialog() {
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.canChooseFiles = YES; p.canChooseDirectories = NO; p.allowsMultipleSelection = NO;
    if ([p runModal] == NSModalResponseOK) return std::string(p.URL.path.UTF8String);
    return {};
}

std::string saveFileDialog(const char* name) {
    NSSavePanel* p = [NSSavePanel savePanel];
    p.nameFieldStringValue = [NSString stringWithUTF8String:name];
    if ([p runModal] == NSModalResponseOK) return std::string(p.URL.path.UTF8String);
    return {};
}

namespace {
struct DefaultsPrefs : Prefs {
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    NSString* k(const char* key) { return [NSString stringWithUTF8String:key]; }
    bool has(const char* key) override { return [d objectForKey:k(key)] != nil; }
    double getD(const char* key, double def) override { return has(key) ? [d doubleForKey:k(key)] : def; }
    long getI(const char* key, long def) override { return has(key) ? (long)[d integerForKey:k(key)] : def; }
    bool getB(const char* key, bool def) override { return has(key) ? [d boolForKey:k(key)] : def; }
    std::string getS(const char* key, const std::string& def) override { NSString* s = [d stringForKey:k(key)]; return s ? std::string(s.UTF8String) : def; }
    void setD(const char* key, double v) override { [d setDouble:v forKey:k(key)]; }
    void setI(const char* key, long v) override { [d setInteger:v forKey:k(key)]; }
    void setB(const char* key, bool v) override { [d setBool:v forKey:k(key)]; }
    void setS(const char* key, const std::string& v) override { [d setObject:[NSString stringWithUTF8String:v.c_str()] forKey:k(key)]; }
    std::vector<Channel> getChannels() override {
        std::vector<Channel> out;
        for (NSDictionary* c in [d arrayForKey:@"channels"]) {
            Channel sc;
            sc.freqMhz = [c[@"f"] doubleValue]; sc.bwMhz = [c[@"bw"] doubleValue];
            sc.name = [c[@"name"] UTF8String] ?: ""; sc.mode = [c[@"mode"] UTF8String] ?: "";
            sc.snrDb = [c[@"snr"] floatValue]; sc.nServices = [c[@"n"] intValue]; sc.favourite = [c[@"fav"] boolValue];
            if (sc.freqMhz > 0) out.push_back(sc);
        }
        return out;
    }
    void setChannels(const std::vector<Channel>& ch) override {
        NSMutableArray* arr = [NSMutableArray array];
        for (auto& c : ch)
            [arr addObject:@{@"f": @(c.freqMhz), @"bw": @(c.bwMhz), @"name": [NSString stringWithUTF8String:c.name.c_str()], @"mode": [NSString stringWithUTF8String:c.mode.c_str()], @"snr": @(c.snrDb), @"n": @(c.nServices), @"fav": @(c.favourite)}];
        [d setObject:arr forKey:@"channels"];
    }
};
}

void showFatalError(const char* title, const char* text) { fprintf(stderr, "%s: %s\n", title, text); }

Prefs& prefs() { static DefaultsPrefs p; return p; }

std::vector<std::string> uiFontCandidates() { return {"/System/Library/Fonts/Helvetica.ttc"}; }
std::vector<std::string> monoFontCandidates() { return {"/System/Library/Fonts/Menlo.ttc"}; }

}
