// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels.walkdf

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/*
 * Walk-around direction finding: the pure maths (no Android classes, so JVM unit tests run it). Everything is on the device; nothing
 * here touches the network.
 *
 *  - [perSecond]   raw level+GPS+heading readings -> one averaged [Sample] per second (bad GPS fixes dropped).
 *  - [fitPathLoss] samples -> transmitter estimate: level = A - 10 n log10(d), unknown position, A and n. For a given position A and n
 *                  are a linear least-squares problem, so the position is found by a grid search plus a pattern-search refinement.
 *                  The uncertainty ellipse is the region whose squared error is within the 95 % (2 degrees of freedom) bound.
 *  - [trend]       hot / cold from the slope of level against the distance walked, with a significance test.
 *  - [bearingFromSweep] rotate-in-place sweep with a directional antenna -> bearing, beam width, confidence.
 *  - [intersectBearings] several bearing lines -> one point.
 *  - [geigerRateHz], [nextClickMs] fox-hunting click rate.
 *  - [toCsv], [toGpx], [toKml] exports.
 */

private const val EARTH_M = 6371000.0
private fun rad(d: Double) = d * PI / 180.0
private fun deg(r: Double) = r * 180.0 / PI

/** Wraps an angle in degrees to [0, 360). */
fun wrap360(a: Double): Double { val r = a % 360.0; return if (r < 0) r + 360.0 else r }

/** Wraps an angle in degrees to (-180, 180]. */
fun wrap180(a: Double): Double { val r = wrap360(a); return if (r > 180.0) r - 360.0 else r }

/** East/north offsets in metres of a point from an origin (equirectangular: exact enough for a walk of a few km). */
data class XY(val x: Double, val y: Double)

fun toLocal(lat: Double, lon: Double, lat0: Double, lon0: Double): XY =
    XY(rad(lon - lon0) * cos(rad(lat0)) * EARTH_M, rad(lat - lat0) * EARTH_M)

fun fromLocal(x: Double, y: Double, lat0: Double, lon0: Double): Pair<Double, Double> =
    (lat0 + deg(y / EARTH_M)) to (lon0 + deg(x / (EARTH_M * cos(rad(lat0)))))

/** Initial bearing in degrees from north (clockwise) from point 1 to point 2, and the distance in metres. */
fun bearingDeg(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
    val d = toLocal(lat2, lon2, lat1, lon1)
    return wrap360(deg(atan2(d.x, d.y)))
}

fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
    val d = toLocal(lat2, lon2, lat1, lon1)
    return hypot(d.x, d.y)
}

/** One raw reading: the tuned channel's level (dB: SNR or RSSI), the GPS fix and the compass heading (null when unknown). */
data class Raw(val tMs: Long, val lat: Double, val lon: Double, val accM: Double, val headingDeg: Double?, val levelDb: Double)

/** One second of the walk. */
data class Sample(val tMs: Long, val lat: Double, val lon: Double, val headingDeg: Double?, val levelDb: Double, val n: Int)

/** Averages the readings per second (level: mean in dB; position: mean; heading: circular mean). Fixes worse than [maxAccM] are dropped. */
fun perSecond(raw: List<Raw>, maxAccM: Double = 50.0): List<Sample> {
    val out = ArrayList<Sample>()
    val good = raw.filter { it.accM <= maxAccM && it.levelDb.isFinite() }
    var i = 0
    while (i < good.size) {
        val sec = Math.floorDiv(good[i].tMs, 1000L)
        var j = i
        while (j < good.size && Math.floorDiv(good[j].tMs, 1000L) == sec) j++
        val g = good.subList(i, j)
        var sx = 0.0; var sy = 0.0; var hc = 0.0; var hs = 0.0; var hn = 0
        for (r in g) { r.headingDeg?.let { hc += cos(rad(it)); hs += sin(rad(it)); hn++ } }
        for (r in g) { sx += r.lat; sy += r.lon }
        val head = if (hn > 0 && hypot(hc, hs) > 1e-6) wrap360(deg(atan2(hs, hc))) else null
        out.add(Sample(sec * 1000L + 500L, sx / g.size, sy / g.size, head, g.sumOf { it.levelDb } / g.size, g.size))
        i = j
    }
    return out
}

