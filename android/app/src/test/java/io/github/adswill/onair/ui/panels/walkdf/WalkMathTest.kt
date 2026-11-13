// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels.walkdf

import java.util.Random
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WalkMathTest {
    private val lat0 = 54.7000
    private val lon0 = 25.3000

    private fun level(x: Double, y: Double, tx: XY, a: Double, n: Double, rng: Random, sigma: Double): Double =
        a - 10.0 * n * log10(max(5.0, hypot(x - tx.x, y - tx.y))) + rng.nextGaussian() * sigma

    /** A lawn-mower walk over [size] m, 1.4 m/s, 4 readings per second, GPS jitter [gps] m. */
    private fun walk(tx: XY, a: Double, n: Double, sigma: Double, size: Double = 160.0, seed: Long = 1, gps: Double = 3.0): List<Raw> {
        val rng = Random(seed)
        val pts = ArrayList<XY>()
        var y = 0.0; var dir = 1
        while (y <= size) { pts.add(XY(if (dir > 0) 0.0 else size, y)); pts.add(XY(if (dir > 0) size else 0.0, y)); y += 40.0; dir = -dir }
        val out = ArrayList<Raw>()
        var t = 1_000_000L
        for (k in 0 until pts.size - 1) {
            val p = pts[k]; val q = pts[k + 1]
            val len = hypot(q.x - p.x, q.y - p.y)
            val steps = (len / 1.4 * 4).toInt()
            for (i in 0 until steps) {
                val f = i.toDouble() / steps
                val x = p.x + (q.x - p.x) * f; val yy = p.y + (q.y - p.y) * f
                val (la, lo) = fromLocal(x + rng.nextGaussian() * gps, yy + rng.nextGaussian() * gps, lat0, lon0)
                out.add(Raw(t, la, lo, 5.0, null, level(x, yy, tx, a, n, rng, sigma)))
                t += 250
            }
        }
        return out
    }

    private fun err(f: FitResult, tx: XY): Double { val p = toLocal(f.lat, f.lon, lat0, lon0); return hypot(p.x - tx.x, p.y - tx.y) }

    @Test fun projectionRoundTrip() {
        val p = toLocal(54.7123, 25.3456, lat0, lon0)
        val (la, lo) = fromLocal(p.x, p.y, lat0, lon0)
        assertEquals(54.7123, la, 1e-9); assertEquals(25.3456, lo, 1e-9)
        assertEquals(0.0, bearingDeg(lat0, lon0, lat0 + 0.01, lon0), 1e-6)
        assertEquals(90.0, bearingDeg(lat0, lon0, lat0, lon0 + 0.01), 1e-6)
    }

    @Test fun perSecondAveragesAndDropsBadFixes() {
        val raw = listOf(
            Raw(1000, 54.0, 25.0, 5.0, 350.0, -10.0), Raw(1500, 54.0, 25.0, 5.0, 10.0, -20.0),
            Raw(1700, 55.0, 26.0, 200.0, 0.0, 99.0), Raw(2100, 54.0, 25.0, 5.0, null, -30.0),
        )
        val s = perSecond(raw)
        assertEquals(2, s.size)
        assertEquals(-15.0, s[0].levelDb, 1e-9); assertEquals(2, s[0].n)
        assertEquals(0.0, wrap180(s[0].headingDeg!!), 1e-6)     // circular mean of 350 and 10
        assertEquals(-30.0, s[1].levelDb, 1e-9); assertNull(s[1].headingDeg)
    }

    @Test fun medianRemovesSpike() {
        val m = medianSmooth(listOf(1.0, 1.0, 50.0, 1.0, 1.0))
        assertEquals(1.0, m[2], 0.0)
    }

    @Test fun fitFindsTransmitterInsideTheArea() {
        val tx = XY(110.0, 90.0)
        val f = fitPathLoss(perSecond(walk(tx, -20.0, 2.8, 2.0)))!!
        assertTrue("error ${err(f, tx)}", err(f, tx) < 25.0)
        assertEquals(2.8, f.exponent, 0.8)
        assertEquals(2.0, f.rmsDb, 1.0)
        assertFalse(f.extrapolated)
        assertTrue(f.ellipse.semiMajorM >= f.ellipse.semiMinorM)
    }

    @Test fun fitOutsideTheWalkedArea() {
        val tx = XY(-220.0, 150.0)
        val f = fitPathLoss(perSecond(walk(tx, -15.0, 2.2, 1.5, seed = 7)))!!
        assertTrue("error ${err(f, tx)}", err(f, tx) < 90.0)
    }

    @Test fun ellipseGrowsWithNoiseAndContainsTruthMostly() {
        var inside = 0; val runs = 12
        var quiet = 0.0; var noisy = 0.0
        for (seed in 1..runs) {
            val tx = XY(80.0 + seed * 3, 60.0)
            val fq = fitPathLoss(perSecond(walk(tx, -20.0, 3.0, 1.0, seed = seed.toLong())))!!
            val fn = fitPathLoss(perSecond(walk(tx, -20.0, 3.0, 6.0, seed = seed.toLong())))!!
            quiet += fq.ellipse.semiMajorM; noisy += fn.ellipse.semiMajorM
            val p = toLocal(fn.lat, fn.lon, lat0, lon0)
            val az = Math.toRadians(fn.ellipse.majorAzimuthDeg)
            val dx = tx.x - p.x; val dy = tx.y - p.y
            val along = dx * sin(az) + dy * cos(az); val across = dx * cos(az) - dy * sin(az)
            val a = along / fn.ellipse.semiMajorM; val b = across / fn.ellipse.semiMinorM
            if (a * a + b * b <= 1.0) inside++
        }
        assertTrue("noisy $noisy quiet $quiet", noisy > quiet)
        assertTrue("inside $inside of $runs", inside >= runs * 3 / 4)
    }

    @Test fun fitNeedsGeometry() {
        assertNull(fitPathLoss(emptyList()))
        val same = (0 until 20).map { Sample(it * 1000L, lat0, lon0, null, -50.0, 1) }
        assertNull(fitPathLoss(same))
    }

    @Test fun walkingTowardIsHotAwayIsCold() {
        val tx = XY(80.0, 0.0)       // close enough that 40 m of walking changes the level well beyond the noise
        val rng = Random(3)
        fun leg(from: Double, to: Double): List<Sample> = (0 until 15).map { i ->
            val x = from + (to - from) * i / 14.0
            val (la, lo) = fromLocal(x, 0.0, lat0, lon0)
            Sample(i * 1000L, la, lo, null, level(x, 0.0, tx, -20.0, 3.0, rng, 1.0), 1)
        }
        assertEquals(Trend.HOT, trend(leg(0.0, 40.0)).trend)
        assertEquals(Trend.COLD, trend(leg(40.0, 0.0)).trend)
        assertEquals(Trend.UNKNOWN, trend(leg(0.0, 1.0)).trend)       // barely moved
        val flat = (0 until 15).map { i ->
            val (la, lo) = fromLocal(i * 3.0, 0.0, lat0, lon0)
            Sample(i * 1000L, la, lo, null, -50.0 + (if (i % 2 == 0) 2.0 else -2.0), 1)
        }
        assertEquals(Trend.FLAT, trend(flat).trend)
    }

    @Test fun arrowIsRelativeToHeading() {
        val (la, lo) = fromLocal(0.0, 100.0, lat0, lon0)       // due north
        assertEquals(0.0, arrowDeg(lat0, lon0, la, lo, 0.0), 1e-6)
        assertEquals(-90.0, arrowDeg(lat0, lon0, la, lo, 90.0), 1e-6)     // facing east: turn left
        assertEquals(180.0, arrowDeg(lat0, lon0, la, lo, 180.0), 1e-6)
    }

    private fun sweep(trueBearing: Double, beam: Double, sigma: Double, seed: Long, from: Double = 0.0, to: Double = 360.0): List<Pair<Double, Double>> {
        val rng = Random(seed)
        return (from.toInt() until to.toInt() step 3).map { h ->
            val off = wrap180(h - trueBearing)
            val k = off / (beam / 2)
            val g = -3.0 * k * k                              // -3 dB at half the beam width, floor 25 dB down
            h.toDouble() to (-40.0 + max(g, -25.0) + rng.nextGaussian() * sigma)
        }
    }

    @Test fun sweepBearing() {
        for (b in doubleArrayOf(10.0, 95.0, 181.0, 275.0, 355.0)) {
            val r = bearingFromSweep(sweep(b, 50.0, 1.5, b.toLong()))!!
            assertEquals("bearing $b", 0.0, abs(wrap180(r.bearingDeg - b)), 12.0)
            assertTrue(r.confidenceDb > 8.0)
            assertTrue("beam ${r.beamWidthDeg}", r.beamWidthDeg in 20.0..90.0)
        }
    }

    @Test fun sweepNeedsCoverage() {
        assertNull(bearingFromSweep(sweep(90.0, 50.0, 1.0, 1, 0.0, 180.0)))
    }

    @Test fun bearingLinesIntersect() {
        val tx = XY(200.0, 300.0)
        val lines = listOf(XY(0.0, 0.0), XY(250.0, -50.0), XY(-100.0, 200.0)).map { p ->
            val (la, lo) = fromLocal(p.x, p.y, lat0, lon0)
            BearingLine(la, lo, wrap360(Math.toDegrees(kotlin.math.atan2(tx.x - p.x, tx.y - p.y)) + 2.0))
        }
        val r = intersectBearings(lines)!!
        val p = toLocal(r.first, r.second, lat0, lon0)
        assertTrue(hypot(p.x - tx.x, p.y - tx.y) < 40.0)
        val par = listOf(BearingLine(lat0, lon0, 45.0), BearingLine(lat0 + 0.001, lon0, 45.0))
        assertNull(intersectBearings(par))
        assertNull(intersectBearings(par.take(1)))
    }

    @Test fun geigerRateAndClicks() {
        assertEquals(0.5, geigerRateHz(-50.0, -50.0, -10.0), 1e-9)
        assertEquals(25.0, geigerRateHz(0.0, -50.0, -10.0), 1e-9)
        assertTrue(geigerRateHz(-30.0, -50.0, -10.0) < geigerRateHz(-20.0, -50.0, -10.0))
        val rng = Random(5)
        val mean = (0 until 5000).map { nextClickMs(10.0, rng.nextDouble()) }.average()
        assertEquals(100.0, mean, 8.0)
    }

    @Test fun exports() {
        val s = listOf(Sample(0L, 54.7, 25.3, 90.0, -42.5, 4), Sample(1000L, 54.7001, 25.3001, null, -40.0, 4))
        val est = FitResult(54.701, 25.301, -20.0, 3.0, 1.5, Ellipse(30.0, 10.0, 45.0), 2, false)
        val csv = toCsv(s).lines()
        assertEquals("time_utc,lat,lon,heading_deg,level_db,readings", csv[0])
        assertEquals("1970-01-01T00:00:00Z,54.7000000,25.3000000,90.0,-42.50,4", csv[1])
        assertTrue(csv[2].contains(",,-40.00,4"))
        val gpx = toGpx(s, est, "a<b")
        assertTrue(gpx.contains("<trkpt lat=\"54.7000000\" lon=\"25.3000000\">") && gpx.contains("a&lt;b") && gpx.contains("Estimated transmitter"))
        val kml = toKml(s, est)
        assertTrue(kml.contains("<coordinates>25.3000000,54.7000000,0</coordinates>") && kml.contains("<LineString>"))
        assertEquals(0xFF, heatArgb(0.5) ushr 24)
    }
}
