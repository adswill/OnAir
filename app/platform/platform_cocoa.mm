#include "platform.h"
#include "datadir_ptr.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

std::string defaultDataDir() {
    NSArray* a = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (!a.count) return cacheDir();
    NSString* d = [a[0] stringByAppendingPathComponent:@"OnAir"];
    [[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d.UTF8String;
}

std::string dataDir() {
    // DECT2_DATA_DIR: another folder for everything OnAir writes (test runs keep away from the user's own data)
    if (const char* o = getenv("DECT2_DATA_DIR"); o && *o) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(o)), ec);
        return o;
    }
    const std::string def = defaultDataDir();
    const std::string chosen = readDataDirPointer(def);   // the folder picked in the settings (issue #383)
    return chosen.empty() ? def : chosen;
}
bool setDataDir(const std::string& path) { return writeDataDirPointer(defaultDataDir(), path); }

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

bool fetchUrl(const std::string& url, const std::string& file, const std::string& userAgent) {
    auto q = [](const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; };
    const std::string cmd = "curl -fsSL --max-time 12 -A " + q(userAgent) + " -o " + q(file) + " " + q(url) + " 2>/dev/null";
    return system(cmd.c_str()) == 0;
}

static bool postImpl(const std::string& url, const std::string& json, const std::string& userAgent, std::string* reply) {
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!u) return false;
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:12];
        req.HTTPMethod = @"POST";
        req.HTTPBody = [NSData dataWithBytes:json.data() length:json.size()];
        [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        [req setValue:[NSString stringWithUTF8String:userAgent.c_str()] forHTTPHeaderField:@"User-Agent"];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block bool ok = false;
        [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
            NSHTTPURLResponse* h = (NSHTTPURLResponse*)resp;
            ok = err == nil && h && h.statusCode >= 200 && h.statusCode < 300;
            if (reply && data) reply->assign((const char*)data.bytes, std::min<size_t>(data.length, 65536));
            dispatch_semaphore_signal(sem);
        }] resume];
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 13 * NSEC_PER_SEC)) != 0) return false;
        return ok;
    }
}

bool postRequest(const std::string& url, const std::string& body, const std::string& contentType, const std::vector<std::string>& headers, const std::string& userAgent) {
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!u) return false;
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:12];
        req.HTTPMethod = @"POST";
        req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        [req setValue:[NSString stringWithUTF8String:(contentType.empty() ? "application/json" : contentType.c_str())] forHTTPHeaderField:@"Content-Type"];
        [req setValue:[NSString stringWithUTF8String:userAgent.c_str()] forHTTPHeaderField:@"User-Agent"];
        for (const std::string& h : headers) {
            const size_t c = h.find(':');
            if (c == std::string::npos || c == 0) continue;
            size_t v = c + 1;
            while (v < h.size() && h[v] == ' ') v++;
            NSString* k = [NSString stringWithUTF8String:h.substr(0, c).c_str()];
            NSString* val = [NSString stringWithUTF8String:h.substr(v).c_str()];
            if (k && val) [req setValue:val forHTTPHeaderField:k];
        }
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block bool ok = false;
        [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData*, NSURLResponse* resp, NSError* err) {
            NSHTTPURLResponse* h = (NSHTTPURLResponse*)resp;
            ok = err == nil && h && h.statusCode >= 200 && h.statusCode < 300;
            dispatch_semaphore_signal(sem);
        }] resume];
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 13 * NSEC_PER_SEC)) != 0) return false;
        return ok;
    }
}

HttpResult httpRequest(const std::string& method, const std::string& url, const std::string& body, const std::string& userAgent, int timeoutSec) {
    HttpResult r;
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!u) { r.net = 4; r.detail = "bad URL"; return r; }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:std::max(1, timeoutSec)];
        req.HTTPMethod = [NSString stringWithUTF8String:method.c_str()];
        if (method == "POST") {
            req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
            [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        }
        [req setValue:[NSString stringWithUTF8String:userAgent.c_str()] forHTTPHeaderField:@"User-Agent"];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block HttpResult res;
        [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
            NSHTTPURLResponse* h = (NSHTTPURLResponse*)resp;
            if (err) {
                switch (err.code) {
                    case NSURLErrorCannotFindHost: case NSURLErrorDNSLookupFailed: res.net = 1; break;
                    case NSURLErrorNotConnectedToInternet: case NSURLErrorCannotConnectToHost: case NSURLErrorNetworkConnectionLost: res.net = 2; break;
                    case NSURLErrorTimedOut: res.net = 3; break;
                    default: res.net = 4; break;
                }
                const char* d = err.localizedDescription.UTF8String;
                res.detail = std::string(d ? d : "network error") + " (" + std::to_string((long)err.code) + ")";
            } else if (h) {
                res.status = (int)h.statusCode;
                if (data) res.body.assign((const char*)data.bytes, std::min<size_t>(data.length, 65536));
            } else { res.net = 4; res.detail = "no HTTP answer"; }
            dispatch_semaphore_signal(sem);
        }] resume];
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutSec + 2) * NSEC_PER_SEC)) != 0) { r.net = 3; r.detail = "timed out"; return r; }
        r = res;
    }
    return r;
}
bool postJson(const std::string& url, const std::string& json, const std::string& userAgent) { return postImpl(url, json, userAgent, nullptr); }
bool postJsonReply(const std::string& url, const std::string& json, const std::string& userAgent, std::string& reply) { reply.clear(); return postImpl(url, json, userAgent, &reply); }

