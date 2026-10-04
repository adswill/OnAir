// Frame interpolation with Apple's machine-learning frame rate conversion (VideoToolbox VTFrameProcessor, macOS 15.4 and later).
// It runs on the GPU / Neural Engine and needs no model download: the model ships with the system.
#include "dect2/conceal.h"
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cstring>
#include <mutex>

// The frame-rate-conversion API is in the macOS 15.4 SDK (Xcode 16.3) and later. Building with an older SDK (some CI images) still works:
// the machine-learning path is then left out and the portable motion search is used.
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150400 && __has_include(<VideoToolbox/VTFrameProcessor.h>)
#define DECT2_APPLE_ML 1
#else
#define DECT2_APPLE_ML 0
#endif

namespace dect2 {

#if DECT2_APPLE_ML

namespace {

std::mutex gMu;
VTFrameProcessor* gProc = nil;
VTFrameRateConversionConfiguration* gCfg = nil;
int gW = 0, gH = 0;
bool gFailed = false;

CVPixelBufferRef makeBuffer(int w, int h, NSDictionary* attrs, OSType fmt) {
    NSMutableDictionary* a = attrs ? [attrs mutableCopy] : [NSMutableDictionary dictionary];
    a[(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey] = @{};
    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferCreate(kCFAllocatorDefault, (size_t)w, (size_t)h, fmt, (__bridge CFDictionaryRef)a, &pb) != kCVReturnSuccess) return nullptr;
    return pb;
}

void fillBuffer(CVPixelBufferRef pb, const VideoFrame& f) {
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* y = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    for (int j = 0; j < f.h; j++) std::memcpy(y + (size_t)j * ys, &f.y[(size_t)j * f.w], (size_t)f.w);
    uint8_t* uv = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t us = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (int j = 0; j < f.h / 2; j++) std::memcpy(uv + (size_t)j * us, &f.uv[(size_t)j * f.w], (size_t)f.w);
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

void readBuffer(CVPixelBufferRef pb, VideoFrame& f, int w, int h) {
    f.w = w; f.h = h;
    f.y.resize((size_t)w * h); f.uv.resize((size_t)w * h / 2);
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t* y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    for (int j = 0; j < h; j++) std::memcpy(&f.y[(size_t)j * w], y + (size_t)j * ys, (size_t)w);
    const uint8_t* uv = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t us = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (int j = 0; j < h / 2; j++) std::memcpy(&f.uv[(size_t)j * w], uv + (size_t)j * us, (size_t)w);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

} // namespace

bool appleInterpolationAvailable() {
    if (@available(macOS 15.4, *)) return [VTFrameRateConversionConfiguration isSupported];
    return false;
}

bool interpolateGapApple(const VideoFrame& A, const VideoFrame& B, int count, std::vector<std::shared_ptr<VideoFrame>>& out) {
    if (@available(macOS 15.4, *)) {
        @autoreleasepool {
            std::lock_guard<std::mutex> lk(gMu);
            if (gFailed || count < 1 || A.w != B.w || A.h != B.h || !A.rgba.empty() || !B.rgba.empty()) return false;
            if (A.y.size() != (size_t)A.w * A.h || B.y.size() != A.y.size() || A.uv.size() < (size_t)A.w * A.h / 2 || (A.w & 1) || (A.h & 1)) return false;
            const int w = A.w, h = A.h;
            if (!gProc || gW != w || gH != h) {
                if (gProc) { [gProc endSession]; gProc = nil; gCfg = nil; }
                gCfg = [[VTFrameRateConversionConfiguration alloc] initWithFrameWidth:w frameHeight:h usePrecomputedFlow:NO
                                                              qualityPrioritization:VTFrameRateConversionConfigurationQualityPrioritizationNormal
                                                                           revision:VTFrameRateConversionConfiguration.defaultRevision];
                if (!gCfg) { gFailed = true; return false; }
                gProc = [[VTFrameProcessor alloc] init];
                NSError* err = nil;
                if (![gProc startSessionWithConfiguration:gCfg error:&err]) { gProc = nil; gCfg = nil; gFailed = true; return false; }
                gW = w; gH = h;
            }
            const OSType fmt = A.fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            CVPixelBufferRef pa = makeBuffer(w, h, gCfg.sourcePixelBufferAttributes, fmt), pb = makeBuffer(w, h, gCfg.sourcePixelBufferAttributes, fmt);
            if (!pa || !pb) { if (pa) CVPixelBufferRelease(pa); if (pb) CVPixelBufferRelease(pb); return false; }
            fillBuffer(pa, A); fillBuffer(pb, B);
            VTFrameProcessorFrame* fa = [[VTFrameProcessorFrame alloc] initWithBuffer:pa presentationTimeStamp:CMTimeMake(0, 1000)];
            VTFrameProcessorFrame* fb = [[VTFrameProcessorFrame alloc] initWithBuffer:pb presentationTimeStamp:CMTimeMake(1000, 1000)];
            NSMutableArray<NSNumber*>* phases = [NSMutableArray array];
            NSMutableArray<VTFrameProcessorFrame*>* dests = [NSMutableArray array];
            std::vector<CVPixelBufferRef> dbufs;
            bool ok = fa && fb;
            for (int k = 0; k < count && ok; k++) {
                const float t = (float)(k + 1) / (float)(count + 1);
                CVPixelBufferRef d = makeBuffer(w, h, gCfg.destinationPixelBufferAttributes, fmt);
                if (!d) { ok = false; break; }
                dbufs.push_back(d);
                [phases addObject:@(t)];
                [dests addObject:[[VTFrameProcessorFrame alloc] initWithBuffer:d presentationTimeStamp:CMTimeMake((int64_t)(t * 1000), 1000)]];
            }
            if (ok) {
                VTFrameRateConversionParameters* params = [[VTFrameRateConversionParameters alloc] initWithSourceFrame:fa nextFrame:fb opticalFlow:nil interpolationPhase:phases submissionMode:VTFrameRateConversionParametersSubmissionModeRandom destinationFrames:dests];
                NSError* err = nil;
                ok = params && [gProc processWithParameters:params error:&err];
                if (!ok) gFailed = err != nil;   // after one hard failure stop trying (the caller falls back to the motion search)
            }
            if (ok) {
                for (size_t k = 0; k < dbufs.size(); k++) {
                    auto f = std::make_shared<VideoFrame>();
                    readBuffer(dbufs[k], *f, w, h);
                    f->bt709 = A.bt709; f->fullRange = A.fullRange; f->interlaced = false;
                    out.push_back(std::move(f));
                }
            }
            for (auto d : dbufs) CVPixelBufferRelease(d);
            CVPixelBufferRelease(pa); CVPixelBufferRelease(pb);
            return ok;
        }
    }
    return false;
}

#else   // older SDK: no machine-learning interpolation
bool appleInterpolationAvailable() { return false; }
bool interpolateGapApple(const VideoFrame&, const VideoFrame&, int, std::vector<std::shared_ptr<VideoFrame>>&) { return false; }
#endif

} // namespace dect2
