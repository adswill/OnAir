#include "platform.h"
#import <Cocoa/Cocoa.h>
#import <CoreLocation/CoreLocation.h>
#import <ImageIO/ImageIO.h>

@interface OnAirLocator : NSObject <CLLocationManagerDelegate>
@property(nonatomic, strong) CLLocationManager* mgr;
@property(nonatomic) int state;
@property(nonatomic) double lat, lon, acc;
@property(nonatomic, copy) NSString* msg;
@end
@implementation OnAirLocator
- (void)locationManager:(CLLocationManager*)m didUpdateLocations:(NSArray<CLLocation*>*)locs {
    CLLocation* l = locs.lastObject;
    if (!l) return;
    self.lat = l.coordinate.latitude; self.lon = l.coordinate.longitude; self.acc = l.horizontalAccuracy; self.state = 2;
    [m stopUpdatingLocation];
}
- (void)locationManager:(CLLocationManager*)m didFailWithError:(NSError*)e {
    if (e.code == kCLErrorLocationUnknown) return;   // not yet: it keeps trying
    self.msg = e.code == kCLErrorDenied ? @"location access was not allowed (System Settings > Privacy & Security > Location Services)" : e.localizedDescription;
    self.state = 3;
    [m stopUpdatingLocation];
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)m {
    if (m.authorizationStatus == kCLAuthorizationStatusDenied || m.authorizationStatus == kCLAuthorizationStatusRestricted) {
        self.msg = @"location access was not allowed (System Settings > Privacy & Security > Location Services)"; self.state = 3;
    }
}
@end

namespace plat {

static OnAirLocator* gLoc = nil;
static double gLocStart = 0;

void locateStart() {
    if (!gLoc) { gLoc = [OnAirLocator new]; gLoc.mgr = [CLLocationManager new]; gLoc.mgr.delegate = gLoc; gLoc.mgr.desiredAccuracy = kCLLocationAccuracyHundredMeters; }
    gLoc.state = 1; gLoc.msg = @"";
    gLocStart = CFAbsoluteTimeGetCurrent();
    if (![CLLocationManager locationServicesEnabled]) { gLoc.msg = @"location services are switched off"; gLoc.state = 3; return; }
    [gLoc.mgr requestWhenInUseAuthorization];
    [gLoc.mgr startUpdatingLocation];
}

int locateState(double& lat, double& lon, double& acc, std::string& msg) {
    if (!gLoc) return 0;
    if (gLoc.state == 1 && CFAbsoluteTimeGetCurrent() - gLocStart > 20) { gLoc.msg = @"no answer from the location service (the program may not be allowed to ask: run it from the app bundle, or enter the position by hand)"; gLoc.state = 3; [gLoc.mgr stopUpdatingLocation]; }
    lat = gLoc.lat; lon = gLoc.lon; acc = gLoc.acc; msg = gLoc.msg ? gLoc.msg.UTF8String : "";
    return gLoc.state;
}

std::string cacheDir() {
    NSArray* a = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString* d = [(a.count ? a[0] : NSTemporaryDirectory()) stringByAppendingPathComponent:@"OnAir"];
    [[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d.UTF8String;
}

bool decodeImage(const std::string& path, int& w, int& h, std::vector<uint32_t>& rgba) {
    @autoreleasepool {
        NSURL* u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)u, nullptr);
        if (!src) return false;
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        CFRelease(src);
        if (!img) return false;
        w = (int)CGImageGetWidth(img); h = (int)CGImageGetHeight(img);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { CGImageRelease(img); return false; }
        rgba.assign((size_t)w * h, 0);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef cx = CGBitmapContextCreate(rgba.data(), w, h, 8, (size_t)w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (!cx) { CGImageRelease(img); return false; }
        CGContextDrawImage(cx, CGRectMake(0, 0, w, h), img);
        CGContextRelease(cx);
        CGImageRelease(img);
        return true;
    }
}

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
