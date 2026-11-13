// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels.walkdf

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.GeomagneticField
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.media.AudioManager
import android.media.ToneGenerator
import android.net.Uri
import android.os.Looper
import android.view.HapticFeedbackConstants
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.CheckRow
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.PositionMap
import io.github.adswill.onair.R
import io.github.adswill.onair.SectionLabel
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.ui.AppPermission
import io.github.adswill.onair.ui.Permissions
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.panels.ModePanel
import io.github.adswill.onair.ui.panels.PanelContext
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * Walk-around direction finding with one SDR: walk with the phone, the panel records the tuned channel's level against GPS position
 * and compass heading, draws the heat map, fits a path-loss model for the transmitter and guides with a hot/cold arrow and a Geiger
 * click rate. Untested on a phone (see docs/PATHWAY.md). The maths is in WalkMath.kt, the state in WalkSession.
 *
 * Privacy: positions stay in memory on the device; the only way out is the export the user starts (system "save as" dialog).
 * Battery: the GPS and the compass are only on while a walk is running (and the panel is shown), the level is sampled twice a second,
 * the model is refitted at most every 3 s on a background thread.
 */
object WalkDfPanel : ModePanel {
    override val id = "walkdf"
    override val order = 90
    @Composable override fun title() = stringResource(R.string.panel_walkdf)
    override fun appliesTo(ctx: PanelContext) = true

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) = WalkDfContent(vm, modifier)
}

