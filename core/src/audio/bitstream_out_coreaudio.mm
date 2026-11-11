// macOS bitstream passthrough (bitstream_out.h): CoreAudio hog mode, the output stream switched to a non-mixable physical format that carries
// IEC 61937 data (kAudioFormat60958AC3, or 16-bit stereo PCM marked non-mixable at the link rate), an IOProc that copies the words untouched.
// EXPERIMENTAL: written from Apple's CoreAudio headers, NOT tested on real hardware.
#include "bitstream_out.h"
#import <CoreAudio/CoreAudio.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace dect2 {

std::string alsaIecDeviceName(const std::string& device, int linkRate) {
    (void)linkRate;
    return device;   // ALSA only
}

namespace {
AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{s, sc, kAudioObjectPropertyElementMain};
}

AudioObjectID findDevice(const std::string& name) {
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices);
    UInt32 sz = 0;
    if (!name.empty() && AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) == noErr && sz) {
        std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) == noErr)
            for (AudioObjectID id : ids) {
                AudioObjectPropertyAddress nm = addr(kAudioObjectPropertyName);
                CFStringRef cf = nullptr; UInt32 csz = sizeof cf;
                if (AudioObjectGetPropertyData(id, &nm, 0, nullptr, &csz, &cf) != noErr || !cf) continue;
                char buf[256] = {};
                CFStringGetCString(cf, buf, sizeof buf, kCFStringEncodingUTF8);
                CFRelease(cf);
                if (name == buf) return id;
            }
    }
    AudioObjectID dev = 0; sz = sizeof dev;
    AudioObjectPropertyAddress d = addr(kAudioHardwarePropertyDefaultOutputDevice);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &d, 0, nullptr, &sz, &dev);
    return dev;
}

class CaBitstream : public BitstreamDevice {
public:
    AudioObjectID dev = 0, stream = 0;
    AudioDeviceIOProcID proc = nullptr;
    AudioStreamBasicDescription oldFmt{};
    bool haveOld = false, hogged = false, mixChanged = false;
    UInt32 oldMix = 1;
    int streamIndex = 0;
    std::vector<int16_t> ring;   // SPSC: write() produces, the IOProc consumes
    size_t cap = 0;              // frames, a power of two
    std::atomic<uint64_t> w{0}, r{0};
    std::atomic<bool> failed{false};

