// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bitstream passthrough devices for Windows (WASAPI exclusive) and Linux (ALSA); macOS is in bitstream_out_coreaudio.mm. See bitstream_out.h.
// EXPERIMENTAL: not tested on real hardware.
#include "bitstream_out.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace dect2 {

std::string alsaIecDeviceName(const std::string& device, int linkRate) {
    // IEC 60958-3 channel status: AES0 0x06 = consumer, non-audio (bit 1), no copyright assertion; AES1 0x82 = original, PCM encoder
    // category; AES3 = the sampling frequency of the link (48 kHz 0x02, 192 kHz 0x0e, 44.1 kHz 0x00, 32 kHz 0x03)
    const char* aes3 = linkRate == 192000 ? "0x0e" : linkRate == 176400 ? "0x0c" : linkRate == 44100 ? "0x00" : linkRate == 32000 ? "0x03" : "0x02";
    std::string base = device.empty() ? std::string("hdmi") : device;
    if (base.find("AES0") != std::string::npos) return base;   // the user gave the channel status already
    const bool hasArgs = base.find(':') != std::string::npos;
    return base + (hasArgs ? "," : ":") + "AES0=0x06,AES1=0x82,AES2=0x00,AES3=" + aes3;
}

#ifdef _WIN32
namespace {
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), here so that no GUID storage has to be instantiated
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// WAVEFORMATEXTENSIBLE_IEC61937 (ksmedia.h), declared here so that no particular SDK version is needed
#pragma pack(push, 1)
struct WfxIec61937 {
    WAVEFORMATEXTENSIBLE fx;
    DWORD encodedSamplesPerSec;
    DWORD encodedChannelCount;
    DWORD averageBytesPerSec;
};
#pragma pack(pop)

class WasapiBitstream : public BitstreamDevice {
public:
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    UINT32 bufFrames = 0;
    bool com = false;
    ~WasapiBitstream() override {
        if (client) client->Stop();
        rel(render); rel(client); rel(dev); rel(en);
        if (com) CoUninitialize();
    }
    bool findDevice(const std::string& name) {
        if (name.empty()) return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev));
        IMMDeviceCollection* col = nullptr;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return false;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n && !dev; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            IPropertyStore* ps = nullptr;
            bool match = false;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) {
                    char u[512] = {};
                    WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, u, sizeof u - 1, nullptr, nullptr);
                    match = name == u;
                }
                PropVariantClear(&v);
                ps->Release();
            }
            if (match) dev = d; else d->Release();
        }
        col->Release();
        return dev || SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev));   // not found: the default output
    }
    bool open(const std::string& name, int linkRate, int kind, std::string& err) {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) || !findDevice(name)) {
            err = "no sound output device"; return false;
        }
        WfxIec61937 w{};
        w.fx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        w.fx.Format.nChannels = 2;
        w.fx.Format.nSamplesPerSec = (DWORD)linkRate;
        w.fx.Format.wBitsPerSample = 16;
        w.fx.Format.nBlockAlign = 4;
        w.fx.Format.nAvgBytesPerSec = (DWORD)linkRate * 4;
        w.fx.Format.cbSize = sizeof(WfxIec61937) - sizeof(WAVEFORMATEX);   // 34
        w.fx.Samples.wValidBitsPerSample = 16;
        w.fx.dwChannelMask = 0x3F;   // KSAUDIO_SPEAKER_5POINT1, as in Microsoft's example for IEC 61937 formats
        // KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL 00000092-0000-0010-8000-00aa00389b71,
        // KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS 0000000a-0cea-0010-8000-00aa00389b71
        w.fx.SubFormat = kind == 2 ? GUID{0x0000000a, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}
                                   : GUID{0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        w.encodedSamplesPerSec = kind == 2 ? (DWORD)(linkRate / 4) : (DWORD)linkRate;
        w.encodedChannelCount = 6;
        w.averageBytesPerSec = 0;
        REFERENCE_TIME aligned = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) { err = "the output device cannot be opened"; return false; }
            if (attempt == 0 && client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.fx.Format, nullptr) != S_OK) {
                err = "this output does not accept a Dolby bitstream in exclusive mode (allow exclusive mode, and use an HDMI or S/PDIF output to a receiver)";
                return false;
            }
            REFERENCE_TIME def = 0, minp = 0;
            client->GetDevicePeriod(&def, &minp);
            const REFERENCE_TIME dur = aligned ? aligned : std::max<REFERENCE_TIME>(def * 4, 400000);   // at least 40 ms of device buffer
            HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, 0, &w.fx.Format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && attempt == 0) {   // the documented retry with the aligned size
                UINT32 n = 0; client->GetBufferSize(&n);
                rel(client);
                aligned = (REFERENCE_TIME)(10000000.0 * n / linkRate + 0.5);
                continue;
            }
            if (FAILED(hr)) {
                err = hr == AUDCLNT_E_DEVICE_IN_USE ? "the output is in use by another program in exclusive mode" : "the output refused the bitstream format";
                return false;
            }
            break;
        }
        if (!client || FAILED(client->GetBufferSize(&bufFrames)) || FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render))) {
            err = "the output device cannot be started"; return false;
        }
        BYTE* p = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufFrames, &p))) render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(client->Start())) { err = "the output device cannot be started"; return false; }
        return true;
    }
    bool write(const int16_t* words, int frames) override {
        int done = 0;
        while (done < frames) {
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) return false;
            const UINT32 room = bufFrames > pad ? bufFrames - pad : 0;
            if (!room) { Sleep(2); continue; }
            const UINT32 n = std::min<UINT32>(room, (UINT32)(frames - done));
            BYTE* p = nullptr;
            if (FAILED(render->GetBuffer(n, &p))) return false;
            memcpy(p, words + (size_t)done * 2, (size_t)n * 4);
            if (FAILED(render->ReleaseBuffer(n, 0))) return false;
            done += (int)n;
        }
        return true;
    }
};
} // namespace

