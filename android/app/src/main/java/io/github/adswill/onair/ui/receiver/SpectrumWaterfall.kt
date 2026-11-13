// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.receiver

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.Palette
import io.github.adswill.onair.R
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.data.ReceiverState
import io.github.adswill.onair.model.SpectrumFrame
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.Waterfall
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

/** The visible part of the capture as fractions of the span, centred on [anchorHz] where the capture allows, for a zoom of 1 or more. */
internal fun viewWindow(zoom: Float, centreHz: Double, spanHz: Double, anchorHz: Double): Pair<Float, Float> {
    val z = zoom.coerceAtLeast(1f)
    if (spanHz <= 0) return 0f to 1f
    val cf = (0.5 + (anchorHz - centreHz) / spanHz).toFloat().coerceIn(0f, 1f)
    var lo = cf - 0.5f / z
    var hi = cf + 0.5f / z
    if (lo < 0f) { hi -= lo; lo = 0f }
    if (hi > 1f) { lo -= hi - 1f; hi = 1f }
    return max(0f, lo) to min(1f, hi)
}

internal fun niceStep(span: Double, targetTicks: Int): Double {
    if (span <= 0) return 1.0
    val raw = span / targetTicks
    val p = 10.0.pow(floor(log10(raw)))
    val m = raw / p
    return p * (if (m < 1.5) 1.0 else if (m < 3.5) 2.0 else if (m < 7.5) 5.0 else 10.0)
}

internal fun bwText(hz: Double): String = if (hz >= 1e6) String.format(Locale.US, "%.1f MHz", hz / 1e6) else String.format(Locale.US, "%.0f kHz", hz / 1e3)

/**
 * The hero of the receiver screen: spectrum (top) and waterfall (below) on one touch surface, as on the desktop.
 *  - tap: tune to the touched frequency; double tap: zoom back to the whole capture;
 *  - pinch: zoom (1 to 64 times) around the fingers; drag: in centre tuning it tunes (the view follows the dial), in normal tuning it
 *    pans the view and the dial marker moves inside it (the desktop's centre / normal toggle).
 * Drawing: one Canvas each, reading [ReceiverViewModel.specVersion] and [Waterfall.version] in the draw phase, so a new line redraws
 * the two canvases and recomposes nothing. The waterfall is one bitmap ring written row by row (no per-frame allocation); paths are
 * reused. Zoom and pan are UI state (saved across rotation); the frequency is the repository's.
 */
@Composable
fun SpectrumWaterfall(vm: ReceiverViewModel, centreTuning: Boolean, modifier: Modifier = Modifier, onToggleCentre: (() -> Unit)? = null) {
    val frame = vm.frame.collectAsStateWithLifecycle()
    val version = vm.specVersion.collectAsStateWithLifecycle()
    val st = vm.state.collectAsStateWithLifecycle()
    val settings = vm.settings.collectAsStateWithLifecycle()
    SpectrumWaterfallContent(
        frame = frame, version = version, state = st, settings = settings, waterfall = vm.waterfall, centreTuning = centreTuning,
        onTune = vm::tune, onToggleCentre = onToggleCentre, modifier = modifier,
    )
}

