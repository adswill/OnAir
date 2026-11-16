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

    @Test fun outputWaitIsShortWhilePrimingAndLongerOnceTheDecoderAnswers() {
        assertEquals(3_000L, XheSupport.outputWaitUs(false, 3))
        assertEquals(3_000L, XheSupport.outputWaitUs(true, 0))
        assertEquals(20_000L, XheSupport.outputWaitUs(true, 1))
    }

    @Test fun statsCountFramesInFlightAndNeverGoNegative() {
        val s = XheStats()
        s.framesIn = 10; s.framesOut = 8
        assertEquals(2L, s.inFlight())
        s.flushes++; s.framesInAtFlush = 10; s.framesOutAtFlush = 8
        assertEquals(0L, s.inFlight())
        s.framesOut = 12   // late output after a flush cannot make it negative
        assertEquals(0L, s.inFlight())
        assertTrue(s.line().contains("in 10") && s.line().contains("flushes 1"))
    }
}