@Composable
private fun WalkDfContent(vm: ReceiverViewModel, modifier: Modifier) {
    val p = LocalPalette.current
    val ctx = LocalContext.current
    val view = LocalView.current
    val t by vm.telemetry.collectAsStateWithLifecycle()
    val level = t?.snrDb ?: t?.general?.levelDb
    val levelNow by rememberUpdatedState(level)
    val s = WalkSession
    @Suppress("UNUSED_VARIABLE") val ver = s.version               // re-read the lists on every change
    var msg by remember { mutableStateOf<String?>(null) }
    var hapticOn by remember { mutableStateOf(false) }
    var soundOn by remember { mutableStateOf(false) }
    var denied by remember { mutableStateOf(false) }

    // GPS + compass: only while a walk is running.
    DisposableEffect(s.running) {
        if (!s.running) return@DisposableEffect onDispose { }
        val stop = startSensors(ctx) { msg = it }
        onDispose { stop() }
    }
    // Level sampling: twice a second.
    LaunchedEffect(s.running) {
        while (s.running) {
            levelNow?.let { s.addReading(System.currentTimeMillis(), it) }
            delay(500)
        }
    }
    // Refit at most every 3 s, off the main thread.
    LaunchedEffect(s.running) {
        var last = -1
        while (s.running) {
            val n = s.samples.size
            if (n != last && n >= 8) {
                last = n
                val snap = s.samples
                s.fit = withContext(Dispatchers.Default) { fitPathLoss(snap) }
            }
            delay(3000)
        }
    }
    // Geiger clicks.
    val span = levelSpan(s.samples.map { it.levelDb })
    val geigerLevel = s.samples.lastOrNull()?.levelDb
    val rate = if (geigerLevel != null) geigerRateHz(geigerLevel, span.first, span.second) else 0.5
    val rateNow by rememberUpdatedState(rate)
    LaunchedEffect(s.running, hapticOn, soundOn) {
        if (!s.running || (!hapticOn && !soundOn)) return@LaunchedEffect
        val tone = if (soundOn) try { ToneGenerator(AudioManager.STREAM_MUSIC, 40) } catch (_: Exception) { null } else null
        try {
            val rnd = java.util.Random()
            while (true) {
                delay(nextClickMs(rateNow, rnd.nextDouble()))
                if (hapticOn) view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                tone?.startTone(ToneGenerator.TONE_PROP_BEEP2, 12)
            }
        } finally { tone?.release() }
    }

    val askPerm = io.github.adswill.onair.ui.rememberPermissionRequest(ctx)
    var pendingExport by remember { mutableStateOf("") }
    val csvLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { u -> writeTo(ctx, u, pendingExport) { msg = it } }
    val gpxLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/gpx+xml")) { u -> writeTo(ctx, u, pendingExport) { msg = it } }
    val kmlLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/vnd.google-earth.kml+xml")) { u -> writeTo(ctx, u, pendingExport) { msg = it } }

    val tr = trend(s.samples)
    val fit = s.fit
    val here = s.samples.lastOrNull()
    val head = s.heading

    Column(modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            PanelButton(if (s.running) "Pause" else if (s.samples.isEmpty()) "Start walk" else "Resume", on = s.running, modifier = Modifier.weight(1f)) {
                if (s.running) s.running = false
                else askPerm(listOf(AppPermission.LOCATION)) { r ->
                    val ok = r[AppPermission.LOCATION] != false
                    denied = !ok
                    if (ok) s.running = true else msg = "Location permission is needed to place the readings on the map."
                }
            }
            PanelButton("Clear", modifier = Modifier.weight(1f), enabled = s.samples.isNotEmpty() || s.bearings.isNotEmpty()) { s.clear(); msg = null }
        }
        if (level == null) Label("No level from the receiver: tune a channel and start the radio.", color = p.warn)
        if (denied) Label("Location is off for OnAir: allow it in the phone settings to use walk-around DF.", color = p.bad)
        msg?.let { Label(it, color = p.warn) }

        // Readout.
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Label(if (here != null) "%.1f dB".format(here.levelDb) else "-- dB", color = p.text, size = 20)
            Label("${s.samples.size} s  ${if (s.running) "recording" else "paused"}", color = p.muted)
            Label("GPS ${if (s.lastAcc < 900) "%.0f m".format(s.lastAcc) else "--"}  hdg ${head?.let { "%.0f".format(it) } ?: "--"}", color = p.muted)
        }

        // Heat map.
        val consent = vm.settings.collectAsStateWithLifecycle().value.mapConsent
        val mapMod = Modifier.fillMaxWidth().height(320.dp)
        val start = s.samples.lastOrNull()
        if (consent == 1 && start != null) {
            // OpenStreetMap tiles under the heat map (the same consent as the other maps).
            val pal = p
            val snap = s.samples; val bl = s.bearings.toList(); val fl = s.fit; val la = s.lastLat; val lo = s.lastLon
            PositionMap(emptyList(), null, { }, consent, vm::setMapConsent, mapMod,
                overlay = { toScreen -> drawWalk(pal, snap, fl, bl, la, lo, toScreen) }, homeOverride = start.lat to start.lon, startZoom = 17f)
        } else {
            HeatMap(s.samples, fit, s.bearings, s.lastLat, s.lastLon, mapMod.background(p.plate))
            if (consent != 1) PanelButton("Show OpenStreetMap tiles under the map", modifier = Modifier.fillMaxWidth()) { vm.setMapConsent(1) }
        }

        // Hot / cold, arrow, estimate.
        val trendText = when (tr.trend) {
            Trend.HOT -> "HOT  +%.1f dB / 10 m".format(tr.slopeDbPer10m)
            Trend.COLD -> "COLD  %.1f dB / 10 m".format(tr.slopeDbPer10m)
            Trend.FLAT -> "flat"
            Trend.UNKNOWN -> "keep walking"
        }
        val trendColor = when (tr.trend) { Trend.HOT -> p.bad; Trend.COLD -> p.accent; else -> p.muted }
        Row(Modifier.fillMaxWidth(), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            val rel = if (fit != null && head != null && !s.lastLat.isNaN()) arrowDeg(s.lastLat, s.lastLon, fit.lat, fit.lon, head) else null
            Canvas(Modifier.height(64.dp).fillMaxWidth(0.25f)) {
                val c = Offset(size.width / 2, size.height / 2); val r = min(size.width, size.height) / 2 - 4f
                drawCircle(p.border, r, c, style = Stroke(1.5f))
                if (rel != null) {
                    val a = rel * PI / 180.0
                    val tip = Offset(c.x + (r * sin(a)).toFloat(), c.y - (r * cos(a)).toFloat())
                    drawLine(trendColor.takeIf { tr.trend != Trend.FLAT && tr.trend != Trend.UNKNOWN } ?: p.accent, c, tip, 6f)
                    drawCircle(p.accent, 6f, tip)
                }
            }
            Column {
                Label(trendText, color = trendColor, size = 14)
                Label("click rate %.1f /s".format(rate), color = p.muted)
                if (rel == null && fit != null) Label("arrow needs the compass", color = p.muted)
            }
        }
        if (fit != null) {
            val d = if (!s.lastLat.isNaN()) distanceM(s.lastLat, s.lastLon, fit.lat, fit.lon) else null
            Label("Estimate %.5f, %.5f".format(fit.lat, fit.lon) + (d?.let { "  %.0f m away".format(it) } ?: ""), color = p.text)
            Label("path loss n=%.1f  rms %.1f dB  uncertainty %.0f x %.0f m".format(fit.exponent, fit.rmsDb, fit.ellipse.semiMajorM, fit.ellipse.semiMinorM), color = p.muted)
            if (fit.extrapolated) Label("The signal source looks outside the walked area: treat this as a direction and walk that way.", color = p.warn)
        } else {
            Label("The estimate appears after 8 s of walking over at least 10 m.", color = p.muted)
        }

        // Directional antenna.
        SectionLabel("Directional antenna")
        Label("Hold the antenna in line with the top of the phone. Mark when the signal peaks, or sweep a full turn in place.", color = p.muted)
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            PanelButton("Mark heading", modifier = Modifier.weight(1f), enabled = s.running) {
                msg = if (s.markAntenna()) "Marked ${s.heading?.roundToInt()} degrees." else "No compass or GPS fix yet."
            }
            PanelButton(if (s.sweeping) "End sweep (${s.sweepCount})" else "Sweep", on = s.sweeping, modifier = Modifier.weight(1f), enabled = s.running) {
                if (!s.sweeping) s.startSweep()
                else msg = s.endSweep()?.let { "Bearing %.0f deg, beam about %.0f deg, peak %.0f dB above the rest.".format(it.bearingDeg, it.beamWidthDeg, it.confidenceDb) }
                    ?: "Turn through at least three quarters of a full circle."
            }
        }
        val cross = intersectBearings(s.bearings)
        if (cross != null) Label("Bearing lines cross at %.5f, %.5f (%d lines)".format(cross.first, cross.second, s.bearings.size), color = p.text)
        else if (s.bearings.isNotEmpty()) Label("${s.bearings.size} bearing line(s). Two from different places give a position.", color = p.muted)

        // Fox hunting.
        SectionLabel("Fox hunting")
        CheckRow("Click with vibration", hapticOn) { hapticOn = it }
        CheckRow("Click with sound", soundOn) { soundOn = it }

        // Export.
        SectionLabel("Export")
        Label("Saved where you choose; nothing is uploaded.", color = p.muted)
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            val have = s.samples.isNotEmpty()
            PanelButton("CSV", modifier = Modifier.weight(1f), enabled = have) { pendingExport = toCsv(s.samples); csvLauncher.launch("onair-walk.csv") }
            PanelButton("GPX", modifier = Modifier.weight(1f), enabled = have) { pendingExport = toGpx(s.samples, s.fit); gpxLauncher.launch("onair-walk.gpx") }
            PanelButton("KML", modifier = Modifier.weight(1f), enabled = have) { pendingExport = toKml(s.samples, s.fit); kmlLauncher.launch("onair-walk.kml") }
        }
        Label("Level is the receiver's SNR of the tuned channel (or the signal level when a mode has no SNR), averaged per second.", color = p.muted)
    }
}

