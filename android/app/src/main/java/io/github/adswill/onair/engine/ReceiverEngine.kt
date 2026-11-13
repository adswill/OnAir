// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.engine

import android.os.SystemClock
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.SpectrumFrame
import io.github.adswill.onair.model.Telemetry
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineName
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.selects.onTimeout
import kotlinx.coroutines.selects.select
import java.util.concurrent.Executors

/**
 * The receiver thread. Every JNI call of the app runs here, one at a time, in the order it was asked for:
 *
 *  - [post] / [call] put a command into one FIFO queue (a serialised command queue: start, stop, retune, mode change ... can never
 *    overlap or overtake each other, so "stop while starting" simply runs after the start returns and undoes it);
 *  - between commands the same thread polls the engine: spectrum lines (~28 per second while running and visible), telemetry and the
 *    C++ log (about 3 per second), and notices when the engine stopped by itself (end of a file, a lost network radio).
 *
 * Results leave the thread only through [frames], [telemetry] and [gainNow] (StateFlows, safe to collect anywhere), the [AppLog], and
 * the [onEngineStopped] / [onFailure] callbacks (called on this thread; keep them short and thread-safe).
 *
 * One instance per process, owned by the ReceiverRepository.
 */
class ReceiverEngine(
    private val onFailure: (where: String, t: Throwable) -> Unit,
    private val onEngineStopped: (note: String) -> Unit,
) {
    private val executor = Executors.newSingleThreadExecutor { r -> Thread(r, THREAD_NAME).apply { isDaemon = true } }
    /** The dispatcher of the receiver thread. Only [ReceiverEngine] runs code on it. */
    private val dispatcher = executor.asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + dispatcher + CoroutineName("onair-receiver"))
    private val core = OnAirCore()

    private class Command(val name: String, val block: (OnAirCore) -> Any?, val reply: CompletableDeferred<Any?>? = null)
    private val queue = Channel<Command>(Channel.UNLIMITED)

    val libraryLoaded: Boolean get() = core.libraryLoaded

    /** Set after nativeInit succeeded: until then the poller leaves the engine alone. */
    @Volatile var ready = false
    /** The engine runs (set by the repository when a start succeeded, cleared on stop): read spectrum and telemetry. */
    @Volatile var polling = false
    /** An activity is on screen: spectrum lines are wanted. Without one, only telemetry, log and the running check go on. */
    @Volatile var uiVisible = true
    @Volatile var peakHold = true
    @Volatile var agc = false
    /** Ask for a full telemetry snapshot next time (after a mode change). */
    @Volatile var forceTelemetry = true

    private val _frames = MutableStateFlow(SpectrumFrame.empty)
    val frames: StateFlow<SpectrumFrame> = _frames.asStateFlow()
    private val _telemetry = MutableStateFlow<Telemetry?>(null)
    val telemetry: StateFlow<Telemetry?> = _telemetry.asStateFlow()
    private val _gainNow = MutableStateFlow(Double.NaN)
    /** The gain the AGC chose (NaN when unknown). */
    val gainNow: StateFlow<Double> = _gainNow.asStateFlow()

    init { scope.launch { loop() } }

    /** Queue [block] and do not wait. Failures are reported to [onFailure] and the log. */
    fun post(name: String, block: (OnAirCore) -> Unit) {
        queue.trySend(Command(name, { block(it) }))
    }

    /** Queue [block] and suspend until it ran; its result, or its exception. Cancelling the caller does not take the command back. */
    suspend fun <T> call(name: String, block: (OnAirCore) -> T): T {
        val reply = CompletableDeferred<Any?>()
        queue.send(Command(name, block, reply))
        @Suppress("UNCHECKED_CAST")
        return reply.await() as T
    }

    fun clearTelemetry() { _telemetry.value = null; forceTelemetry = true }

    /** Stops the engine and ends the thread. Only for process teardown and tests. */
    fun close() {
        post("close") { if (ready) it.stop() }
        queue.close()
    }

    private fun execute(c: Command) {
        try {
            val r = c.block(core)
            c.reply?.complete(r)
        } catch (e: CancellationException) {
            c.reply?.cancel(e)
        } catch (t: Throwable) {
            AppLog.e("engine", "${c.name} failed", t)
            onFailure(c.name, t)
            c.reply?.completeExceptionally(t)
        }
    }

    private suspend fun loop() {
        var nextTick = 0L
        var nextTele = 0L
        while (currentCoroutineContext().isActive) {
            val wait = nextTick - SystemClock.uptimeMillis()
            // select, not withTimeoutOrNull around receive: a command delivered just as the timeout fired was lost, its caller waited forever and
            // the receive session could no longer be stopped or changed (seen in the all-modes test)
            val c = if (wait > 0) select<Command?> {
                queue.onReceiveCatching { it.getOrNull() }
                onTimeout(wait) { null }
            } else queue.tryReceive().getOrNull()
            if (queue.isClosedForReceive && c == null) break
            if (c != null) {
                execute(c)
                if (SystemClock.uptimeMillis() < nextTick) continue
            }
            val now = SystemClock.uptimeMillis()
            try {
                if (ready) {
                    if (polling) pollRunning()
                    if (now >= nextTele) { pollTelemetryAndLog(); nextTele = now + TELEMETRY_MS }
                }
            } catch (e: CancellationException) { throw e
            } catch (t: Throwable) {
                AppLog.e("engine", "poll failed", t)
                onFailure("poll", t)
                polling = false
                try { core.stop() } catch (_: Throwable) {}
                onEngineStopped("Receiver error: ${t.message ?: t.javaClass.simpleName}")
            }
            nextTick = SystemClock.uptimeMillis() + when { polling && uiVisible -> FRAME_MS; polling -> HIDDEN_MS; else -> IDLE_MS }
        }
        AppLog.i("engine", "receiver thread ends")
        scope.cancel()
        executor.shutdown()
    }

    // ---- polling (receiver thread only) --------------------------------------------------------------------------------------
    private val buf = FloatArray(SpectrumFrame.BINS)
    private val sortBuf = FloatArray(SpectrumFrame.BINS)
    private val meta = DoubleArray(4)
    private var peak = FloatArray(SpectrumFrame.BINS) { -120f }
    private var wfFloor = -88f
    private var wfCeil = -25f
    private var seq = 0L
    private var logTotal = 0L

    private fun pollRunning() {
        if (!core.running()) {
            polling = false
            val note = core.note()
            AppLog.w("engine", "engine stopped: ${note.ifEmpty { "no reason given" }}")
            onEngineStopped(note)
            return
        }
        if (uiVisible && core.spectrum(buf, meta)) {
            val n = SpectrumFrame.BINS
            val next = FloatArray(n)
            val hold = peakHold
            for (i in 0 until n) next[i] = if (hold) maxOf(peak[i] - 0.05f, buf[i]) else buf[i]
            peak = next
            System.arraycopy(buf, 0, sortBuf, 0, n)
            sortBuf.sort()
            val med = sortBuf[n / 2]
            val top = sortBuf[n - 1 - n / 100]
            wfFloor += (med - 4f - wfFloor) * 0.05f
            wfCeil += (maxOf(med + 30f, top + 3f) - wfCeil) * 0.05f
            _frames.value = SpectrumFrame(++seq, buf.copyOf(), next, meta[0], meta[1], meta[2], meta[3], wfFloor, wfCeil)
        }
        if (agc) _gainNow.value = core.gainNow()
    }

    private fun pollTelemetryAndLog() {
        val force = forceTelemetry || _telemetry.value == null
        core.telemetry(force)?.let {
            _telemetry.value = it; forceTelemetry = false
            if (io.github.adswill.onair.BuildConfig.DEBUG) android.util.Log.i("OnAirSmoke", "std=${it.standard} rate=${meta[1]} lock=${it.lock} snr=${it.snrDb} cards=${it.cards.size} tables=${it.tables.size} rows=${it.tables.sumOf { t -> t.rows.size }} summary=${it.summary.take(100)}")
        }
        val (total, lines) = core.log(logTotal)
        logTotal = total
        if (lines.isNotEmpty()) {
            val t = System.currentTimeMillis()
            AppLog.addAll(lines.map { AppLog.Entry(t, levelOf(it), "core", it) })
        }
    }

    private fun levelOf(line: String): AppLog.Level {
        val l = line.lowercase()
        return when {
            "error" in l || "fail" in l -> AppLog.Level.ERROR
            "warn" in l -> AppLog.Level.WARN
            else -> AppLog.Level.INFO
        }
    }

    companion object {
        const val THREAD_NAME = "onair-receiver"
        const val FRAME_MS = 35L
        const val HIDDEN_MS = 100L
        const val IDLE_MS = 350L
        const val TELEMETRY_MS = 350L
    }
}
