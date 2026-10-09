// Direct3D 11 compute version of the picture repair's synthesis step (Windows). The motion between the two real pictures is estimated on the
// CPU (conceal.cpp, once per gap); this file makes the pictures in between from it, one GPU thread per 4x4 cell, with the same per-cell
// choice of motion and the same bilinear, motion-compensated blend as the CPU code. The pictures of a gap are made in a few short
// dispatches (about a millisecond each, so the GPU's other user, the LDPC decoder, is not kept waiting) and read back once.
// d3d11.dll and d3dcompiler_47.dll are loaded at run time: a computer without a usable GPU simply reports the backend as unavailable.
#include "conceal_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

namespace {

// Byte planes live in raw buffers (ByteAddressBuffer): a byte is read as part of its 32-bit word. A thread owns one 4x4 cell, so its
// luma rows (4 bytes) and chroma rows (2 U/V pairs = 4 bytes) are whole aligned words and no two threads write the same word.
const char* const kHlsl = R"HLSL(
ByteAddressBuffer Ay : register(t0);     // luma of picture A, W*H bytes
ByteAddressBuffer By : register(t1);     // luma of picture B
ByteAddressBuffer Auv : register(t2);    // interleaved U/V of picture A, W*H/2 bytes
ByteAddressBuffer Buv : register(t3);
ByteAddressBuffer cells : register(t4);  // per picture and 4x4 cell: cvx (12 bits, signed) | cvy (12 bits, signed) << 12 | set << 24
RWByteAddressBuffer outp : register(u0); // per picture: W*H luma bytes, then W*H/2 chroma bytes

cbuffer P : register(b0) {
    uint W, H, GW, GH;
    float T;         // position of this picture between A (0) and B (1)
    uint pic;        // index of this picture within the gap
    uint nMot;       // number of whole-picture motions to try (at most 6)
    uint pad;
    int4 mot[3];     // the motions, in pixels: mot[i / 2].xy for even i, .zw for odd i
};

uint rdb(ByteAddressBuffer b, uint idx) {
    uint wv = b.Load(idx & ~3u);
    return (wv >> ((idx & 3u) * 8u)) & 255u;
}

float bilinY(ByteAddressBuffer b, float x, float y) {
    x = clamp(x, 0.0f, (float)(W - 1));
    y = clamp(y, 0.0f, (float)(H - 1));
    int x0 = (int)x, y0 = (int)y;
    int x1 = min((int)W - 1, x0 + 1), y1 = min((int)H - 1, y0 + 1);
    float fx = x - (float)x0, fy = y - (float)y0;
    float a = (float)rdb(b, (uint)y0 * W + (uint)x0) * (1.0f - fx) + (float)rdb(b, (uint)y0 * W + (uint)x1) * fx;
    float c = (float)rdb(b, (uint)y1 * W + (uint)x0) * (1.0f - fx) + (float)rdb(b, (uint)y1 * W + (uint)x1) * fx;
    return a * (1.0f - fy) + c * fy;
}

float bilinC(ByteAddressBuffer b, uint comp, float x, float y) {
    uint CW = W / 2, CH = H / 2;
    x = clamp(x, 0.0f, (float)(CW - 1));
    y = clamp(y, 0.0f, (float)(CH - 1));
    int x0 = (int)x, y0 = (int)y;
    int x1 = min((int)CW - 1, x0 + 1), y1 = min((int)CH - 1, y0 + 1);
    float fx = x - (float)x0, fy = y - (float)y0;
    float a = (float)rdb(b, (uint)y0 * W + 2u * (uint)x0 + comp) * (1.0f - fx) + (float)rdb(b, (uint)y0 * W + 2u * (uint)x1 + comp) * fx;
    float c = (float)rdb(b, (uint)y1 * W + 2u * (uint)x0 + comp) * (1.0f - fx) + (float)rdb(b, (uint)y1 * W + 2u * (uint)x1 + comp) * fx;
    return a * (1.0f - fy) + c * fy;
}

// how well the two real pictures agree at this cell if it moves by (px, py) over the whole gap (nearest-pixel SAD, 16 samples)
int score(float px, float py, int cx, int cy) {
    int sad = 0;
    for (int j = -2; j < 6; j += 2) {
        for (int i = -2; i < 6; i += 2) {
            int x = clamp(cx * 4 + i, 0, (int)W - 1);
            int y = clamp(cy * 4 + j, 0, (int)H - 1);
            int xa = clamp((int)floor((float)x - T * px + 0.5f), 0, (int)W - 1);
            int ya = clamp((int)floor((float)y - T * py + 0.5f), 0, (int)H - 1);
            int xb = clamp((int)floor((float)x + (1.0f - T) * px + 0.5f), 0, (int)W - 1);
            int yb = clamp((int)floor((float)y + (1.0f - T) * py + 0.5f), 0, (int)H - 1);
            sad += abs((int)rdb(Ay, (uint)ya * W + (uint)xa) - (int)rdb(By, (uint)yb * W + (uint)xb));
        }
    }
    return sad;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint cxu = id.x, cyu = id.y;
    if (cxu < GW && cyu < GH) {
        int cx = (int)cxu, cy = (int)cyu;
        uint cell = cells.Load(((pic * GH + cyu) * GW + cxu) * 4u);
        int cvx = ((int)(cell << 20)) >> 20;
        int cvy = ((int)((cell >> 12) << 20)) >> 20;
        bool cset = ((cell >> 24) & 1u) != 0u;
        float mx = 0.0f, my = 0.0f;
        if (nMot > 0u || cset) {
            int best = score(0.0f, 0.0f, cx, cy);
            if (cset) {
                float px = (float)cvx, py = (float)cvy;
                int sc = score(px, py, cx, cy);
                if (sc <= best + 40) { best = sc; mx = px; my = py; }
            }
            if (mx == 0.0f && my == 0.0f) {
                for (uint m = 0; m < nMot; m++) {
                    int4 mm = mot[m >> 1];
                    float px = (m & 1u) != 0u ? (float)mm.z : (float)mm.x;
                    float py = (m & 1u) != 0u ? (float)mm.w : (float)mm.y;
                    int sc = score(px, py, cx, cy);
                    if (sc * 10 < best * 8) { best = sc; mx = px; my = py; }
                }
            }
        }
        uint frameBase = pic * (W * H + W * H / 2u);
        for (int j = 0; j < 4; j++) {
            uint packed = 0u;
            for (int i = 0; i < 4; i++) {
                float x = (float)(cx * 4 + i), y = (float)(cy * 4 + j);
                float va = bilinY(Ay, x - T * mx, y - T * my);
                float vb = bilinY(By, x + (1.0f - T) * mx, y + (1.0f - T) * my);
                float wb = T;
                if (abs(va - vb) > 40.0f) wb = (T < 0.5f) ? 0.0f : 1.0f;
                uint v = (uint)clamp(va * (1.0f - wb) + vb * wb + 0.5f, 0.0f, 255.0f);
                packed |= v << (uint)(i * 8);
            }
            outp.Store(frameBase + (uint)(cy * 4 + j) * W + (uint)cx * 4u, packed);
        }
        for (int j2 = 0; j2 < 2; j2++) {
            uint packed = 0u;
            for (int i2 = 0; i2 < 2; i2++) {
                float x = (float)(cx * 2 + i2), y = (float)(cy * 2 + j2);
                float fx = mx * 0.5f, fy = my * 0.5f;
                float ua = bilinC(Auv, 0u, x - T * fx, y - T * fy);
                float ub = bilinC(Buv, 0u, x + (1.0f - T) * fx, y + (1.0f - T) * fy);
                float va = bilinC(Auv, 1u, x - T * fx, y - T * fy);
                float vb = bilinC(Buv, 1u, x + (1.0f - T) * fx, y + (1.0f - T) * fy);
                float wb = T;
                if (abs(ua - ub) + abs(va - vb) > 40.0f) wb = (T < 0.5f) ? 0.0f : 1.0f;
                uint u8 = (uint)clamp(ua * (1.0f - wb) + ub * wb + 0.5f, 0.0f, 255.0f);
                uint v8 = (uint)clamp(va * (1.0f - wb) + vb * wb + 0.5f, 0.0f, 255.0f);
                packed |= u8 << (uint)(i2 * 16);
                packed |= v8 << (uint)(i2 * 16 + 8);
            }
            outp.Store(frameBase + W * H + (uint)(cy * 2 + j2) * W + (uint)cx * 4u, packed);
        }
    }
}
)HLSL";

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct CBuf {
    uint32_t W, H, GW, GH;
    float T;
    uint32_t pic, nMot, pad;
    int32_t mot[12];
};