private fun writeTo(ctx: Context, uri: Uri?, text: String, report: (String) -> Unit) {
    if (uri == null) return
    try {
        ctx.contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray(Charsets.UTF_8)) }
        report("Saved.")
    } catch (e: Exception) { AppLog.w("walkdf", "export failed: ${e.message}"); report("Could not save the file.") }
}

/** GPS (1 s) and compass (rotation vector) listeners; returns the function that stops them. */
@SuppressLint("MissingPermission")
private fun startSensors(ctx: Context, report: (String) -> Unit): () -> Unit {
    val s = WalkSession
    val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager
    val sm = ctx.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    var declination = 0f
    val ll = LocationListener { l: Location ->
        s.lastLat = l.latitude; s.lastLon = l.longitude
        s.lastAcc = if (l.hasAccuracy()) l.accuracy.toDouble() else 30.0
        s.lastFixMs = System.currentTimeMillis()
        declination = GeomagneticField(l.latitude.toFloat(), l.longitude.toFloat(), 0f, s.lastFixMs).declination
    }
    try {
        if (!Permissions.granted(ctx, AppPermission.LOCATION)) throw SecurityException("no location permission")
        lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, ll, Looper.getMainLooper())
    } catch (e: Exception) { report("GPS unavailable: ${e.message}"); AppLog.w("walkdf", "GPS: ${e.message}") }
    val rot = FloatArray(9); val ori = FloatArray(3)
    val sl = object : SensorEventListener {
        override fun onSensorChanged(e: SensorEvent) {
            SensorManager.getRotationMatrixFromVector(rot, e.values)
            SensorManager.getOrientation(rot, ori)
            s.heading = wrap360(Math.toDegrees(ori[0].toDouble()) + declination)       // portrait, phone top = heading
        }
        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
    }
    val sensor = sm.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
    if (sensor != null) sm.registerListener(sl, sensor, SensorManager.SENSOR_DELAY_UI) else report("No compass in this phone: heading, arrow and sweeps are off.")
    return { try { lm.removeUpdates(ll) } catch (_: Exception) {}; sm.unregisterListener(sl) }
}

