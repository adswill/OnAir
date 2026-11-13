// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class AvSyncTest {
    @Test fun audioClockFollowsThePlayingBlockAcrossAGap() {
        val c = AudioClock().apply { sampleRate = 48000 }
        assertEquals(NO_PTS, c.nowUs(0))
        c.wrote(48000, 10_000_000)       // one second from t = 10 s
        c.wrote(24000, 11_500_000)       // then a gap of half a second in the stream (lost PES), half a second of sound
        c.playedFrames = 0; assertEquals(10_000_000L, c.nowUs(0))
        c.playedFrames = 24000; assertEquals(10_500_000L, c.nowUs(0))
        c.playedFrames = 48000; assertEquals(11_500_000L, c.nowUs(0))
        c.playedFrames = 60000; assertEquals(11_750_000L, c.nowUs(0))
        assertEquals(250_000L, c.bufferedUs)
        c.extraLatencyUs = 40_000; assertEquals(11_710_000L, c.nowUs(0))
    }

    @Test fun wallClockRunsFromTheFirstPicture() {
        val w = WallClock()
        assertEquals(NO_PTS, w.nowUs(5))
        w.start(1_000_000, 2_000_000_000)
        assertEquals(1_000_000L, w.nowUs(2_000_000_000))
        assertEquals(1_040_000L, w.nowUs(2_040_000_000))
    }

    @Test fun showsDropsAndHolds() {
        val s = AvSync()
        val now = 1_000_000_000L
        assertEquals(FrameAction.Show(now + 20_000_000), s.decide(1_020_000, 1_000_000, now))   // 20 ms early: scheduled
        assertEquals(FrameAction.Show(now), s.decide(970_000, 1_000_000, now))                  // 30 ms late: still shown at once
        assertEquals(FrameAction.Drop, s.decide(900_000, 1_000_000, now))                       // 100 ms late: dropped
        val h = s.decide(1_800_000, 1_000_000, now)                                             // 800 ms early: held
        assertTrue(h is FrameAction.Hold && h.retryMs in 5..200)
        assertEquals(FrameAction.Show(now), s.decide(60_000_000, 1_000_000, now))               // a jump: shown, counted
        assertEquals(1, s.jumps)
        assertEquals(1, s.dropped)
        assertEquals(FrameAction.Show(now), s.decide(NO_PTS, 1_000_000, now))
    }

    /** A 25 fps picture against a 48 kHz sound clock that starts 300 ms after the first picture: nothing early is shown late. */
    @Test fun simulatedPlaybackStaysInStep() {
        val c = AudioClock().apply { sampleRate = 48000 }
        val s = AvSync()
        c.wrote(48000 * 10, 300_000)
        var lateShown = 0
        for (k in 0 until 250) {
            val pts = k * 40_000L
            val playedUs = pts - 10_000   // the picture is decoded 10 ms before its time
            c.playedFrames = ((playedUs - 300_000).coerceAtLeast(0) * 48 / 1000)
            val clock = if (playedUs < 300_000) NO_PTS else c.nowUs(0)
            val a = s.decide(pts, clock, 0)
            if (a is FrameAction.Show && clock != NO_PTS && pts - clock < -s.lateUs) lateShown++
        }
        assertEquals(0, lateShown)
        assertEquals(0, s.dropped)
    }
}
