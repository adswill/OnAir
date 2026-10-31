package io.github.adswill.onair.diag

import android.os.Handler
import android.os.Looper
import android.view.Choreographer

/**
 * Verbose logging of the UI's frame times (Settings > Verbose logging): every 2 s one line with the number of frames, the average and the
 * worst gap between two frames and how many threads the process has, into [AppLog]. A frozen spectrum on one phone and not on another is
 * told apart this way: a UI that still draws (frames keep coming) against a UI thread that is stuck (no frames, a long worst gap).
 * Off costs nothing: the callback is not posted.
 */
object FrameStats {
    private val main = Handler(Looper.getMainLooper())
    private var running = false
    private var last = 0L
    private var windowStart = 0L
    private var frames = 0
    private var worstNs = 0L
    private var sumNs = 0L

    private val cb = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!running) return
            if (last != 0L) { val d = frameTimeNanos - last; frames++; sumNs += d; if (d > worstNs) worstNs = d }
            last = frameTimeNanos
            if (windowStart == 0L) windowStart = frameTimeNanos
            if (frameTimeNanos - windowStart >= 2_000_000_000L && frames > 0) {
                AppLog.d("ui", "frames %d in %.1f s, average %.1f ms, worst %.1f ms; %d threads".format(
                    frames, (frameTimeNanos - windowStart) / 1e9, sumNs / frames / 1e6, worstNs / 1e6, Thread.getAllStackTraces().size))
                windowStart = frameTimeNanos; frames = 0; worstNs = 0; sumNs = 0
            }
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    fun set(on: Boolean) {
        main.post {
            if (on == running) return@post
            running = on
            last = 0; windowStart = 0; frames = 0; worstNs = 0; sumNs = 0
            if (on) Choreographer.getInstance().postFrameCallback(cb) else Choreographer.getInstance().removeFrameCallback(cb)
        }
    }
}
