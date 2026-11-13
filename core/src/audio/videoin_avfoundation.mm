// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// macOS cameras and capture cards (Blackmagic, USB HDMI, Continuity, built-in).
#include "videoin_impl.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <cmath>
#include <dispatch/dispatch.h>

@interface OnAirVideoSink : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property dect2::VideoIn::Impl* impl;
@end
@implementation OnAirVideoSink
- (void)captureOutput:(AVCaptureVideoDataOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sb fromConnection:(AVCaptureConnection*)conn {
    (void)output; (void)conn;
    CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
    if (!img || !_impl) return;
    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    const int w = (int)CVPixelBufferGetWidth(img), h = (int)CVPixelBufferGetHeight(img);
    OSType fmt = CVPixelBufferGetPixelFormatType(img);
    std::vector<uint8_t> rgb((size_t)w * (size_t)h * 3);
    if (fmt == kCVPixelFormatType_32BGRA) {
        const uint8_t* src = (const uint8_t*)CVPixelBufferGetBaseAddress(img);
        const size_t stride = CVPixelBufferGetBytesPerRow(img);
        for (int y = 0; y < h; y++) {
            const uint8_t* row = src + (size_t)y * stride;
            uint8_t* dst = rgb.data() + (size_t)y * (size_t)w * 3;
            for (int x = 0; x < w; x++) { dst[0] = row[2]; dst[1] = row[1]; dst[2] = row[0]; row += 4; dst += 3; }
        }
    } else if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        const uint8_t* yp = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(img, 0);
        const uint8_t* up = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(img, 1);
        const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0), us = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
        const bool full = fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int Y = yp[(size_t)y * ys + (size_t)x];
                const uint8_t* uv = up + (size_t)(y / 2) * us + (size_t)(x & ~1);
                const int U = uv[0], V = uv[1];
                const float yf = full ? Y / 255.f : (Y - 16) / 219.f;
                const float uf = (U - 128) / 112.f, vf = (V - 128) / 112.f;
                const float r = std::min(1.f, std::max(0.f, yf + 1.402f * vf));
                const float g = std::min(1.f, std::max(0.f, yf - 0.344f * uf - 0.714f * vf));
                const float b = std::min(1.f, std::max(0.f, yf + 1.772f * uf));
                uint8_t* d = rgb.data() + ((size_t)y * (size_t)w + (size_t)x) * 3;
                d[0] = (uint8_t)std::lround(r * 255); d[1] = (uint8_t)std::lround(g * 255); d[2] = (uint8_t)std::lround(b * 255);
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    _impl->setRgb(w, h, rgb.data());
}
@end

namespace dect2 {

namespace {
NSArray<AVCaptureDevice*>* allVideoDevices() {
    NSMutableArray<AVCaptureDevice*>* out = [NSMutableArray array];
    NSMutableSet<NSString*>* seen = [NSMutableSet set];
    auto add = ^(AVCaptureDevice* d) {
        if (!d || [seen containsObject:d.uniqueID]) return;
        [seen addObject:d.uniqueID];
        [out addObject:d];
    };
    NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    if (@available(macOS 14.0, *)) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
        [types addObject:AVCaptureDeviceTypeExternal];
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
#pragma clang diagnostic pop
    }
    AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:types
        mediaType:AVMediaTypeVideo
        position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* d in ds.devices) add(d);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (AVCaptureDevice* d in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) add(d);
#pragma clang diagnostic pop
    return out;
}

AVCaptureDevice* findDev(const std::string& id) {
    NSArray<AVCaptureDevice*>* all = allVideoDevices();
    if (id.empty()) {
        AVCaptureDevice* d = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        return d ? d : all.firstObject;
    }
    NSString* want = [NSString stringWithUTF8String:id.c_str()];
    for (AVCaptureDevice* d in all)
        if ([d.uniqueID isEqualToString:want] || [d.localizedName isEqualToString:want]) return d;
    return nil;
}

struct Cam {
    AVCaptureSession* sess = nil;
    OnAirVideoSink* sink = nil;
};
}

bool videoInCameraStart(VideoIn::Impl* I, const std::string& deviceId, std::string& err) {
    @autoreleasepool {
        const AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (st == AVAuthorizationStatusDenied || st == AVAuthorizationStatusRestricted) { err = "camera access is off for OnAir (System Settings > Privacy & Security > Camera)"; return false; }
        AVCaptureDevice* dev = findDev(deviceId);
        if (!dev) { err = deviceId.empty() ? "no camera or capture card was found" : "the chosen camera is not connected any more"; return false; }
        NSError* nse = nil;
        AVCaptureDeviceInput* in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&nse];
        if (!in) { err = nse.localizedDescription.UTF8String ? nse.localizedDescription.UTF8String : "the camera could not be opened (check Camera in System Settings)"; return false; }
        AVCaptureSession* sess = [[AVCaptureSession alloc] init];
        if ([sess canSetSessionPreset:AVCaptureSessionPreset640x480]) sess.sessionPreset = AVCaptureSessionPreset640x480;
        if (![sess canAddInput:in]) { err = "this capture device cannot be used as a video input"; return false; }
        [sess addInput:in];
        AVCaptureVideoDataOutput* out = [[AVCaptureVideoDataOutput alloc] init];
        out.videoSettings = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
        out.alwaysDiscardsLateVideoFrames = YES;
        OnAirVideoSink* sink = [[OnAirVideoSink alloc] init];
        sink.impl = I;
        dispatch_queue_t q = dispatch_queue_create("onair.videoin", DISPATCH_QUEUE_SERIAL);
        [out setSampleBufferDelegate:sink queue:q];
        if (![sess canAddOutput:out]) { err = "could not attach a video output to this capture device"; return false; }
        [sess addOutput:out];
        Cam* c = new Cam;
        c->sess = sess; c->sink = sink;
        I->backend = c;
        [sess startRunning];
        if (!sess.running) { err = "the camera did not start (check Camera in System Settings)"; videoInCameraStop(I); return false; }
        return true;
    }
}

void videoInCameraStop(VideoIn::Impl* I) {
    if (!I->backend) return;
    @autoreleasepool {
        Cam* c = (Cam*)I->backend;
        [c->sess stopRunning];
        c->sess = nil; c->sink = nil;
        delete c;
        I->backend = nullptr;
    }
}

std::vector<VideoInputDevice> videoInListCameras() {
    std::vector<VideoInputDevice> v;
    @autoreleasepool {
        AVCaptureDevice* def = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (def) v.push_back({def.uniqueID.UTF8String, std::string(def.localizedName.UTF8String) + " (default)"});
        for (AVCaptureDevice* d in allVideoDevices()) {
            if (def && [d.uniqueID isEqualToString:def.uniqueID]) continue;
            v.push_back({d.uniqueID.UTF8String, d.localizedName.UTF8String});
        }
    }
    if (v.empty()) v.push_back({"", "Default camera"});
    return v;
}

} // namespace dect2
