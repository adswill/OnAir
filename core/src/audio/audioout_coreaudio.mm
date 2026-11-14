// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// macOS backend: an AudioUnit on the default output device.
#include "audioout_impl.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <cstddef>
#include <string>
#include <vector>
#include "dect2/common/diag.h"

namespace dect2 {

// The unit normally converts our rate to the device's. When it refuses our stream format (the device now runs at another rate, e.g. 96 kHz
// after another app changed it, or a Bluetooth headset at 16 / 44.1 kHz) the old code ignored the error and the 48 kHz stream played at the
// device rate: choppy and pitched up or down. Then the stream is converted here (linear interpolation) to the rate the unit really uses.
struct CaRender {
    AudioOut::Impl* impl = nullptr;
    double ratio = 1.0;          // our frames per device frame (1: no conversion)
    double pos = 0;              // fractional read position in `src`
    std::vector<float> src;      // pending frames (C channels), src[0..] starting at integer position 0
    int C = 2;
};

static OSStatus renderCb(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 nFrames, AudioBufferList* io) {
    auto* c = static_cast<CaRender*>(ref);
    float* out = (float*)io->mBuffers[0].mData;
    if (c->ratio == 1.0) { c->impl->render(out, nFrames); return noErr; }
    const size_t C = (size_t)c->C;
    const size_t need = (size_t)(c->pos + nFrames * c->ratio) + 2;
    const size_t have = c->src.size() / C;
    if (have < need) {
        std::vector<float> t((need - have) * C);
        c->impl->render(t.data(), (uint32_t)(need - have));
        c->src.insert(c->src.end(), t.begin(), t.end());
    }
    for (UInt32 i = 0; i < nFrames; i++) {
        const size_t k = (size_t)c->pos;
        const float f = (float)(c->pos - (double)k);
        for (size_t ch = 0; ch < C; ch++) out[C * i + ch] = c->src[C * k + ch] * (1.f - f) + c->src[C * (k + 1) + ch] * f;
        c->pos += c->ratio;
    }
    const size_t used = (size_t)c->pos;
    c->src.erase(c->src.begin(), c->src.begin() + (std::ptrdiff_t)(used * C));
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

int audioDeviceChannels(const std::string& name) {
    AudioObjectID dev = 0;
    if (!name.empty()) for (auto& d : outputDevices()) if (d.second == name) { dev = d.first; break; }
    if (!dev) {
        UInt32 sz = sizeof dev;
        AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &dev) != noErr) return 0;
    }
    {   // the speakers the user set up (Audio MIDI Setup > Configure Speakers): only channels with a loudspeaker label count, so an
        // 18-output studio interface is not taken for a surround system
        AudioObjectPropertyAddress pl = {kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
        UInt32 lsz = 0;
        if (AudioObjectGetPropertyDataSize(dev, &pl, 0, nullptr, &lsz) == noErr && lsz >= offsetof(AudioChannelLayout, mChannelDescriptions)) {
            std::vector<uint8_t> lb(lsz);
            auto* L = reinterpret_cast<AudioChannelLayout*>(lb.data());
            if (AudioObjectGetPropertyData(dev, &pl, 0, nullptr, &lsz, L) == noErr) {
                int n = 0;
                if (L->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
                    for (UInt32 i = 0; i < L->mNumberChannelDescriptions && offsetof(AudioChannelLayout, mChannelDescriptions) + (i + 1) * sizeof(AudioChannelDescription) <= lsz; i++) {
                        const AudioChannelLabel c = L->mChannelDescriptions[i].mChannelLabel;
                        if ((c >= kAudioChannelLabel_Left && c <= kAudioChannelLabel_VerticalHeightRight) || (c >= kAudioChannelLabel_RearSurroundLeft && c <= kAudioChannelLabel_LFE2)) n++;
                    }
                } else if (L->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap) {
                    for (UInt32 b = L->mChannelBitmap; b; b &= b - 1) n++;
                } else n = (int)AudioChannelLayoutTag_GetNumberOfChannels(L->mChannelLayoutTag);
                if (n > 0) return n;
            }
        }
    }
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz < sizeof(AudioBufferList)) return 0;
    std::vector<uint8_t> buf(sz);
    auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, bl) != noErr) return 0;
    int n = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += (int)bl->mBuffers[i].mNumberChannels;
    return n;
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
    const UInt32 C = (UInt32)I->channels;
    f.mFramesPerPacket = 1; f.mChannelsPerFrame = C; f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4 * C; f.mBytesPerPacket = 4 * C;
    auto* ca = new CaRender;
    ca->impl = I;
    ca->C = (int)C;
    if (C > 2) {   // say which speaker each channel is for, so the unit maps them onto the device's speaker setup (Audio MIDI Setup)
        const AudioChannelLabel l51[6] = {kAudioChannelLabel_Left, kAudioChannelLabel_Right, kAudioChannelLabel_Center, kAudioChannelLabel_LFEScreen,
                                          kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RightSurround};
        const AudioChannelLabel l71[8] = {kAudioChannelLabel_Left, kAudioChannelLabel_Right, kAudioChannelLabel_Center, kAudioChannelLabel_LFEScreen,
                                          kAudioChannelLabel_RearSurroundLeft, kAudioChannelLabel_RearSurroundRight, kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RightSurround};
        const size_t lsz = offsetof(AudioChannelLayout, mChannelDescriptions) + sizeof(AudioChannelDescription) * C;
        std::vector<uint8_t> lb(lsz, 0);
        auto* lay = reinterpret_cast<AudioChannelLayout*>(lb.data());
        lay->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
        lay->mNumberChannelDescriptions = C;
        for (UInt32 i = 0; i < C; i++) lay->mChannelDescriptions[i].mChannelLabel = C == 8 ? l71[i] : l51[i];
        AudioUnitSetProperty(unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, lay, (UInt32)lsz);
    }
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr) {
        // refused: play at the format the unit has and convert to it ourselves
        AudioStreamBasicDescription g = {};
        UInt32 gs = sizeof g;
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &g, &gs) == noErr && g.mSampleRate > 0 && g.mChannelsPerFrame == C &&
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
        DIAG(677231, "CoreAudio could not start the output unit, so sound falls back to the other audio backend.");
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
