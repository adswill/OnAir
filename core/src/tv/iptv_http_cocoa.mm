// Internet TV: https on macOS through the system's own TLS (NSURLSession: the system trust store, no extra library, nothing to license).
// Plain http never comes here (iptv_http.cpp does it with sockets). The response body is handed over as it arrives.
#import <Foundation/Foundation.h>
#include "dect2/tv/iptv.h"
#include <condition_variable>
#include <deque>
#include <mutex>

@interface OnAirIptvDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
@property(nonatomic) std::mutex* mu;
@property(nonatomic) std::condition_variable* cv;
@property(nonatomic) std::deque<uint8_t>* buf;
@property(nonatomic) bool* finished;
@property(nonatomic) std::string* err;
@property(nonatomic) int* status;
@property(nonatomic) std::map<std::string, std::string>* headers;
@property(nonatomic) bool* gotHead;
@property(nonatomic) bool insecure;
@end
@implementation OnAirIptvDelegate
- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveResponse:(NSURLResponse*)resp completionHandler:(void (^)(NSURLSessionResponseDisposition))ch {
    std::lock_guard<std::mutex> l(*self.mu);
    if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
        NSHTTPURLResponse* h = (NSHTTPURLResponse*)resp;
        *self.status = (int)h.statusCode;
        for (NSString* k in h.allHeaderFields) {
            std::string key = [[k lowercaseString] UTF8String];
            (*self.headers)[key] = [[NSString stringWithFormat:@"%@", h.allHeaderFields[k]] UTF8String];
        }
    }
    *self.gotHead = true;
    self.cv->notify_all();
    ch(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveData:(NSData*)d {
    std::lock_guard<std::mutex> l(*self.mu);
    const uint8_t* p = (const uint8_t*)d.bytes;
    self.buf->insert(self.buf->end(), p, p + d.length);
    self.cv->notify_all();
}
- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)e {
    std::lock_guard<std::mutex> l(*self.mu);
    if (e) *self.err = [[e localizedDescription] UTF8String];
    *self.finished = true; *self.gotHead = true;
    self.cv->notify_all();
}
- (void)URLSession:(NSURLSession*)s didReceiveChallenge:(NSURLAuthenticationChallenge*)c completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))ch {
    if (self.insecure && [c.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust] && c.protectionSpace.serverTrust)
        ch(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:c.protectionSpace.serverTrust]);   // Advanced: ignore certificate errors, chosen by the user
    else ch(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}
@end

namespace dect2::iptv {
namespace {
class CocoaBody : public HttpBody {
public:
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint8_t> buf;
    bool finished = false, gotHead = false;
    std::string err;
    int status = 0;
    std::map<std::string, std::string> headers;
    NSURLSession* session = nil;
    NSURLSessionDataTask* task = nil;
    OnAirIptvDelegate* del = nil;
    int readMs = 15000;
    std::atomic<bool>* cancel = nullptr;
    ~CocoaBody() override { @autoreleasepool { [task cancel]; [session invalidateAndCancel]; } }
    long read(void* b, size_t n) override {
        std::unique_lock<std::mutex> l(mu);
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(readMs);
        while (buf.empty()) {
            if (finished) return err.empty() ? 0 : -1;
            if (cancel && cancel->load()) return -2;
            if (cv.wait_until(l, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(200))) == std::cv_status::timeout && std::chrono::steady_clock::now() >= end) return -1;
        }
        const size_t k = std::min(n, buf.size());
        std::copy(buf.begin(), buf.begin() + k, (uint8_t*)b);
        buf.erase(buf.begin(), buf.begin() + k);
        return (long)k;
    }
};
}

std::unique_ptr<HttpBody> httpOpenPlatform(const std::string& url, const HttpOptions& o, HttpResult& r, bool& handled) {
    handled = true;
    @autoreleasepool {
        NSURL* nu = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!nu) { r.error = "not a valid address"; return nullptr; }
        auto b = std::make_unique<CocoaBody>();
        b->readMs = o.readTimeoutMs; b->cancel = o.cancel;
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nu];
        req.timeoutInterval = o.connectTimeoutMs / 1000.0;
        [req setValue:[NSString stringWithUTF8String:(o.userAgent.empty() ? "OnAir" : o.userAgent.c_str())] forHTTPHeaderField:@"User-Agent"];
        if (!o.referer.empty()) [req setValue:[NSString stringWithUTF8String:o.referer.c_str()] forHTTPHeaderField:@"Referer"];
        for (auto& h : o.headers) [req setValue:[NSString stringWithUTF8String:h.second.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:h.first.c_str()]];
        OnAirIptvDelegate* d = [OnAirIptvDelegate new];
        d.mu = &b->mu; d.cv = &b->cv; d.buf = &b->buf; d.finished = &b->finished; d.err = &b->err; d.status = &b->status; d.headers = &b->headers; d.gotHead = &b->gotHead; d.insecure = o.insecureTls;
        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = o.readTimeoutMs / 1000.0;
        cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        b->del = d;
        b->session = [NSURLSession sessionWithConfiguration:cfg delegate:d delegateQueue:nil];
        b->task = [b->session dataTaskWithRequest:req];
        [b->task resume];
        {
            std::unique_lock<std::mutex> l(b->mu);
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.connectTimeoutMs + o.readTimeoutMs);
            while (!b->gotHead) {
                if (o.cancel && o.cancel->load()) { r.error = "stopped"; return nullptr; }
                if (b->cv.wait_for(l, std::chrono::milliseconds(200)) == std::cv_status::timeout && std::chrono::steady_clock::now() >= end) { r.error = "the server did not answer in time"; return nullptr; }
            }
            if (b->status == 0) { r.error = b->err.empty() ? "no answer from the server" : b->err; return nullptr; }
        }
        r.status = b->status; r.headers = b->headers; r.finalUrl = url;
        if (r.headers.count("content-type")) r.contentType = r.headers["content-type"];
        if (r.headers.count("content-length")) r.contentLength = atoll(r.headers["content-length"].c_str());
        if (r.status < 200 || r.status >= 300) {
            r.error = "HTTP " + std::to_string(r.status) + (r.status == 403 ? " Forbidden: the server refuses this request (try another User-Agent or Referer under Advanced, the link may have expired, or the stream is not available in your country)" : r.status == 404 ? " Not Found: the stream no longer exists at this address" : "");
            return nullptr;
        }
        return b;
    }
}
} // namespace dect2::iptv