std::unique_ptr<BitstreamDevice> bitstreamOpen(const std::string& device, int linkRate, int kind, std::string& err) {
    auto d = std::make_unique<WasapiBitstream>();
    if (!d->open(device, linkRate, kind, err)) return nullptr;
    return d;
}

#elif !defined(__APPLE__)
namespace {
class AlsaBitstream : public BitstreamDevice {
public:
    void* lib = nullptr;
    void* pcm = nullptr;
    int (*open_)(void**, const char*, int, int) = nullptr;
    int (*setParams)(void*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
    long (*writei)(void*, const void*, unsigned long) = nullptr;
    int (*recover)(void*, int, int) = nullptr;
    int (*close_)(void*) = nullptr;
    const char* (*strerror_)(int) = nullptr;
    ~AlsaBitstream() override { if (pcm && close_) close_(pcm); }
    bool open(const std::string& device, int linkRate, std::string& err) {
        lib = dlopen("libasound.so.2", RTLD_NOW);
        if (!lib) { err = "ALSA (libasound.so.2) is not installed"; return false; }
        open_ = (decltype(open_))dlsym(lib, "snd_pcm_open");
        setParams = (decltype(setParams))dlsym(lib, "snd_pcm_set_params");
        writei = (decltype(writei))dlsym(lib, "snd_pcm_writei");
        recover = (decltype(recover))dlsym(lib, "snd_pcm_recover");
        close_ = (decltype(close_))dlsym(lib, "snd_pcm_close");
        strerror_ = (decltype(strerror_))dlsym(lib, "snd_strerror");
        if (!open_ || !setParams || !writei || !recover || !close_) { err = "ALSA functions missing"; return false; }
        const std::string name = alsaIecDeviceName(device, linkRate);
        int r = open_(&pcm, name.c_str(), 0, 0);
        if (r < 0) { pcm = nullptr; err = "ALSA device " + name + ": " + (strerror_ ? strerror_(r) : "cannot be opened") + " (PulseAudio / PipeWire may hold it; pick the hdmi device of the receiver)"; return false; }
        // S16_LE, interleaved, 2 channels at the link rate, no resampling (it would destroy the bitstream), 100 ms
        r = setParams(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */, 2, (unsigned)linkRate, 0, 100000);
        if (r < 0) { err = "ALSA device " + name + " refuses 16-bit stereo at " + std::to_string(linkRate) + " Hz"; return false; }
        return true;
    }
    bool write(const int16_t* words, int frames) override {
        const int16_t* p = words;
        long left = frames;
        while (left > 0) {
            const long n = writei(pcm, p, (unsigned long)left);
            if (n < 0) { if (recover(pcm, (int)n, 1) < 0) return false; continue; }
            p += n * 2; left -= n;
        }
        return true;
    }
};
} // namespace

std::unique_ptr<BitstreamDevice> bitstreamOpen(const std::string& device, int linkRate, int /*kind*/, std::string& err) {
    auto d = std::make_unique<AlsaBitstream>();
    if (!d->open(device, linkRate, err)) return nullptr;
    return d;
}
#endif

} // namespace dect2