/** The stateless body (previews and tests drive it with fixed State objects). */
@Composable
fun SpectrumWaterfallContent(
    frame: State<SpectrumFrame>, version: State<Long>, state: State<ReceiverState>, settings: State<Settings>, waterfall: Waterfall,
    centreTuning: Boolean, onTune: (Double) -> Unit, onToggleCentre: (() -> Unit)?, modifier: Modifier = Modifier,
) {
    val p = LocalPalette.current
    val measurer = rememberTextMeasurer()
    val bmp = remember(waterfall) { waterfall.bmp.asImageBitmap() }
    var zoom by rememberSaveable { mutableFloatStateOf(1f) }
    // normal tuning: the view's own centre (absolute Hz); NaN = follow the dial
    var anchor by rememberSaveable { mutableDoubleStateOf(Double.NaN) }
    val centre by rememberUpdatedState(centreTuning)
    val tune by rememberUpdatedState(onTune)
    fun anchorHz(): Double = if (centre || anchor.isNaN()) state.value.freqMhz * 1e6 else anchor
    val label = remember(p) { TextStyle(color = p.plotText, fontSize = 10.sp, fontFamily = Mono) }
    val trace = remember { Path() }
    val peak = remember { Path() }
    val freqText = String.format(Locale.US, "%.6f", state.value.freqMhz).trimEnd('0').trimEnd('.')
    val desc = stringResource(R.string.cd_spectrum, freqText)
    val aZoomIn = stringResource(R.string.cd_zoom_in)
    val aZoomOut = stringResource(R.string.cd_zoom_out)
    val aUp = stringResource(R.string.cd_tune_up)
    val aDown = stringResource(R.string.cd_tune_down)

    Column(
        modifier.background(p.plate)
            .semantics {
                contentDescription = desc
                customActions = listOf(
                    CustomAccessibilityAction(aZoomIn) { zoom = (zoom * 2f).coerceAtMost(64f); true },
                    CustomAccessibilityAction(aZoomOut) { zoom = (zoom / 2f).coerceAtLeast(1f); true },
                    CustomAccessibilityAction(aUp) { val f = frame.value; if (f.spanHz > 0) tune(state.value.freqMhz + f.spanHz / zoom / 20 / 1e6); true },
                    CustomAccessibilityAction(aDown) { val f = frame.value; if (f.spanHz > 0) tune(state.value.freqMhz - f.spanHz / zoom / 20 / 1e6); true },
                )
            }
            .pointerInput(Unit) {
                detectTransformGestures { centroid, pan, pinch, _ ->
                    val fr = frame.value
                    if (fr.spanHz <= 0) return@detectTransformGestures
                    val w = size.width.toFloat().coerceAtLeast(1f)
                    if (pinch != 1f) {
                        val (lo, hi) = viewWindow(zoom, fr.centreHz, fr.spanHz, anchorHz())
                        val focalHz = fr.centreHz + (lo + centroid.x / w * (hi - lo) - 0.5) * fr.spanHz
                        val nz = (zoom * pinch).coerceIn(1f, 64f)
                        if (!centre) anchor = focalHz + (anchorHz() - focalHz) * zoom / nz
                        zoom = nz
                    }
                    if (abs(pan.x) > 0f) {
                        val (lo, hi) = viewWindow(zoom, fr.centreHz, fr.spanHz, anchorHz())
                        val dHz = pan.x / w * (hi - lo) * fr.spanHz
                        if (centre) tune(state.value.freqMhz - dHz / 1e6)
                        else anchor = (anchorHz() - dHz).coerceIn(fr.centreHz - fr.spanHz / 2, fr.centreHz + fr.spanHz / 2)
                    }
                }
            }
            .pointerInput(Unit) {
                detectTapGestures(onDoubleTap = { zoom = 1f; anchor = Double.NaN }) { o ->
                    val fr = frame.value
                    if (fr.spanHz > 0) {
                        if (!centre && anchor.isNaN()) anchor = anchorHz()        // the view stays where it is; the marker moves
                        val (lo, hi) = viewWindow(zoom, fr.centreHz, fr.spanHz, anchorHz())
                        tune((fr.centreHz + (lo + o.x / size.width * (hi - lo) - 0.5) * fr.spanHz) / 1e6)
                    }
                }
            },
    ) {
        Box(Modifier.weight(0.45f).fillMaxWidth()) {
            Canvas(Modifier.fillMaxSize()) {
                version.value                               // read in the draw phase: a new line invalidates only this draw
                drawSpectrum(frame.value, state.value, settings.value, zoom, anchorHz(), p, measurer, label, trace, peak)
            }
            Row(Modifier.align(Alignment.TopEnd).padding(4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
                val bw = state.value.channelHz()
                if (bw > 0) Chip("BW " + bwText(bw), color = p.plotText, minHeight = false)
                if (zoom > 1.01f) Chip(String.format(Locale.US, "x%.1f", zoom), color = p.plotText, minHeight = false, onClick = { zoom = 1f; anchor = Double.NaN })
                if (onToggleCentre != null) Chip(
                    stringResource(if (centreTuning) R.string.tuning_centre else R.string.tuning_normal),
                    color = if (centreTuning) p.accent else p.plotText, selected = centreTuning, onClick = onToggleCentre,
                )
            }
        }
        Box(Modifier.weight(0.55f).fillMaxWidth()) {
            Canvas(Modifier.fillMaxSize()) {
                waterfall.version                           // read in the draw phase: a committed row redraws only this canvas
                drawWaterfall(frame.value, state.value.freqMhz, zoom, anchorHz(), waterfall, bmp, p, measurer, label)
            }
            if (!state.value.running) Text(
                stringResource(R.string.not_running_hint), Modifier.padding(12.dp), color = p.plotText, fontFamily = Mono, fontSize = TypeScale.SMALL.sp,
            )
        }
    }
}

private fun DrawScope.drawSpectrum(
    fr: SpectrumFrame, s0: ReceiverState, set: Settings, zoom: Float, anchorHz: Double, p: Palette, measurer: TextMeasurer,
    label: TextStyle, trace: Path, peak: Path,
) {
    val w = size.width; val h = size.height
    val freqMhz = s0.freqMhz
    val (lo, hi) = viewWindow(zoom, fr.centreHz, fr.spanHz, anchorHz)
    val lo0 = set.minDb; val hi0 = max(set.maxDb, set.minDb + 1f)
    // the tuned channel: a light band around the dial frequency, like the desktop
    val bwHz = s0.channelHz(maxMhz = 0.5).let { if (it >= 1e3 || s0.mode.family == 5) it else 0.0 }
    if (fr.spanHz > 0 && bwHz > 0) {
        val c = (((freqMhz * 1e6 - fr.centreHz) / fr.spanHz + 0.5 - lo) / (hi - lo) * w).toFloat()
        val half = (bwHz / 2 / fr.spanHz / (hi - lo) * w).toFloat().coerceAtLeast(2f)
        drawRect(p.accent.copy(alpha = 0.14f), Offset(c - half, 0f), Size(2 * half, h))
    }
    // grid: every 20 dB, and the frequency ticks
    var db = ceil(lo0 / 20f) * 20f
    while (db <= hi0) {
        val y = h - (db - lo0) / (hi0 - lo0) * h
        drawLine(p.grid, Offset(0f, y), Offset(w, y), 1f)
        drawText(measurer, String.format(Locale.US, "%.0f", db), Offset(3f, (y - 12.sp.toPx()).coerceAtLeast(0f)), label)
        db += 20f
    }
    if (fr.spanHz <= 0) return
    val vspan = (hi - lo) * fr.spanHz
    val step = niceStep(vspan, 5)
    val f0 = fr.centreHz + (lo - 0.5) * fr.spanHz
    var f = ceil(f0 / step) * step
    val decimals = if (step >= 1e6) 1 else if (step >= 1e5) 2 else if (step >= 1e4) 3 else 4
    while (f <= f0 + vspan) {
        val x = ((f - f0) / vspan * w).toFloat()
        drawLine(p.grid, Offset(x, 0f), Offset(x, h), 1f)
        if (x > 0f && x < w - 70.dp.toPx()) drawText(measurer, String.format(Locale.US, "%.${decimals}f", f / 1e6), Offset(x + 3f, h - 14.sp.toPx()), label)
        f += step
    }
    val bins = fr.bins
    if (bins < 2) return
    val i0 = (lo * bins).toInt().coerceIn(0, bins - 2)
    val i1 = (hi * bins).toInt().coerceIn(i0 + 2, bins)
    fun build(path: Path, a: FloatArray) {
        path.reset()
        val n = (i1 - i0 - 1).coerceAtLeast(1)
        for (i in i0 until i1) {
            val x = (i - i0).toFloat() / n * w
            val y = (h - (a[i] - lo0) / (hi0 - lo0) * h).coerceIn(0f, h)
            if (i == i0) path.moveTo(x, y) else path.lineTo(x, y)
        }
    }
    if (set.peakHold) { build(peak, fr.peak); drawPath(peak, p.accent.copy(alpha = 0.55f), style = Stroke(1.2f)) }
    build(trace, fr.db)
    drawPath(trace, p.trace, style = Stroke(1.3f, cap = StrokeCap.Round))
    val xd = (((freqMhz * 1e6 - fr.centreHz) / fr.spanHz + 0.5 - lo) / (hi - lo) * w).toFloat()
    drawLine(p.accent, Offset(xd, 0f), Offset(xd, h), 1.5f)
}

private fun DrawScope.drawWaterfall(
    fr: SpectrumFrame, freqMhz: Double, zoom: Float, anchorHz: Double, wf: Waterfall, bmp: ImageBitmap, p: Palette,
    measurer: TextMeasurer, label: TextStyle,
) {
    val (lo, hi) = viewWindow(zoom, fr.centreHz, fr.spanHz, anchorHz)
    val bw = wf.w; val bh = wf.h
    val x0 = (lo * bw).toInt().coerceIn(0, bw - 1)
    val x1 = (hi * bw).toInt().coerceIn(x0 + 1, bw)
    val top = wf.top
    val dw = size.width.toInt(); val dh = size.height.toInt()
    if (dw <= 0 || dh <= 0) return
    // the ring: rows top..H-1 are the newest, then 0..top-1
    val upper = ((bh - top).toFloat() / bh * dh).toInt().coerceIn(0, dh)
    if (upper > 0) drawImage(bmp, IntOffset(x0, top), IntSize(x1 - x0, bh - top), IntOffset(0, 0), IntSize(dw, upper), filterQuality = FilterQuality.Low)
    if (top > 0 && dh - upper > 0) drawImage(bmp, IntOffset(x0, 0), IntSize(x1 - x0, top), IntOffset(0, upper), IntSize(dw, dh - upper), filterQuality = FilterQuality.Low)
    if (fr.spanHz > 0) {
        val xd = (((freqMhz * 1e6 - fr.centreHz) / fr.spanHz + 0.5 - lo) / (hi - lo) * size.width).toFloat()
        drawLine(p.accent.copy(alpha = 0.6f), Offset(xd, 0f), Offset(xd, size.height), 1.5f)
    }
    // the time axis
    val total = bh * wf.lineSec
    val step = niceStep(total.toDouble(), 5).toFloat().coerceAtLeast(0.1f)
    var t = 0f
    while (t < total) {
        val y = t / total * size.height
        drawLine(Color(0x55FFFFFF), Offset(0f, y), Offset(10f, y), 1f)
        drawText(measurer, if (t == 0f) "now" else String.format(Locale.US, "%.0f s", t), Offset(12f, (y - 6f).coerceAtLeast(0f)), label)
        t += step
    }
}
