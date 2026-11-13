// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.tv

import android.graphics.Bitmap
import io.github.adswill.onair.diag.AppLog
import kotlinx.coroutines.CoroutineName
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.nio.ByteBuffer

/** What the analogue Watch panel draws: the newest picture (two bitmaps take turns, so the one on screen is never written), its facts. */
data class AnalogUiState(
    val source: AnalogSource? = null,
    val libraryLoaded: Boolean = true,
    val bitmap: Bitmap? = null,
    val info: AnalogFrameInfo? = null,
    val pictures: Long = 0,
)

/**
 * The analogue TV pictures (Analog TV, MAC, FPV video), one per process (AppGraph.analogTv): while the panel is on screen it asks the core for
 * the newest picture a little faster than the signal's frame rate ([analogPollMs]) and copies it into a Bitmap. The sound is the receiver's
 * own (the engine plays it as on the desktop); nothing is decoded here.
 */
class AnalogTvController {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + CoroutineName("onair-atv-ui"))
    private val _state = MutableStateFlow(AnalogUiState(libraryLoaded = TvNative.loaded))
    val state: StateFlow<AnalogUiState> = _state.asStateFlow()
    private var job: Job? = null

    fun show(source: AnalogSource?) {
        job?.cancel(); job = null
        _state.value = AnalogUiState(source = source, libraryLoaded = TvNative.loaded)
        if (source == null || !TvNative.loaded) return
        job = scope.launch {
            val meta = DoubleArray(6)
            var buf = ByteArray(768 * 576 * 4)
            val bitmaps = arrayOfNulls<Bitmap>(2)
            var turn = 0
            var count = 0L
            var pollMs = 200L
            while (isActive) {
                val n = try { TvNative.nativeAnalogFrame(source.code, meta, buf) } catch (e: Throwable) { AppLog.e("tv", "analogue picture failed", e); -1 }
                if (n < -1) {   // a larger picture than the buffer: grow and ask again at once (the picture number was not advanced)
                    buf = ByteArray(-n)
                    meta[0] = (meta[0] - 1).coerceAtLeast(0.0)
                    continue
                }
                val info = if (n > 0) AnalogFrameInfo.from(meta) else null
                if (info != null && n >= info.bytes) {
                    var b = bitmaps[turn]
                    if (b == null || b.width != info.width || b.height != info.height) {
                        b = Bitmap.createBitmap(info.width, info.height, Bitmap.Config.ARGB_8888)
                        bitmaps[turn] = b
                    }
                    b!!.copyPixelsFromBuffer(ByteBuffer.wrap(buf, 0, info.bytes))   // ARGB_8888 is RGBA in memory: the core's byte order
                    count++
                    _state.value = AnalogUiState(source, true, b, info, count)
                    turn = 1 - turn
                    pollMs = analogPollMs(info.fieldRate)
                }
                delay(pollMs)
            }
        }
    }

    fun hide() = show(null)
}