/** Moving median of the levels (window odd), to take out single-sample spikes. */
fun medianSmooth(levels: List<Double>, window: Int = 3): List<Double> {
    if (window <= 1 || levels.size < 3) return levels
    val h = window / 2
    return levels.indices.map { i ->
        val s = levels.subList(max(0, i - h), min(levels.size, i + h + 1)).sorted()
        s[s.size / 2]
    }
}

/** An uncertainty ellipse: semi-axes in metres, [majorAzimuthDeg] = azimuth of the major axis from north, clockwise (0..180). */
data class Ellipse(val semiMajorM: Double, val semiMinorM: Double, val majorAzimuthDeg: Double)

data class FitResult(
    val lat: Double, val lon: Double,
    /** Level at 1 m (dB) and the path-loss exponent. */
    val offsetDb: Double, val exponent: Double,
    /** RMS residual of the fit in dB. */
    val rmsDb: Double,
    val ellipse: Ellipse,
    val samples: Int,
    /** True when the transmitter lies outside the walked area by more than 3 times its size (the estimate is a direction more than a place). */
    val extrapolated: Boolean,
)

private const val DMIN_M = 5.0
private const val N_MIN = 0.8
private const val N_MAX = 6.0

private class Lsq(val sse: Double, val a: Double, val n: Double)

private fun lsqAt(xs: DoubleArray, ys: DoubleArray, lv: DoubleArray, px: Double, py: Double, ls: DoubleArray): Lsq {
    val m = xs.size
    var sl = 0.0; var sy = 0.0
    for (i in 0 until m) { ls[i] = -10.0 * log10(max(DMIN_M, hypot(xs[i] - px, ys[i] - py))); sl += ls[i]; sy += lv[i] }
    val ml = sl / m; val my = sy / m
    var cov = 0.0; var vr = 0.0
    for (i in 0 until m) { cov += (ls[i] - ml) * (lv[i] - my); vr += (ls[i] - ml) * (ls[i] - ml) }
    var n = if (vr < 1e-9) 2.0 else cov / vr
    n = min(N_MAX, max(N_MIN, n))
    val a = my - n * ml
    var sse = 0.0
    for (i in 0 until m) { val r = lv[i] - (a + n * ls[i]); sse += r * r }
    return Lsq(sse, a, n)
}

/**
 * Fits level = A - 10 n log10(distance) with unknown transmitter position, A and n. Returns null with fewer than [minSamples] samples
 * or when the walk is shorter than ~10 m (no geometry to fit).
 */
