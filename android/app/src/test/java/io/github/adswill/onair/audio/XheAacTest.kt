// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.audio

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** The pure parts of DRM xHE-AAC on the phone (the MediaCodec calls themselves need a phone). */
class XheAacTest {
    @Test fun statusLineWhenThePhoneCannotDecode() {
        assertEquals("This phone has no xHE-AAC decoder; DRM audio of this service cannot play", XheSupport.NO_DECODER)
        assertEquals(XheSupport.NO_DECODER, XheSupport.reason(27, true))    // before Android 9
        assertEquals(XheSupport.NO_DECODER, XheSupport.reason(34, false))   // no codec lists xHE-AAC
        assertEquals("", XheSupport.reason(28, true))
    }

    @Test fun profileListFindsXhe() {
        assertTrue(XheSupport.listsXhe(intArrayOf(2, 5, 29, 42)))
        assertFalse(XheSupport.listsXhe(intArrayOf(2, 5, 29, 39)))   // LC, HE, HE-PS, ELD: no USAC
        assertEquals(42, XheSupport.AAC_OBJECT_XHE)
    }

    @Test fun pcmToShorts() {
        val b = ByteBuffer.allocate(8).order(ByteOrder.nativeOrder())
        b.asShortBuffer().put(shortArrayOf(1, -2, 300, Short.MIN_VALUE)); b.limit(8)
        assertArrayEquals(shortArrayOf(1, -2, 300, Short.MIN_VALUE), XheSupport.toShorts(b, false))
        val f = ByteBuffer.allocate(12).order(ByteOrder.nativeOrder())
        f.asFloatBuffer().put(floatArrayOf(0f, 1.5f, -1f)); f.limit(12)
        assertArrayEquals(shortArrayOf(0, 32767, -32767), XheSupport.toShorts(f, true))
    }
}
