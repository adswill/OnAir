// Metal back end (macOS).
#include "gfx.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <vector>

namespace gfx {
namespace {

NSString* const kYuvShader = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void nv12(texture2d<float, access::read> Y [[texture(0)]], texture2d<float, access::read> UV [[texture(1)]],
                 texture2d<float, access::write> out [[texture(2)]], constant float3& prm [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    float y = Y.read(gid).r;
    if (prm.z > 0.5) { // interlaced: replace combed pixels (a line that differs from both neighbours in the same direction) by the line average
        const uint H = out.get_height();
        float ya = Y.read(uint2(gid.x, gid.y > 0 ? gid.y - 1 : gid.y)).r;
        float yb = Y.read(uint2(gid.x, gid.y + 1 < H ? gid.y + 1 : gid.y)).r;
        float comb = (y - ya) * (y - yb);
        float w = clamp((comb - 0.0008) / 0.004, 0.0, 1.0);
        y = mix(y, 0.5 * (ya + yb), w);
    }
    float2 c = UV.read(gid / 2).rg;
    if (prm.y < 0.5) { y = (y - 16.0 / 255.0) * (255.0 / 219.0); c = (c - 128.0 / 255.0) * (255.0 / 224.0); }
    else c -= 0.5;
    float3 rgb;
    if (prm.x > 0.5) rgb = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);
    else rgb = float3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x);
    out.write(float4(saturate(rgb), 1.0), gid);
}
)MSL";

id<MTLDevice> gDevice;
id<MTLCommandQueue> gQueue;
id<MTLComputePipelineState> gPso;

struct MtlImage : Image {
    id<MTLTexture> tex = nil;
    MtlImage(int w, int h, uint32_t fill) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        tex = [gDevice newTextureWithDescriptor:d];
        std::vector<uint32_t> px((size_t)w * h, fill);
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px.data() bytesPerRow:w * 4];
    }
    ImTextureID texture() const override { return (ImTextureID)(uintptr_t)(__bridge void*)tex; }
    void update(int x, int y, int w, int h, const uint32_t* rgba) override {
        [tex replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    }
};

struct MtlVideo : Video {
    id<MTLTexture> tex = nil, yTex = nil, uvTex = nil;
    int w, h;
    MtlVideo(int w_, int h_) : w(w_), h(h_) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        tex = [gDevice newTextureWithDescriptor:d];
        MTLTextureDescriptor* dy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:w height:h mipmapped:NO];
        yTex = [gDevice newTextureWithDescriptor:dy];
        MTLTextureDescriptor* du = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:w / 2 height:h / 2 mipmapped:NO];
        uvTex = [gDevice newTextureWithDescriptor:du];
    }
    ImTextureID texture() const override { return (ImTextureID)(uintptr_t)(__bridge void*)tex; }
    void uploadRGBA(const uint8_t* rgba) override {
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    }
    bool uploadNV12(const uint8_t* y, const uint8_t* uv, bool bt709, bool fullRange, bool deint) override {
      @autoreleasepool {
        if (!gPso) {
            NSError* err = nil;
            id<MTLLibrary> lib = [gDevice newLibraryWithSource:kYuvShader options:nil error:&err];
            gPso = lib ? [gDevice newComputePipelineStateWithFunction:[lib newFunctionWithName:@"nv12"] error:&err] : nil;
            if (!gPso) { NSLog(@"yuv shader: %@", err); return false; }
        }
        [yTex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:y bytesPerRow:w];
        [uvTex replaceRegion:MTLRegionMake2D(0, 0, w / 2, h / 2) mipmapLevel:0 withBytes:uv bytesPerRow:w];
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gPso];
        float prm[4] = {bt709 ? 1.f : 0.f, fullRange ? 1.f : 0.f, deint ? 1.f : 0.f, 0.f};
        [e setBytes:prm length:sizeof prm atIndex:0];
        [e setTexture:yTex atIndex:0]; [e setTexture:uvTex atIndex:1]; [e setTexture:tex atIndex:2];
        [e dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
        [cb commit]; // the render pass later on the same queue sees the converted texture
        return true;
      }
    }
};

struct MtlBackend : Backend {
    GLFWwindow* win;
    CAMetalLayer* layer;
    MTLRenderPassDescriptor* rpd;
    id<CAMetalDrawable> drawable = nil;
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = nil;

    explicit MtlBackend(GLFWwindow* w) : win(w) {
        gDevice = MTLCreateSystemDefaultDevice();
        gQueue = [gDevice newCommandQueue];
        ImGui_ImplMetal_Init(gDevice);
        NSWindow* nswin = glfwGetCocoaWindow(w);
        layer = [CAMetalLayer layer];
        layer.device = gDevice;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // a framebuffer-only layer lets the GPU compress and skip the copies; only the dev screenshot reads the picture back
        layer.framebufferOnly = [[NSProcessInfo processInfo].arguments containsObject:@"--shot"] ? NO : YES;
        nswin.contentView.layer = layer;
        nswin.contentView.wantsLayer = YES;
        rpd = [MTLRenderPassDescriptor new];
    }
    Image* createImage(int w, int h, uint32_t fill) override { return new MtlImage(w, h, fill); }
    Video* createVideo(int w, int h) override { return new MtlVideo(w, h); }
    void newFrame(int fbW, int fbH, const float c[4]) override {
      @autoreleasepool {
        if ((int)layer.drawableSize.width != fbW || (int)layer.drawableSize.height != fbH) layer.drawableSize = CGSizeMake(fbW, fbH);
        drawable = [layer nextDrawable];
        cb = [gQueue commandBuffer];
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        rpd.colorAttachments[0].texture = drawable.texture;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:rpd];
        ImGui_ImplMetal_NewFrame(rpd);
      }
    }
    void endFrame(ImDrawData* dd, const char* shotPath) override {
      @autoreleasepool {
        ImGui_ImplMetal_RenderDrawData(dd, cb, enc);
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
        if (shotPath) {
            [cb waitUntilCompleted];
            id<MTLTexture> t = drawable.texture;
            NSUInteger tw = t.width, th = t.height;
            std::vector<uint8_t> px(tw * th * 4);
            [t getBytes:px.data() bytesPerRow:tw * 4 fromRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0];
            for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]); // BGRA -> RGBA
            writePng(shotPath, px.data(), (int)tw, (int)th);
        }
        drawable = nil; cb = nil; enc = nil;
      }
    }
    void shutdown() override { ImGui_ImplMetal_Shutdown(); }
};

} // namespace

void windowHints() { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }
Backend* create(GLFWwindow* window) { return new MtlBackend(window); }

}