fun fitPathLoss(all: List<Sample>, minSamples: Int = 8, maxSamples: Int = 240): FitResult? {
    if (all.size < minSamples) return null
    // Long walks are thinned evenly so a refit stays cheap on a phone.
    val samples = if (all.size <= maxSamples) all else List(maxSamples) { all[(it.toLong() * (all.size - 1) / (maxSamples - 1)).toInt()] }
    val lat0 = samples.sumOf { it.lat } / samples.size
    val lon0 = samples.sumOf { it.lon } / samples.size
    val xs = DoubleArray(samples.size); val ys = DoubleArray(samples.size); val lv = DoubleArray(samples.size)
    for ((i, s) in samples.withIndex()) { val p = toLocal(s.lat, s.lon, lat0, lon0); xs[i] = p.x; ys[i] = p.y; lv[i] = s.levelDb }
    val minX = xs.min(); val maxX = xs.max(); val minY = ys.min(); val maxY = ys.max()
    val span = hypot(maxX - minX, maxY - minY)
    if (span < 10.0) return null
    val ls = DoubleArray(xs.size)

    // Search area: the walked box grown by 3 spans (at least 300 m) on every side.
    val grow = max(3.0 * span, 300.0)
    val x0 = minX - grow; val x1 = maxX + grow; val y0 = minY - grow; val y1 = maxY + grow
    val g = 61
    val dx = (x1 - x0) / (g - 1); val dy = (y1 - y0) / (g - 1)
    var best = Double.MAX_VALUE; var bi = 0; var bj = 0
    for (j in 0 until g) for (i in 0 until g) {
        val r = lsqAt(xs, ys, lv, x0 + i * dx, y0 + j * dy, ls)
                if (r.sse < best) { best = r.sse; bi = i; bj = j }
    }
    // Pattern search refinement from the best cell.
    var px = x0 + bi * dx; var py = y0 + bj * dy
    var cur = best
    var step = max(dx, dy)
    while (step > 0.25) {
        var moved = false
        for ((ox, oy) in listOf(1.0 to 0.0, -1.0 to 0.0, 0.0 to 1.0, 0.0 to -1.0, 1.0 to 1.0, -1.0 to -1.0, 1.0 to -1.0, -1.0 to 1.0)) {
            val r = lsqAt(xs, ys, lv, px + ox * step, py + oy * step, ls)
            if (r.sse < cur - 1e-12) { cur = r.sse; px += ox * step; py += oy * step; moved = true; break }
        }
        if (!moved) step /= 2.0
    }
    val fin = lsqAt(xs, ys, lv, px, py, ls)
    val m = samples.size
    val sigma2 = max(1.0, fin.sse / max(1, m - 4))
    val bound = fin.sse + 6.17 * sigma2     // 95 % region for 2 free parameters
    // The 95 % region is found on a fine grid centred on the optimum, whose size adapts: doubled while the region touches the edge,
    // halved while it covers fewer than 20 cells (the coarse grid above is too coarse for a well-determined fit).
    val gf = 41
    var half = max(dx, dy) * 2.0
    var e = Ellipse(1.0, 1.0, 0.0)
    for (iter in 0 until 14) {
        val cell = 2.0 * half / (gf - 1)
        var cnt = 0; var touches = false
        var mx = 0.0; var my = 0.0
        val inside = BooleanArray(gf * gf)
        for (j in 0 until gf) for (i in 0 until gf) {
            val x = px - half + i * cell; val y = py - half + j * cell
            if (lsqAt(xs, ys, lv, x, y, ls).sse <= bound) {
                inside[j * gf + i] = true; cnt++; mx += x; my += y
                if (i == 0 || j == 0 || i == gf - 1 || j == gf - 1) touches = true
            }
        }
        if (touches && half < 20.0 * grow) { half *= 2.0; continue }
        if (cnt < 20 && half > 0.5 && iter < 13) { half /= 2.0; continue }
        if (cnt == 0) { e = Ellipse(cell / 2.0, cell / 2.0, 0.0); break }
        mx /= cnt; my /= cnt
        var cxx = 0.0; var cyy = 0.0; var cxy = 0.0
        for (j in 0 until gf) for (i in 0 until gf) if (inside[j * gf + i]) {
            val ex = px - half + i * cell - mx; val ey = py - half + j * cell - my
            cxx += ex * ex; cyy += ey * ey; cxy += ex * ey
        }
        cxx = cxx / cnt + cell * cell / 12.0; cyy = cyy / cnt + cell * cell / 12.0; cxy /= cnt
        val tr = cxx + cyy; val det = cxx * cyy - cxy * cxy
        val disc = sqrt(max(0.0, tr * tr / 4.0 - det))
        val l1 = tr / 2.0 + disc; val l2 = max(0.0, tr / 2.0 - disc)
        val ang = 0.5 * atan2(2.0 * cxy, cxx - cyy)       // from +x (east), counter-clockwise
        e = Ellipse(2.0 * sqrt(l1), 2.0 * sqrt(l2), wrap360(90.0 - deg(ang)) % 180.0)
        break
    }
    val (lat, lon) = fromLocal(px, py, lat0, lon0)
    val outside = max(max(minX - px, px - maxX), max(minY - py, py - maxY))
    return FitResult(lat, lon, fin.a, fin.n, sqrt(fin.sse / m), e, m, outside > 3.0 * span)
}

enum class Trend { HOT, COLD, FLAT, UNKNOWN }

/** [slopeDbPer10m]: dB per 10 m walked. */
data class TrendResult(val trend: Trend, val slopeDbPer10m: Double)