/** The heat map on its own plate (no tiles): north up, scaled to the walk. */
@Composable
private fun HeatMap(samples: List<Sample>, fit: FitResult?, bearings: List<BearingLine>, curLat: Double, curLon: Double, modifier: Modifier) {
    val pal = LocalPalette.current
    Canvas(modifier) {
        if (samples.isEmpty()) return@Canvas
        val lat0 = samples.sumOf { it.lat } / samples.size
        val lon0 = samples.sumOf { it.lon } / samples.size
        val pts = samples.map { toLocal(it.lat, it.lon, lat0, lon0) }
        val est = fit?.let { toLocal(it.lat, it.lon, lat0, lon0) }
        var x0 = pts.minOf { it.x }; var x1 = pts.maxOf { it.x }; var y0 = pts.minOf { it.y }; var y1 = pts.maxOf { it.y }
        if (est != null && fit != null) {
            val r = fit.ellipse.semiMajorM.coerceAtMost(300.0)
            x0 = min(x0, est.x - r); x1 = max(x1, est.x + r); y0 = min(y0, est.y - r); y1 = max(y1, est.y + r)
        }
        val w = max(40.0, x1 - x0); val h = max(40.0, y1 - y0)
        val sc = (min(size.width / (w * 1.15), size.height / (h * 1.15))).toFloat()
        val cx = (x0 + x1) / 2; val cy = (y0 + y1) / 2
        val toScreen = { lat: Double, lon: Double ->
            val q = toLocal(lat, lon, lat0, lon0)
            Offset(size.width / 2 + ((q.x - cx) * sc).toFloat(), size.height / 2 - ((q.y - cy) * sc).toFloat())
        }
        drawWalk(pal, samples, fit, bearings, curLat, curLon, toScreen)
        val target = size.width / 5f / sc
        val bar = listOf(5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000).lastOrNull { it <= target } ?: 5
        drawLine(pal.plotText, Offset(12f, size.height - 14f), Offset(12f + bar * sc, size.height - 14f), 3f)
    }
}

/** Draws the walk on any lat/lon -> screen mapping (own plate or the OSM tile map): rays, track, heat dots, position, estimate + ellipse. */
private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawWalk(
    p: io.github.adswill.onair.Palette, samples: List<Sample>, fit: FitResult?, bearings: List<BearingLine>, curLat: Double, curLon: Double,
    toScreen: (Double, Double) -> Offset,
) {
    if (samples.isEmpty()) return
    val (lo, hi) = levelSpan(samples.map { it.levelDb })
    // pixels per metre near the walk
    val a = samples.last()
    val o0 = toScreen(a.lat, a.lon)
    val (la1, lo1) = fromLocal(0.0, 100.0, a.lat, a.lon)
    val ppm = ((toScreen(la1, lo1) - o0).getDistance() / 100f).coerceAtLeast(1e-4f)
    for (b in bearings) {
        val s0 = toScreen(b.lat, b.lon)
        val t = b.bearingDeg * PI / 180.0
        val (la2, lo2) = fromLocal(2000.0 * sin(t), 2000.0 * cos(t), b.lat, b.lon)
        drawLine(p.warn.copy(alpha = 0.8f), s0, toScreen(la2, lo2), 3f)
    }
    for (i in 1 until samples.size) drawLine(p.grid, toScreen(samples[i - 1].lat, samples[i - 1].lon), toScreen(samples[i].lat, samples[i].lon), 2f)
    for (s in samples) {
        val f = if (hi > lo) (s.levelDb - lo) / (hi - lo) else 0.5
        drawCircle(Color(heatArgb(f)), 8f, toScreen(s.lat, s.lon))
    }
    if (!curLat.isNaN()) drawCircle(p.bright, 11f, toScreen(curLat, curLon), style = Stroke(3f))
    if (fit != null) {
        val c = toScreen(fit.lat, fit.lon)
        val rw = (fit.ellipse.semiMajorM * ppm).toFloat(); val rh = (fit.ellipse.semiMinorM * ppm).toFloat()
        rotate(degrees = (fit.ellipse.majorAzimuthDeg - 90.0).toFloat(), pivot = c) {
            drawOval(p.accent.copy(alpha = 0.2f), Offset(c.x - rw, c.y - rh), Size(2 * rw, 2 * rh))
            drawOval(p.accent, Offset(c.x - rw, c.y - rh), Size(2 * rw, 2 * rh), style = Stroke(2f))
        }
        drawLine(p.accent, Offset(c.x - 14f, c.y), Offset(c.x + 14f, c.y), 4f)
        drawLine(p.accent, Offset(c.x, c.y - 14f), Offset(c.x, c.y + 14f), 4f)
    }
}