    ~CaBitstream() override {
        if (proc) { AudioDeviceStop(dev, proc); AudioDeviceDestroyIOProcID(dev, proc); }
        if (haveOld) { AudioObjectPropertyAddress a = addr(kAudioStreamPropertyPhysicalFormat); AudioObjectSetPropertyData(stream, &a, 0, nullptr, sizeof oldFmt, &oldFmt); }
        if (mixChanged) { AudioObjectPropertyAddress a = addr(kAudioDevicePropertySupportsMixing); AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof oldMix, &oldMix); }
        if (hogged) { pid_t none = -1; AudioObjectPropertyAddress a = addr(kAudioDevicePropertyHogMode); AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof none, &none); }
    }

    static OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*, AudioBufferList* out, const AudioTimeStamp*, void* ref) {
        auto* s = static_cast<CaBitstream*>(ref);
        if (!out || (UInt32)s->streamIndex >= out->mNumberBuffers) return noErr;
        AudioBuffer& b = out->mBuffers[s->streamIndex];
        auto* dst = static_cast<int16_t*>(b.mData);
        const size_t frames = b.mDataByteSize / 4;
        const uint64_t rd = s->r.load(std::memory_order_relaxed), wr = s->w.load(std::memory_order_acquire);
        const size_t n = (size_t)std::min<uint64_t>(wr - rd, frames);
        for (size_t i = 0; i < n; i++) {
            const size_t k = (size_t)((rd + i) & (s->cap - 1)) * 2;
            dst[2 * i] = s->ring[k]; dst[2 * i + 1] = s->ring[k + 1];
        }
        if (n < frames) memset(dst + 2 * n, 0, (frames - n) * 4);   // zeros: no burst, the receiver mutes
        s->r.store(rd + n, std::memory_order_release);
        return noErr;
    }

    bool open(const std::string& name, int linkRate, int kind, std::string& err) {
        dev = findDevice(name);
        if (!dev) { err = "no sound output device"; return false; }
        AudioObjectPropertyAddress sa = addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(dev, &sa, 0, nullptr, &sz) != noErr || !sz) { err = "the device has no output stream"; return false; }
        std::vector<AudioStreamID> streams(sz / sizeof(AudioStreamID));
        AudioObjectGetPropertyData(dev, &sa, 0, nullptr, &sz, streams.data());
        // a stream that offers a bitstream format at the link rate: the IEC 60958 AC-3 format, else 16-bit stereo non-mixable PCM
        AudioStreamBasicDescription pick{};
        bool found = false;
        for (size_t si = 0; si < streams.size() && !found; si++) {
            AudioObjectPropertyAddress fa = addr(kAudioStreamPropertyAvailablePhysicalFormats);
            UInt32 fsz = 0;
            if (AudioObjectGetPropertyDataSize(streams[si], &fa, 0, nullptr, &fsz) != noErr || !fsz) continue;
            std::vector<AudioStreamRangedDescription> f(fsz / sizeof(AudioStreamRangedDescription));
            AudioObjectGetPropertyData(streams[si], &fa, 0, nullptr, &fsz, f.data());
            for (int pass = 0; pass < 2 && !found; pass++)
                for (const AudioStreamRangedDescription& d : f) {
                    const AudioStreamBasicDescription& m = d.mFormat;
                    const bool rateOk = m.mSampleRate == linkRate || (m.mSampleRate == 0 && d.mSampleRateRange.mMinimum <= linkRate && d.mSampleRateRange.mMaximum >= linkRate);
                    if (!rateOk || m.mChannelsPerFrame != 2) continue;
                    const bool ac3Fmt = m.mFormatID == kAudioFormat60958AC3 || m.mFormatID == 'IAC3';
                    const bool rawPcm = m.mFormatID == kAudioFormatLinearPCM && m.mBitsPerChannel == 16 && (m.mFormatFlags & kAudioFormatFlagIsNonMixable);
                    if ((pass == 0 && ac3Fmt && kind == 1) || (pass == 1 && (rawPcm || ac3Fmt))) {
                        pick = m; pick.mSampleRate = linkRate; stream = streams[si]; streamIndex = (int)si; found = true; break;
                    }
                }
        }
        if (!found) {
            err = "this output offers no bitstream format at " + std::to_string(linkRate) + " Hz (it needs an HDMI or optical output to a receiver" + std::string(kind == 2 ? " that takes Dolby Digital Plus)" : ")");
            return false;
        }
        pid_t me = getpid();
        AudioObjectPropertyAddress ha = addr(kAudioDevicePropertyHogMode);
        if (AudioObjectSetPropertyData(dev, &ha, 0, nullptr, sizeof me, &me) == noErr) hogged = true;
        AudioObjectPropertyAddress ma = addr(kAudioDevicePropertySupportsMixing);
        Boolean settable = false;
        if (AudioObjectIsPropertySettable(dev, &ma, &settable) == noErr && settable) {
            UInt32 msz = sizeof oldMix; AudioObjectGetPropertyData(dev, &ma, 0, nullptr, &msz, &oldMix);
            UInt32 off = 0;
            if (AudioObjectSetPropertyData(dev, &ma, 0, nullptr, sizeof off, &off) == noErr) mixChanged = true;
        }
        AudioObjectPropertyAddress pa = addr(kAudioStreamPropertyPhysicalFormat);
        UInt32 osz = sizeof oldFmt;
        haveOld = AudioObjectGetPropertyData(stream, &pa, 0, nullptr, &osz, &oldFmt) == noErr;
        if (AudioObjectSetPropertyData(stream, &pa, 0, nullptr, sizeof pick, &pick) != noErr) { err = "the output refused the bitstream format"; return false; }
        for (int i = 0; i < 50; i++) {   // the format change is asynchronous
            AudioStreamBasicDescription now{}; UInt32 nsz = sizeof now;
            if (AudioObjectGetPropertyData(stream, &pa, 0, nullptr, &nsz, &now) == noErr && now.mFormatID == pick.mFormatID && now.mSampleRate == pick.mSampleRate) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cap = 1 << 15;   // 32768 frames: 170 ms at 192 kHz
        ring.assign(cap * 2, 0);
        if (AudioDeviceCreateIOProcID(dev, ioProc, this, &proc) != noErr || AudioDeviceStart(dev, proc) != noErr) { err = "the output device cannot be started"; return false; }
        return true;
    }

    bool write(const int16_t* words, int frames) override {
        int done = 0;
        while (done < frames) {
            const uint64_t wr = w.load(std::memory_order_relaxed), rd = r.load(std::memory_order_acquire);
            const size_t room = cap - (size_t)(wr - rd);
            if (!room) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            const size_t n = std::min(room, (size_t)(frames - done));
            for (size_t i = 0; i < n; i++) {
                const size_t k = (size_t)((wr + i) & (cap - 1)) * 2;
                ring[k] = words[2 * (done + i)]; ring[k + 1] = words[2 * (done + i) + 1];
            }
            w.store(wr + n, std::memory_order_release);
            done += (int)n;
        }
        return !failed.load();
    }
};
} // namespace

std::unique_ptr<BitstreamDevice> bitstreamOpen(const std::string& device, int linkRate, int kind, std::string& err) {
    auto d = std::make_unique<CaBitstream>();
    if (!d->open(device, linkRate, kind, err)) return nullptr;
    return d;
}

} // namespace dect2
