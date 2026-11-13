// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import io.github.adswill.onair.OnAirNative

/**
 * The radio-access part of libonair.so (native/src/jni_radio.cpp; tools/check_jni.py compares the two): USB radios handed over as
 * descriptors, network radios, the pushed sound-card input.
 *
 * THREADING: like [OnAirNative], call these only inside ReceiverEngine blocks (data/RadioRepository does), except [nativeAudioPush],
 * which the recording thread of audio/SoundCardInput calls (the push buffer has its own lock and touches nothing else).
 */
object RadioNative {
    /** The library is the same one: loaded by [OnAirNative]. */
    val loaded: Boolean get() = OnAirNative.loaded

    external fun nativeUsbFeatures(): String
    external fun nativeUsbAttach(name: String, kind: String, fd: Int, vid: Int, pid: Int, manufacturer: String, product: String, serial: String): Boolean
    external fun nativeUsbDetach(name: String)
    external fun nativeUsbRadios(): String
    external fun nativeNetDevice(spec: String): String
    external fun nativeNetProbe(spec: String, timeoutMs: Int): String
    external fun nativeNetSpec(type: Int, host: String, port: Int, remote: String): String
    external fun nativeAudioConfigure(rate: Int, seconds: Double)
    external fun nativeAudioPush(samples: FloatArray, frames: Int): Int
}