/** Hot / cold from the last [window] samples: slope of the level against the distance walked, "hot" or "cold" only when significant. */
fun trend(samples: List<Sample>, window: Int = 12, minWalkM: Double = 5.0, tStat: Double = 1.5): TrendResult {
    val s = samples.takeLast(window)
    if (s.size < 4) return TrendResult(Trend.UNKNOWN, 0.0)
    val d = DoubleArray(s.size)
    for (i in 1 until s.size) d[i] = d[i - 1] + distanceM(s[i - 1].lat, s[i - 1].lon, s[i].lat, s[i].lon)
    if (d.last() < minWalkM) return TrendResult(Trend.UNKNOWN, 0.0)
    val lv = DoubleArray(s.size) { s[it].levelDb }
    val md = d.average(); val ml = lv.average()
    var sxx = 0.0; var sxy = 0.0
    for (i in s.indices) { sxx += (d[i] - md) * (d[i] - md); sxy += (d[i] - md) * (lv[i] - ml) }
    if (sxx < 1e-9) return TrendResult(Trend.UNKNOWN, 0.0)
    val slope = sxy / sxx
    var sse = 0.0
    for (i in s.indices) { val r = lv[i] - (ml + slope * (d[i] - md)); sse += r * r }
    val se = sqrt(max(sse, 1e-9) / (s.size - 2) / sxx)
    val t = abs(slope) / se
    val tr = if (t < tStat) Trend.FLAT else if (slope > 0) Trend.HOT else Trend.COLD
    return TrendResult(tr, slope * 10.0)
}

/** Angle in (-180, 180] to turn the phone's arrow: bearing to the target relative to the compass heading. */
fun arrowDeg(fromLat: Double, fromLon: Double, toLat: Double, toLon: Double, headingDeg: Double): Double =
    wrap180(bearingDeg(fromLat, fromLon, toLat, toLon) - headingDeg)

data class BearingResult(val bearingDeg: Double, val beamWidthDeg: Double, val confidenceDb: Double, val coverage: Double)

/**
 * A rotate-in-place sweep: (heading deg, level dB) pairs while the user turns a directional antenna through a full circle.
 * Bins of 10 degrees, averaged; the bearing is the power-weighted centre of the bins within 3 dB of the peak and 60 degrees of it.
 * Returns null unless at least 75 % of the circle was covered. [confidenceDb] = peak minus the median bin (how clear the peak is).
 */
fun bearingFromSweep(points: List<Pair<Double, Double>>, binDeg: Int = 10, minCoverage: Double = 0.75): BearingResult? {
    val nb = 360 / binDeg
    val sum = DoubleArray(nb); val cnt = IntArray(nb)
    for ((h, l) in points) { if (!l.isFinite()) continue; val b = (wrap360(h) / binDeg).toInt() % nb; sum[b] += l; cnt[b]++ }
    val have = cnt.count { it > 0 }
    val coverage = have.toDouble() / nb
    if (coverage < minCoverage) return null
    val raw = DoubleArray(nb) { if (cnt[it] > 0) sum[it] / cnt[it] else Double.NaN }
    // Triangular smoothing over neighbouring bins that have data.
    val v = DoubleArray(nb) { i ->
        if (raw[i].isNaN()) Double.NaN else {
            var a = 2.0 * raw[i]; var w = 2.0
            for (o in intArrayOf(-1, 1)) { val r = raw[(i + o + nb) % nb]; if (!r.isNaN()) { a += r; w += 1.0 } }
            a / w
        }
    }
    var pk = -1
    for (i in 0 until nb) if (!v[i].isNaN() && (pk < 0 || v[i] > v[pk])) pk = i
    val peak = v[pk]
    val med = v.filter { !it.isNaN() }.sorted().let { it[it.size / 2] }
    var sx = 0.0; var sy = 0.0
    var lo = 0; var hi = 0
    for (o in 1..nb / 2) { val b = (pk - o + nb) % nb; if (v[b].isNaN() || v[b] < peak - 3.0) break; lo = o }
    for (o in 1..nb / 2) { val b = (pk + o) % nb; if (v[b].isNaN() || v[b] < peak - 3.0) break; hi = o }
    val reach = (60 / binDeg)
    for (o in -min(lo, reach)..min(hi, reach)) {
        val b = (pk + o + nb) % nb
        val w = 10.0.pow((v[b] - peak) / 10.0)
        val c = (b + 0.5) * binDeg
        sx += w * sin(rad(c)); sy += w * cos(rad(c))
    }
    val bearing = wrap360(deg(atan2(sx, sy)))
    return BearingResult(bearing, ((lo + hi + 1) * binDeg).toDouble(), peak - med, coverage)
}

