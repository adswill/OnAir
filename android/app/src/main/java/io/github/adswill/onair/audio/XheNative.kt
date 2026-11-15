// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.audio

import io.github.adswill.onair.OnAirNative

/**
 * The xHE-AAC hand-off of libonair.so (native/src/jni_xhe.cpp; tools/check_jni.py compares the two). The core's DRM audio decoder calls
 * the registered [XheAacDecoder] from its own decode thread through core/include/dect2/drm/drm_usac_ext.h.
 */
object XheNative {
    val loaded: Boolean get() = OnAirNative.loaded

    /** Makes [decoder] (an [XheAacDecoder]) the core's external xHE-AAC decoder. True when the core found all of its methods. */
    external fun nativeXheRegister(decoder: Any): Boolean
}