std::string openFileDialog() {
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.canChooseFiles = YES; p.canChooseDirectories = NO; p.allowsMultipleSelection = NO;
    if ([p runModal] == NSModalResponseOK) return std::string(p.URL.path.UTF8String);
    return {};
}

std::string openFolderDialog() {
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.canChooseFiles = NO; p.canChooseDirectories = YES; p.canCreateDirectories = YES; p.allowsMultipleSelection = NO;
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
// Never nil: stringWithUTF8String gives nil for bytes that are not UTF-8 (a service name damaged by bit errors), and nil in the dictionary
// literal of setChannels() throws. Such bytes are kept as Latin-1.
NSString* nsText(const std::string& s) {
    if (NSString* u = [NSString stringWithUTF8String:s.c_str()]) return u;
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSISOLatin1StringEncoding] ?: @"";
}

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
    void setS(const char* key, const std::string& v) override { [d setObject:nsText(v) forKey:k(key)]; }
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
            [arr addObject:@{@"f": @(c.freqMhz), @"bw": @(c.bwMhz), @"name": nsText(c.name), @"mode": nsText(c.mode), @"snr": @(c.snrDb), @"n": @(c.nServices), @"fav": @(c.favourite)}];
        [d setObject:arr forKey:@"channels"];
    }
    std::vector<dect2::backup::PrefItem> dumpAll() override {
        std::vector<dect2::backup::PrefItem> o;
        NSString* domain = NSBundle.mainBundle.bundleIdentifier ?: NSProcessInfo.processInfo.processName;
        NSDictionary* all = [d persistentDomainForName:domain];
        for (NSString* key in all) {
            id v = all[key];
            if ([key isEqualToString:@"channels"] || key.UTF8String == nullptr) continue;
            if ([v isKindOfClass:[NSString class]]) o.push_back({key.UTF8String, 'S', std::string(((NSString*)v).UTF8String ?: "")});
            else if ([v isKindOfClass:[NSNumber class]]) {
                NSNumber* n = v;
                if (CFGetTypeID((__bridge CFTypeRef)n) == CFBooleanGetTypeID()) o.push_back({key.UTF8String, 'B', n.boolValue ? "1" : "0"});
                else if (!strcmp(n.objCType, "d") || !strcmp(n.objCType, "f")) { char b[64]; snprintf(b, sizeof b, "%.17g", n.doubleValue); o.push_back({key.UTF8String, 'D', b}); }
                else o.push_back({key.UTF8String, 'I', std::to_string(n.longLongValue)});
            }
        }
        return o;
    }
    std::string setAside() override {
        NSString* domain = NSBundle.mainBundle.bundleIdentifier ?: NSProcessInfo.processInfo.processName;
        NSDictionary* old = [d persistentDomainForName:domain];
        NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/OnAir"];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString* file = [dir stringByAppendingPathComponent:@"settings-crashed.plist"];
        const bool kept = old && [old writeToURL:[NSURL fileURLWithPath:file] error:nil];
        [d removePersistentDomainForName:domain];
        return kept ? std::string(file.UTF8String) : "";
    }
};
}

// also as an alert: started from the Finder, a message on stderr is never seen and the app just disappears. May run before main()
// (the processor check in main.cpp), so it makes the application object itself.
void showFatalError(const char* title, const char* text) {
    fprintf(stderr, "%s: %s\n", title, text);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert* a = [[NSAlert alloc] init];
        a.alertStyle = NSAlertStyleCritical;
        a.messageText = [NSString stringWithUTF8String:title];
        a.informativeText = [NSString stringWithUTF8String:text];
        [a runModal];
    }
}

Prefs& basePrefs() { static DefaultsPrefs p; return p; }

std::vector<std::string> uiFontCandidates() { return {"/System/Library/Fonts/Helvetica.ttc"}; }
std::vector<std::string> cjkFontCandidates() { return {"/System/Library/Fonts/AppleSDGothicNeo.ttc", "/Library/Fonts/AppleGothic.ttf",
                                                          "/System/Library/Fonts/Supplemental/AppleGothic.ttf", "/System/Library/Fonts/PingFang.ttc",
                                                          "/System/Library/Fonts/STHeiti Medium.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                                          "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc"}; }
std::vector<std::string> monoFontCandidates() { return {"/System/Library/Fonts/Menlo.ttc"}; }
std::string uiLanguage() {
    NSArray<NSString*>* l = [NSLocale preferredLanguages];   // the order of System Settings > Language & Region
    return l.count ? std::string([l[0] UTF8String]) : std::string("en");
}

}