/** A bearing observation: a point and the compass bearing (degrees from north) the signal came from. */
data class BearingLine(val lat: Double, val lon: Double, val bearingDeg: Double)

/** Least-squares point closest to all the rays; null for fewer than two lines or (nearly) parallel lines. Result lat/lon. */
fun intersectBearings(lines: List<BearingLine>): Pair<Double, Double>? {
    if (lines.size < 2) return null
    val lat0 = lines.sumOf { it.lat } / lines.size; val lon0 = lines.sumOf { it.lon } / lines.size
    var a11 = 0.0; var a12 = 0.0; var a22 = 0.0; var b1 = 0.0; var b2 = 0.0
    for (l in lines) {
        val p = toLocal(l.lat, l.lon, lat0, lon0)
        val nx = cos(rad(l.bearingDeg)); val ny = -sin(rad(l.bearingDeg))     // unit normal of the ray (ray dir = sin, cos)
        a11 += nx * nx; a12 += nx * ny; a22 += ny * ny
        val dot = nx * p.x + ny * p.y
        b1 += nx * dot; b2 += ny * dot
    }
    val det = a11 * a22 - a12 * a12
    if (det < 0.02 * lines.size * lines.size / 4.0) return null
    val x = (a22 * b1 - a12 * b2) / det; val y = (a11 * b2 - a12 * b1) / det
    return fromLocal(x, y, lat0, lon0)
}

/** Geiger-style click rate in Hz: level between [loDb] (quiet) and [hiDb] (loud), 0.5 Hz up to 25 Hz, quadratic so the top end speeds up. */
fun geigerRateHz(levelDb: Double, loDb: Double, hiDb: Double): Double {
    val f = if (hiDb - loDb < 1e-6) 0.0 else ((levelDb - loDb) / (hiDb - loDb)).coerceIn(0.0, 1.0)
    return 0.5 + 24.5 * f * f
}

/** Milliseconds to the next click for a Poisson process of [rateHz] (u uniform in [0,1)). */
fun nextClickMs(rateHz: Double, u: Double): Long = (-ln(1.0 - u.coerceIn(0.0, 0.999999)) / max(0.1, rateHz) * 1000.0).toLong().coerceAtLeast(15L)

/** 5th and 95th percentile of the levels, the span the heat map colours and the Geiger rate use. */
fun levelSpan(levels: List<Double>): Pair<Double, Double> {
    if (levels.isEmpty()) return -30.0 to 10.0
    val s = levels.sorted()
    val lo = s[((s.size - 1) * 0.05).toInt()]; val hi = s[((s.size - 1) * 0.95).toInt()]
    return if (hi - lo < 3.0) (lo - 1.5) to (lo + 1.5) else lo to hi
}

/** Heat colour (opaque ARGB int) for a fraction 0..1: blue, cyan, green, yellow, red. */
fun heatArgb(frac: Double): Int {
    val f = frac.coerceIn(0.0, 1.0)
    val stops = arrayOf(doubleArrayOf(0.0, 40.0, 90.0, 220.0), doubleArrayOf(0.25, 40.0, 190.0, 210.0), doubleArrayOf(0.5, 90.0, 200.0, 90.0),
        doubleArrayOf(0.75, 235.0, 205.0, 60.0), doubleArrayOf(1.0, 225.0, 60.0, 50.0))
    var k = 0
    while (k < stops.size - 2 && f > stops[k + 1][0]) k++
    val a = stops[k]; val b = stops[k + 1]
    val t = (f - a[0]) / (b[0] - a[0])
    fun c(i: Int) = (a[i] + (b[i] - a[i]) * t).toInt().coerceIn(0, 255)
    return (0xFF shl 24) or (c(1) shl 16) or (c(2) shl 8) or c(3)
}

