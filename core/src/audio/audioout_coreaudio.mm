// macOS backend: an AudioUnit on the default output device.
#include "audioout_impl.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <string>
#include <vector>

namespace dect2 {

// The unit normally converts our rate to the device's. When it refuses our stream format (the device now runs at another rate, e.g. 96 kHz
// after another app changed it, or a Bluetooth headset at 16 / 44.1 kHz) the old code ignored the error and the 48 kHz stream played at the
// device rate: choppy and pitched up or down. Then the stream is converted here (linear interpolation) to the rate the unit really uses.
struct CaRender {
    AudioOut::Impl* impl = nullptr;
    double ratio = 1.0;          // our frames per device frame (1: no conversion)
    double pos = 0;              // fractional read position in `src`
    std::vector<float> src;      // pending stereo frames, src[0..] starting at integer position 0
    float last[2] = {0, 0};
};

static OSStatus renderCb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 nFrames, AudioBufferList* io) {
    auto* c = static_cast<CaRender*>(ref);
    float* out = (float*)io->mBuffers[0].mData;
    if (c->ratio == 1.0) { c->impl->render(out, nFrames); return noErr; }
    const size_t need = (size_t)(c->pos + nFrames * c->ratio) + 2;
    const size_t have = c->src.size() / 2;
    if (have < need) {
        std::vector<float> t((need - have) * 2);
        c->impl->render(t.data(), (uint32_t)(need - have));
        c->src.insert(c->src.end(), t.begin(), t.end());
    }
    for (UInt32 i = 0; i < nFrames; i++) {
        const size_t k = (size_t)c->pos;
        const float f = (float)(c->pos - (double)k);
        for (int ch = 0; ch < 2; ch++) out[2 * i + ch] = c->src[2 * k + ch] * (1.f - f) + c->src[2 * (k + 1) + ch] * f;
        c->pos += c->ratio;
    }
    const size_t used = (size_t)c->pos;
    c->src.erase(c->src.begin(), c->src.begin() + (std::ptrdiff_t)(used * 2));
    c->pos -= (double)used;
    return noErr;
}

// Output devices: (id, name) of every device that has output streams.
static std::vector<std::pair<AudioObjectID, std::string>> outputDevices() {
    std::vector<std::pair<AudioObjectID, std::string>> v;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr || !sz) return v;
    std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return v;
    for (AudioObjectID id : ids) {
        AudioObjectPropertyAddress st = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
        UInt32 ssz = 0;
        if (AudioObjectGetPropertyDataSize(id, &st, 0, nullptr, &ssz) != noErr || !ssz) continue;   // no output streams
        AudioObjectPropertyAddress nm = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef cf = nullptr; UInt32 csz = sizeof cf;
        if (AudioObjectGetPropertyData(id, &nm, 0, nullptr, &csz, &cf) != noErr || !cf) continue;
        char buf[256] = {};
        CFStringGetCString(cf, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(cf);
        v.emplace_back(id, buf);
    }
    return v;
}

std::vector<std::string> audioListOutputs() {
    std::vector<std::string> r;
    for (auto& d : outputDevices()) r.push_back(d.second);
    return r;
}

bool audioBackendStart(AudioOut::Impl* I, int rate) {
    AudioObjectID dev = 0;
    if (!I->device.empty()) for (auto& d : outputDevices()) if (d.second == I->device) { dev = d.first; break; }
    AudioComponentDescription d = {kAudioUnitType_Output, dev ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(nullptr, &d);
    AudioUnit unit = nullptr;
    if (!c || AudioComponentInstanceNew(c, &unit) != noErr) return false;
    if (dev) {   // a named device: the HAL unit, output only
        UInt32 off = 0, on = 1;
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &off, sizeof off);
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &on, sizeof on);
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    }
    AudioStreamBasicDescription f = {};
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 8; f.mBytesPerPacket = 8;
    auto* ca = new CaRender;
    ca->impl = I;
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr) {
        // refused: play at the format the unit has and convert to it ourselves
        AudioStreamBasicDescription g = {};
        UInt32 gs = sizeof g;
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &g, &gs) == noErr && g.mSampleRate > 0 && g.mChannelsPerFrame == 2 &&
            (g.mFormatFlags & kAudioFormatFlagIsFloat) && g.mBitsPerChannel == 32 && !(g.mFormatFlags & kAudioFormatFlagIsNonInterleaved))
            ca->ratio = (double)rate / g.mSampleRate;
        else {   // an unusual native format: ask for it with our layout at the unit's rate
            AudioStreamBasicDescription h = f;
            h.mSampleRate = g.mSampleRate > 0 ? g.mSampleRate : rate;
            if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &h, sizeof h) == noErr) ca->ratio = (double)rate / h.mSampleRate;
        }
    }
    AURenderCallbackStruct cb = {renderCb, ca};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(unit) != noErr || AudioOutputUnitStart(unit) != noErr) {
        AudioComponentInstanceDispose(unit);
        delete ca;
        return false;
    }
    I->backend = unit;
    I->backendAux = ca;
    return true;
}

void audioBackendStop(AudioOut::Impl* I) {
    AudioUnit unit = (AudioUnit)I->backend;
    if (!unit) return;
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    I->backend = nullptr;
    delete static_cast<CaRender*>(I->backendAux);
    I->backendAux = nullptr;
}

} // namespace dect2
