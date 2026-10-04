// Our own sound output for Windows (WASAPI) and Linux (ALSA, which also reaches PulseAudio and PipeWire through their ALSA plug-ins).
// macOS has its own file (audioout_coreaudio.mm). When the native backend cannot open a device, the miniaudio backend (audioout_miniaudio.cpp)
// is tried, which also provides a silent device with correct timing for machines without sound.
//
// Both backends run one thread that asks AudioOut::Impl::render() for a short block of audio and hands it to the device. The thread has a
// high priority: if it is late, the sound breaks up.
#include "audioout_impl.h"
#include "dect2/platform.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#else
#include <dlfcn.h>
#endif

namespace dect2 {

namespace {

struct NativeBackend {
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> opened{false};   // the thread has tried to open the device
    std::atomic<bool> ok{false};       // and succeeded
};

#ifdef _WIN32
// ---------------------------------------------------------------- WASAPI, shared mode, event driven
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Wasapi {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    UINT32 bufFrames = 0;

    ~Wasapi() { close(); }
    void close() {
        if (client) client->Stop();
        release(render); release(client); release(dev); release(en);
        if (event) { CloseHandle(event); event = nullptr; }
    }
    bool open(int rate) {
        close();
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) return false;
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = (DWORD)rate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = (DWORD)rate * 8;
        wf.Format.cbSize = 22;
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        // the system converts our format to the one of the device (rate and channels)
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000 /* 40 ms */, 0, &wf.Format, nullptr))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event || FAILED(client->SetEventHandle(event))) return false;
        if (FAILED(client->GetBufferSize(&bufFrames))) return false;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render))) return false;
        // start with silence so that the first event finds a full buffer to refill
        BYTE* p = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufFrames, &p))) render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        return SUCCEEDED(client->Start());
    }
};

void threadMain(NativeBackend* b, AudioOut::Impl* I, int rate) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // the multimedia scheduler gives an audio thread the processor in time (avrt.dll, looked up so that nothing needs to link to it)
    DWORD task = 0;
    if (HMODULE av = LoadLibraryW(L"avrt.dll")) {
        typedef HANDLE(WINAPI * Fn)(LPCWSTR, LPDWORD);
        if (auto f = (Fn)GetProcAddress(av, "AvSetMmThreadCharacteristicsW")) f(L"Pro Audio", &task);
    }
    if (!task) setThreadPriority(ThreadPriority::Realtime);
    Wasapi w;
    bool open = w.open(rate);
    b->ok = open; b->opened = true;
    if (!open) { w.close(); CoUninitialize(); return; }   // release the COM objects while COM is still there
    while (!b->stop) {
        const DWORD r = WaitForSingleObject(w.event, 200);
        if (b->stop) break;
        UINT32 pad = 0;
        HRESULT hr = r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT ? w.client->GetCurrentPadding(&pad) : E_FAIL;
        if (SUCCEEDED(hr) && pad < w.bufFrames) {
            const UINT32 n = w.bufFrames - pad;
            BYTE* p = nullptr;
            hr = w.render->GetBuffer(n, &p);
            if (SUCCEEDED(hr)) { I->render(reinterpret_cast<float*>(p), n); hr = w.render->ReleaseBuffer(n, 0); }
        }
        if (FAILED(hr)) {   // the device went away (headphones unplugged, default device changed): open the new default device
            w.close();
            while (!b->stop && !w.open(rate)) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    w.close();
    CoUninitialize();
}
#else
// ---------------------------------------------------------------- ALSA (libasound is loaded when needed, nothing is linked)
struct Alsa {
    void* lib = nullptr;
    void* pcm = nullptr;
    int (*open)(void**, const char*, int, int) = nullptr;
    int (*setParams)(void*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
    long (*writei)(void*, const void*, unsigned long) = nullptr;
    int (*recover)(void*, int, int) = nullptr;
    int (*close)(void*) = nullptr;
    int (*drop)(void*) = nullptr;

    bool load() {
        if (lib) return true;
        lib = dlopen("libasound.so.2", RTLD_NOW);
        if (!lib) return false;
        open = (decltype(open))dlsym(lib, "snd_pcm_open");
        setParams = (decltype(setParams))dlsym(lib, "snd_pcm_set_params");
        writei = (decltype(writei))dlsym(lib, "snd_pcm_writei");
        recover = (decltype(recover))dlsym(lib, "snd_pcm_recover");
        close = (decltype(close))dlsym(lib, "snd_pcm_close");
        drop = (decltype(drop))dlsym(lib, "snd_pcm_drop");
        return open && setParams && writei && recover && close && drop;
    }
    bool openDevice(int rate) {
        closeDevice();
        const char* name = getenv("DECT2_ALSA_DEVICE") ? getenv("DECT2_ALSA_DEVICE") : "default";
        if (open(&pcm, name, 0 /* playback */, 0) < 0) { pcm = nullptr; return false; }
        // interleaved 32-bit float, the system resamples; 60 ms of buffering
        if (setParams(pcm, 14 /* FLOAT_LE */, 3 /* RW_INTERLEAVED */, 2, (unsigned)rate, 1, 60000) < 0) { closeDevice(); return false; }
        return true;
    }
    void closeDevice() { if (pcm) { drop(pcm); close(pcm); pcm = nullptr; } }
    ~Alsa() { closeDevice(); }
};

void threadMain(NativeBackend* b, AudioOut::Impl* I, int rate) {
    setThreadPriority(ThreadPriority::Realtime);
    Alsa a;
    const bool open = a.load() && a.openDevice(rate);
    b->ok = open; b->opened = true;
    if (!open) return;
    const unsigned block = (unsigned)std::max(240, rate / 100);   // 10 ms
    std::vector<float> buf((size_t)block * 2);
    while (!b->stop) {
        I->render(buf.data(), block);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(buf.data());
        unsigned left = block;
        while (left && !b->stop) {
            const long n = a.writei(a.pcm, p, left);
            if (n < 0) {
                if (a.recover(a.pcm, (int)n, 1) < 0) {   // a device that disappeared: try again until it comes back
                    a.closeDevice();
                    while (!b->stop && !a.openDevice(rate)) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                break;
            }
            p += (size_t)n * 8; left -= (unsigned)n;
        }
    }
}
#endif

} // namespace

bool audioBackendStart(AudioOut::Impl* I, int rate) {
    const char* want = getenv("DECT2_AUDIO");
    if (!(want && !strcmp(want, "miniaudio"))) {
        auto* b = new NativeBackend;
        b->th = std::thread(threadMain, b, I, rate);
        for (int i = 0; i < 300 && !b->opened; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));   // the device opens within moments
        if (b->opened && b->ok) { I->backend = b; I->backendKind = 1; return true; }
        b->stop = true;
        if (b->th.joinable()) b->th.join();
        delete b;
    }
    // no sound device we can open ourselves: the fallback
    if (!audioMiniaudioStart(I, rate)) return false;
    I->backendKind = 2;
    return true;
}

void audioBackendStop(AudioOut::Impl* I) {
    if (I->backendKind == 2) { audioMiniaudioStop(I); I->backendKind = 0; return; }
    auto* b = static_cast<NativeBackend*>(I->backend);
    if (!b) return;
    b->stop = true;
    if (b->th.joinable()) b->th.join();
    delete b;
    I->backend = nullptr;
    I->backendKind = 0;
}

} // namespace dect2
