// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM xHE-AAC (USAC) on the phone: the core hands the compressed access units to the phone's own MediaCodec decoder (audio/XheAacDecoder.kt)
// through the portable hand-off in core/include/dect2/drm/drm_usac_ext.h, and takes 16-bit PCM back. No codec is in this library.
//
// THREADING: the calls come from the receiver's decode thread, which is not a Java thread: it is attached to the VM on its first call
// and detached when it ends. The Kotlin object serialises the calls itself.
#include <jni.h>
#include <android/log.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dect2/drm/drm_usac_ext.h"

using namespace dect2::drm;

namespace {

JavaVM* gVm = nullptr;

struct ThreadEnv {
    JNIEnv* e = nullptr;
    bool attached = false;
    ~ThreadEnv() { if (attached && gVm) gVm->DetachCurrentThread(); }
};

JNIEnv* env() {
    thread_local ThreadEnv t;
    if (t.e) return t.e;
    if (!gVm) return nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&t.e), JNI_VERSION_1_6) == JNI_OK) return t.e;
    if (gVm->AttachCurrentThread(&t.e, nullptr) != JNI_OK) { t.e = nullptr; return nullptr; }
    t.attached = true;
    return t.e;
}

std::string str(JNIEnv* e, jstring s) {
    if (!s) return std::string();
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    e->DeleteLocalRef(s);
    return r;
}

class AndroidUsac : public UsacExternalDecoder {
public:
    AndroidUsac(JNIEnv* e, jobject obj) {
        obj_ = e->NewGlobalRef(obj);
        jclass c = e->GetObjectClass(obj);
        mReason_ = e->GetMethodID(c, "unavailableReason", "()Ljava/lang/String;");
        mOpen_ = e->GetMethodID(c, "open", "([BII)Ljava/lang/String;");
        mDecode_ = e->GetMethodID(c, "decode", "([B)[S");
        mRate_ = e->GetMethodID(c, "outRate", "()I");
        mChannels_ = e->GetMethodID(c, "outChannels", "()I");
        mFlush_ = e->GetMethodID(c, "flush", "()V");
        mClose_ = e->GetMethodID(c, "close", "()V");
        e->DeleteLocalRef(c);
        if (e->ExceptionCheck()) { e->ExceptionClear(); ok_ = false; }
        else ok_ = mReason_ && mOpen_ && mDecode_ && mRate_ && mChannels_ && mFlush_ && mClose_;
    }
    ~AndroidUsac() override {
        if (JNIEnv* e = env()) e->DeleteGlobalRef(obj_);
    }
    bool ok() const { return ok_; }
    const char* name() const override { return "Android MediaCodec"; }

    std::string unavailableReason() override {
        JNIEnv* e = env();
        if (!e || !ok_) return usacNoDecoderText();
        std::string r = str(e, static_cast<jstring>(e->CallObjectMethod(obj_, mReason_)));
        if (fail(e)) return usacNoDecoderText();
        return r;
    }
    bool open(const std::vector<uint8_t>& asc, int rateHz, int channels, std::string& why) override {
        JNIEnv* e = env();
        if (!e || !ok_) { why = usacNoDecoderText(); return false; }
        jbyteArray a = e->NewByteArray(static_cast<jsize>(asc.size()));
        e->SetByteArrayRegion(a, 0, static_cast<jsize>(asc.size()), reinterpret_cast<const jbyte*>(asc.data()));
        const std::string r = str(e, static_cast<jstring>(e->CallObjectMethod(obj_, mOpen_, a, rateHz, channels)));
        e->DeleteLocalRef(a);
        if (fail(e)) { why = "the phone's xHE-AAC decoder failed to start"; return false; }
        if (!r.empty()) { why = r; return false; }
        return true;
    }
    Result decode(const std::vector<uint8_t>& au, std::vector<int16_t>& pcm, int& rateHz, int& channels) override {
        pcm.clear();
        JNIEnv* e = env();
        if (!e || !ok_) return Result::kError;
        jbyteArray a = e->NewByteArray(static_cast<jsize>(au.size()));
        e->SetByteArrayRegion(a, 0, static_cast<jsize>(au.size()), reinterpret_cast<const jbyte*>(au.data()));
        jshortArray out = static_cast<jshortArray>(e->CallObjectMethod(obj_, mDecode_, a));
        e->DeleteLocalRef(a);
        if (fail(e) || !out) { if (out) e->DeleteLocalRef(out); return Result::kError; }
        const jsize n = e->GetArrayLength(out);
        if (n > 0) {
            pcm.resize(static_cast<size_t>(n));
            e->GetShortArrayRegion(out, 0, n, reinterpret_cast<jshort*>(pcm.data()));
            rateHz = e->CallIntMethod(obj_, mRate_);
            channels = e->CallIntMethod(obj_, mChannels_);
        }
        e->DeleteLocalRef(out);
        if (fail(e)) return Result::kError;
        return n > 0 && rateHz > 0 && channels > 0 ? Result::kPcm : Result::kNoOutput;
    }
    void flush() override { call(mFlush_); }
    void close() override { call(mClose_); }

private:
    bool fail(JNIEnv* e) {
        if (!e->ExceptionCheck()) return false;
        e->ExceptionClear();
        return true;
    }
    void call(jmethodID m) {
        JNIEnv* e = env();
        if (!e || !ok_) return;
        e->CallVoidMethod(obj_, m);
        fail(e);
    }
    jobject obj_ = nullptr;
    jmethodID mReason_ = nullptr, mOpen_ = nullptr, mDecode_ = nullptr, mRate_ = nullptr, mChannels_ = nullptr, mFlush_ = nullptr, mClose_ = nullptr;
    bool ok_ = false;
};

} // namespace

#define XFN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_io_github_adswill_onair_audio_XheNative_##name

// Registers the Kotlin decoder object (audio/XheAacDecoder.kt) as the core's external xHE-AAC decoder. True when its methods were found.
XFN(jboolean, nativeXheRegister)(JNIEnv* e, jobject, jobject decoder) {
    if (!gVm) e->GetJavaVM(&gVm);
    if (!decoder) { usac_ext::setShared(nullptr); return JNI_FALSE; }
    auto d = std::make_shared<AndroidUsac>(e, decoder);
    if (!d->ok()) { __android_log_print(ANDROID_LOG_ERROR, "onair", "xHE-AAC decoder object has no matching methods"); return JNI_FALSE; }
    usac_ext::setShared(d);
    return JNI_TRUE;
}
