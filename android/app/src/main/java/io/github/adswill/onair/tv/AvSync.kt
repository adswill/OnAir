// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

/*
 * Picture and sound in step, on the presentation timestamps (PTS) of the stream. The sound is the master clock when there is sound:
 * the time of the sample the loudspeaker plays now ([AudioClock]); without sound a wall clock started at the first picture
 * ([WallClock]). Each decoded picture is then shown at its time, dropped when it is too late, or held when it is early ([AvSync]).
 * Pure Kotlin (AvSyncTest).
 */

/** The playback clock in stream microseconds, or [NO_PTS] while it does not run. */
interface MasterClock {
    fun nowUs(wallNs: Long): Long
}

/**
 * Sound clock: maps the output position of the audio sink (frames played, as AudioTrack.getPlaybackHeadPosition reports them) back to
 * stream time through the PTS of each block written. Gaps in the sound (a lost PES) are handled because every block carries its own
 * time: the clock follows the block that is playing.
 */
class AudioClock : MasterClock {
    private class Mark(val frameIndex: Long, val ptsUs: Long)
    private val marks = ArrayDeque<Mark>()
    private var written = 0L
    var sampleRate = 48000
    /** Frames the sink has played, set by the player from the sink (getPlaybackHeadPosition). */
    @Volatile var playedFrames = 0L
    /** The sink's output latency beyond the head position, microseconds (0 when unknown). */
    var extraLatencyUs = 0L

    /** [frames] frames starting at stream time [ptsUs] were handed to the sink. */
    fun wrote(frames: Int, ptsUs: Long) {
        if (frames <= 0) return
        marks.addLast(Mark(written, ptsUs))
        written += frames
        while (marks.size > 512) marks.removeFirst()
    }

    fun reset() { marks.clear(); written = 0; playedFrames = 0 }

    val writtenFrames: Long get() = written
    /** Sound written but not yet played, microseconds. */
    val bufferedUs: Long get() = if (sampleRate > 0) (written - playedFrames).coerceAtLeast(0) * 1_000_000L / sampleRate else 0

    override fun nowUs(wallNs: Long): Long = timeAt(playedFrames) - extraLatencyUs

    /** Stream time of the sample with index [frame] (counted from the first one written). */
    fun timeAt(frame: Long): Long {
        if (marks.isEmpty() || sampleRate <= 0) return NO_PTS
        var m = marks.first()
        for (x in marks) { if (x.frameIndex <= frame) m = x else break }
        return m.ptsUs + (frame - m.frameIndex) * 1_000_000L / sampleRate
    }
}

/** Wall clock for a service without sound: starts at the first picture, then runs with the phone's clock. */
class WallClock : MasterClock {
    private var anchorPtsUs = NO_PTS
    private var anchorNs = 0L
    fun start(ptsUs: Long, wallNs: Long) { anchorPtsUs = ptsUs; anchorNs = wallNs }
    fun reset() { anchorPtsUs = NO_PTS }
    val running: Boolean get() = anchorPtsUs != NO_PTS
    override fun nowUs(wallNs: Long): Long = if (anchorPtsUs == NO_PTS) NO_PTS else anchorPtsUs + (wallNs - anchorNs) / 1000
}

/** What to do with a decoded picture. */
sealed class FrameAction {
    /** Show it at the phone time [atWallNs] (MediaCodec.releaseOutputBuffer(index, ns) schedules it). */
    data class Show(val atWallNs: Long) : FrameAction()
    /** It is too late: release it without showing. */
    object Drop : FrameAction()
    /** It is early by more than the decoder may schedule ahead: keep it and ask again in [retryMs]. */
    data class Hold(val retryMs: Long) : FrameAction()
}

/**
 * The decision for each picture. [lateUs]: older than this behind the clock is dropped (a picture shown 80 ms late is worse than a
 * skipped one); [aheadUs]: how far ahead MediaCodec is asked to schedule (Android allows up to a second); [jumpUs]: an offset this large
 * is a broken or jumped timestamp, not a wait: the picture is shown now and the counters note a jump.
 */
class AvSync(val lateUs: Long = 80_000, val aheadUs: Long = 500_000, val jumpUs: Long = 3_000_000) {
    var shown = 0L; private set
    var dropped = 0L; private set
    var jumps = 0L; private set

    fun decide(framePtsUs: Long, clockUs: Long, wallNs: Long): FrameAction {
        if (framePtsUs == NO_PTS || clockUs == NO_PTS) { shown++; return FrameAction.Show(wallNs) }
        val d = framePtsUs - clockUs
        return when {
            d < -jumpUs || d > jumpUs -> { jumps++; shown++; FrameAction.Show(wallNs) }
            d < -lateUs -> { dropped++; FrameAction.Drop }
            d > aheadUs -> FrameAction.Hold(((d - aheadUs) / 1000).coerceIn(5, 200))
            else -> { shown++; FrameAction.Show(wallNs + maxOf(0L, d) * 1000) }
        }
    }

    fun reset() { shown = 0; dropped = 0; jumps = 0 }
}