// ---- Exports -----------------------------------------------------------------------------------------------------------------------

private fun iso(ms: Long): String = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(ms))
private fun f(v: Double, d: Int) = String.format(Locale.US, "%.${d}f", v)
private fun esc(s: String) = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;")

/** CSV: time_utc,lat,lon,heading_deg,level_db,readings (one row per second). */
fun toCsv(samples: List<Sample>): String {
    val sb = StringBuilder("time_utc,lat,lon,heading_deg,level_db,readings\n")
    for (s in samples) sb.append(iso(s.tMs)).append(',').append(f(s.lat, 7)).append(',').append(f(s.lon, 7)).append(',')
        .append(s.headingDeg?.let { f(it, 1) } ?: "").append(',').append(f(s.levelDb, 2)).append(',').append(s.n).append('\n')
    return sb.toString()
}

/** GPX 1.1: one track with the level as the point's `<extensions>`/`<name>`, the estimate (if any) as a waypoint. */
fun toGpx(samples: List<Sample>, est: FitResult?, name: String = "OnAir walk"): String {
    val sb = StringBuilder("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n")
    if (est != null) sb.append("  <wpt lat=\"").append(f(est.lat, 7)).append("\" lon=\"").append(f(est.lon, 7))
        .append("\"><name>Estimated transmitter</name><desc>").append(esc("n=${f(est.exponent, 2)} rms=${f(est.rmsDb, 1)} dB, ellipse ${f(est.ellipse.semiMajorM, 0)} x ${f(est.ellipse.semiMinorM, 0)} m"))
        .append("</desc></wpt>\n")
    sb.append("  <trk><name>").append(esc(name)).append("</name><trkseg>\n")
    for (s in samples) sb.append("    <trkpt lat=\"").append(f(s.lat, 7)).append("\" lon=\"").append(f(s.lon, 7)).append("\"><time>").append(iso(s.tMs))
        .append("</time><name>").append(f(s.levelDb, 1)).append(" dB</name></trkpt>\n")
    sb.append("  </trkseg></trk>\n</gpx>\n")
    return sb.toString()
}

/** KML: the track as a line, one coloured point per second (heat colour of the level), the estimate as a placemark. */
fun toKml(samples: List<Sample>, est: FitResult?, name: String = "OnAir walk"): String {
    val (lo, hi) = levelSpan(samples.map { it.levelDb })
    val sb = StringBuilder("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><name>").append(esc(name)).append("</name>\n")
    if (samples.size > 1) {
        sb.append("<Placemark><name>Track</name><Style><LineStyle><color>ccffffff</color><width>2</width></LineStyle></Style><LineString><coordinates>")
        for (s in samples) sb.append(f(s.lon, 7)).append(',').append(f(s.lat, 7)).append(",0 ")
        sb.append("</coordinates></LineString></Placemark>\n")
    }
    for (s in samples) {
        val c = heatArgb(if (hi > lo) (s.levelDb - lo) / (hi - lo) else 0.5)
        val kmlColor = String.format(Locale.US, "ff%02x%02x%02x", c and 0xFF, (c shr 8) and 0xFF, (c shr 16) and 0xFF)   // aabbggrr
        sb.append("<Placemark><name>").append(f(s.levelDb, 1)).append(" dB</name><TimeStamp><when>").append(iso(s.tMs)).append("</when></TimeStamp>")
            .append("<Style><IconStyle><color>").append(kmlColor).append("</color><scale>0.6</scale><Icon><href>http://maps.google.com/mapfiles/kml/shapes/shaded_dot.png</href></Icon></IconStyle></Style>")
            .append("<Point><coordinates>").append(f(s.lon, 7)).append(',').append(f(s.lat, 7)).append(",0</coordinates></Point></Placemark>\n")
    }
    if (est != null) sb.append("<Placemark><name>Estimated transmitter</name><Point><coordinates>").append(f(est.lon, 7)).append(',').append(f(est.lat, 7))
        .append(",0</coordinates></Point></Placemark>\n")
    sb.append("</Document></kml>\n")
    return sb.toString()
}