struct Gpu {
    HMODULE hD3d = nullptr, hComp = nullptr;
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<ID3D11ComputeShader> cs;
    Com<ID3D11Buffer> cbuf;
    Com<ID3D11Buffer> outBuf, stage;       // reused between gaps while they are big enough
    Com<ID3D11UnorderedAccessView> outUav;
    size_t outCap = 0;
    std::string name;
    bool ready = false;
    std::mutex mu;
    Gpu();
};

Gpu::Gpu() {
    if (getenv("DECT2_NOGPU") || getenv("DECT2_SWINTERP")) return;
    { const char* e = getenv("DECT2_CONCEAL_GPU"); if (e && atoi(e) == 0) return; }
    hD3d = LoadLibraryA("d3d11.dll");
    hComp = LoadLibraryA("d3dcompiler_47.dll");
    if (!hD3d || !hComp) return;
    auto create = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3d, "D3D11CreateDevice");
    auto compile = (pD3DCompile)GetProcAddress(hComp, "D3DCompile");
    if (!create || !compile) return;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    // the same adapter choice as the LDPC decoder: the one with the most video memory of its own (DECT2_GPU_ADAPTER=<index> forces one)
    Com<IDXGIAdapter> best;
    if (HMODULE hDxgi = LoadLibraryA("dxgi.dll")) {
        typedef HRESULT(WINAPI * PFN_FACTORY)(REFIID, void**);
        if (auto mk = (PFN_FACTORY)GetProcAddress(hDxgi, "CreateDXGIFactory1")) {
            Com<IDXGIFactory1> fac;
            if (SUCCEEDED(mk(__uuidof(IDXGIFactory1), (void**)fac.put()))) {
                SIZE_T bestMem = 0;
                const int forced = getenv("DECT2_GPU_ADAPTER") ? atoi(getenv("DECT2_GPU_ADAPTER")) : -1;
                for (UINT i = 0;; i++) {
                    Com<IDXGIAdapter> a;
                    if (fac->EnumAdapters(i, a.put()) != S_OK) break;
                    DXGI_ADAPTER_DESC ds{};
                    if (FAILED(a->GetDesc(&ds)) || ds.VendorId == 0x1414) continue;
                    if (forced >= 0 ? (int)i == forced : (!best || ds.DedicatedVideoMemory > bestMem)) { best = std::move(a); bestMem = ds.DedicatedVideoMemory; }
                }
            }
        }
    }
    if (FAILED(create(best.p, best ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, dev.put(), &got, ctx.put()))) { dev.reset(); ctx.reset(); return; }
    if (got < D3D_FEATURE_LEVEL_11_0) { dev.reset(); ctx.reset(); return; }
    {
        Com<IDXGIDevice> dxgi;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)dxgi.put()))) {
            Com<IDXGIAdapter> ad;
            if (SUCCEEDED(dxgi->GetAdapter(ad.put()))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(ad->GetDesc(&desc))) {
                    char buf[256] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof buf - 1, nullptr, nullptr);
                    name = buf;
                    if (desc.VendorId == 0x1414) { dev.reset(); ctx.reset(); return; }   // the software rasteriser is slower than the CPU code
                }
            }
        }
    }
    Com<ID3DBlob> code, err;
    const HRESULT hr = compile(kHlsl, strlen(kHlsl), "conceal.hlsl", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put());
    if (FAILED(hr) || !code) {
        fprintf(stderr, "Conceal: shader compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        dev.reset(); ctx.reset();
        return;
    }
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.put()))) { dev.reset(); ctx.reset(); return; }
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = sizeof(CBuf);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&cd, nullptr, cbuf.put()))) { dev.reset(); ctx.reset(); return; }
    ready = true;
    fprintf(stderr, "Conceal: Direct3D 11 picture repair on \"%s\"\n", name.c_str());
}

