package io.github.adswill.onair.data

import io.github.adswill.onair.engine.RecStats
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class IqRecordingTest {
    @Test fun sizeAndClock() {
        assertEquals("1.5 MB", IqRecording.size(1572864L))
        assertEquals("1.00 GB", IqRecording.size(1073741824L))
        assertEquals("2:05", IqRecording.clock(125.4))
        assertEquals(1024, IqRecording.DEFAULT_CAP_MB)
    }

    @Test fun statsParse() {
        val s = RecStats.parse("1\t12.500000\t25000000\t0\t0\t\t/x/iq/a.cs8")
        assertTrue(s.active); assertEquals(25000000L, s.bytes); assertEquals(12.5, s.seconds, 1e-9); assertFalse(s.capReached)
        assertEquals("/x/iq/a.cs8", s.path)
        val c = RecStats.parse("0\t3\t100\t5\t1\tdisk full\t/p")
        assertFalse(c.active); assertTrue(c.capReached); assertEquals("disk full", c.error)
        assertFalse(RecStats.parse("garbage").active)
    }
}