Gpu& gpu() { static Gpu g; return g; }

bool rawSrv(Gpu& g, const void* data, size_t bytes, Com<ID3D11Buffer>& b, Com<ID3D11ShaderResourceView>& v) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (UINT)((bytes + 3) & ~(size_t)3);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    if (FAILED(g.dev->CreateBuffer(&d, &sd, b.put()))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx.FirstElement = 0;
    sv.BufferEx.NumElements = d.ByteWidth / 4;
    sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    return SUCCEEDED(g.dev->CreateShaderResourceView(b.p, &sv, v.put()));
}

} // namespace

bool d3d11ConcealAvailable() { return gpu().ready; }
const char* d3d11ConcealDevice() { return gpu().name.c_str(); }

bool d3d11Synthesize(const VideoFrame& A, const VideoFrame& B, int count, const GapMotion& gm, std::vector<std::shared_ptr<VideoFrame>>& out) {
    Gpu& g = gpu();
    if (!g.ready) return false;
    const int w = A.w, h = A.h;
    if (w < 64 || h < 64 || (w & 3) || (h & 3) || count < 1 || count > 48 || B.w != w || B.h != h) return false;
    const size_t planeY = (size_t)w * h, planeUV = planeY / 2;
    if (A.y.size() != planeY || B.y.size() != planeY || A.uv.size() < planeUV || B.uv.size() < planeUV) return false;
    const int cs = 4, gw = w / cs, gh = h / cs;
    std::lock_guard<std::mutex> lk(g.mu);

    // per picture and cell: the vector of the moving 32x32 block that is projected onto it (the last block in raster order wins, as in
    // the CPU code), so the GPU only has to decide, per cell, between that, standing still, and the picture's common motions
    std::vector<uint32_t> cells((size_t)count * gw * gh, 0u);
    for (int k = 0; k < count; k++) {
        const float t = (float)(k + 1) / (float)(count + 1);
        uint32_t* cp = &cells[(size_t)k * gw * gh];
        for (int by = 0; by < gm.bh; by++)
            for (int bx = 0; bx < gm.bw; bx++) {
                const int fx = gm.mvx[(size_t)by * gm.bw + bx] * 4, fy = gm.mvy[(size_t)by * gm.bw + bx] * 4;
                if (!fx && !fy) continue;
                const int x0 = (int)std::lround(bx * 32 + t * fx), y0 = (int)std::lround(by * 32 + t * fy);
                const uint32_t packed = ((uint32_t)fx & 0xFFFu) | (((uint32_t)fy & 0xFFFu) << 12) | (1u << 24);
                for (int cy = std::max(0, y0 / cs); cy < std::min(gh, (y0 + 32) / cs); cy++)
                    for (int cx = std::max(0, x0 / cs); cx < std::min(gw, (x0 + 32) / cs); cx++) cp[(size_t)cy * gw + cx] = packed;
            }
    }

    Com<ID3D11Buffer> bAy, bBy, bAuv, bBuv, bCells;
    Com<ID3D11ShaderResourceView> vAy, vBy, vAuv, vBuv, vCells;
    if (!rawSrv(g, A.y.data(), planeY, bAy, vAy) || !rawSrv(g, B.y.data(), planeY, bBy, vBy) || !rawSrv(g, A.uv.data(), planeUV, bAuv, vAuv) ||
        !rawSrv(g, B.uv.data(), planeUV, bBuv, vBuv) || !rawSrv(g, cells.data(), cells.size() * 4, bCells, vCells)) return false;

    const size_t frameBytes = planeY + planeUV, used = (size_t)count * frameBytes;
    if (used > g.outCap) {
        g.outUav.reset(); g.outBuf.reset(); g.stage.reset(); g.outCap = 0;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = (UINT)used;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(g.dev->CreateBuffer(&d, nullptr, g.outBuf.put()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.FirstElement = 0;
        u.Buffer.NumElements = (UINT)(used / 4);
        u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(g.dev->CreateUnorderedAccessView(g.outBuf.p, &u, g.outUav.put()))) { g.outBuf.reset(); return false; }
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = (UINT)used;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g.dev->CreateBuffer(&sd, nullptr, g.stage.put()))) { g.outUav.reset(); g.outBuf.reset(); return false; }
        g.outCap = used;
    }

    ID3D11ShaderResourceView* srv[5] = {vAy.p, vBy.p, vAuv.p, vBuv.p, vCells.p};
    ID3D11UnorderedAccessView* uav[1] = {g.outUav.p};
    g.ctx->CSSetShader(g.cs.p, nullptr, 0);
    g.ctx->CSSetShaderResources(0, 5, srv);
    g.ctx->CSSetUnorderedAccessViews(0, 1, uav, nullptr);
    g.ctx->CSSetConstantBuffers(0, 1, &g.cbuf.p);
    for (int k = 0; k < count; k++) {
        CBuf cb{};
        cb.W = (uint32_t)w; cb.H = (uint32_t)h; cb.GW = (uint32_t)gw; cb.GH = (uint32_t)gh;
        cb.T = (float)(k + 1) / (float)(count + 1);
        cb.pic = (uint32_t)k;
        cb.nMot = (uint32_t)std::min<size_t>(6, gm.motions.size());
        for (uint32_t i = 0; i < cb.nMot; i++) { cb.mot[2 * i] = gm.motions[i].first; cb.mot[2 * i + 1] = gm.motions[i].second; }
        g.ctx->UpdateSubresource(g.cbuf.p, 0, nullptr, &cb, 0, 0);
        g.ctx->Dispatch((UINT)((gw + 7) / 8), (UINT)((gh + 7) / 8), 1);
    }
    ID3D11ShaderResourceView* nullSrv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUav[1] = {nullptr};
    g.ctx->CSSetShaderResources(0, 5, nullSrv);
    g.ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    D3D11_BOX box{0, 0, 0, (UINT)used, 1, 1};
    g.ctx->CopySubresourceRegion(g.stage.p, 0, 0, 0, 0, g.outBuf.p, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(g.ctx->Map(g.stage.p, 0, D3D11_MAP_READ, 0, &mp))) return false;
    const uint8_t* src = static_cast<const uint8_t*>(mp.pData);
    for (int k = 0; k < count; k++) {
        auto f = std::make_shared<VideoFrame>();
        f->w = w; f->h = h; f->dar = A.dar; f->bt709 = A.bt709; f->fullRange = A.fullRange; f->interlaced = false;
        f->y.resize(planeY);
        f->uv.resize(A.uv.size(), 128);
        memcpy(f->y.data(), src + (size_t)k * frameBytes, planeY);
        memcpy(f->uv.data(), src + (size_t)k * frameBytes + planeY, planeUV);
        out.push_back(std::move(f));
    }
    g.ctx->Unmap(g.stage.p, 0);
    return true;
}

} // namespace dect2
