// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels.walkdf

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * The walk in progress. It lives in memory only (never written to disk, never sent anywhere: location stays on the device) and outlives
 * the panel, so switching tabs does not lose the walk. All calls are on the main thread.
 */
object WalkSession {
    /** Bumped on every change so composables can re-read the lists. */
    var version by mutableIntStateOf(0); private set

    var running by mutableStateOf(false)
    val raw = ArrayList<Raw>()
    var samples: List<Sample> = emptyList(); private set
    var fit: FitResult? by mutableStateOf(null)
    val bearings = ArrayList<BearingLine>()
    var sweeping by mutableStateOf(false)
    private val sweepPts = ArrayList<Pair<Double, Double>>()
    var lastSweep: BearingResult? by mutableStateOf(null)

    /** Newest fix and compass heading, as the listeners deliver them. */
    @Volatile var lastLat = Double.NaN
    @Volatile var lastLon = Double.NaN
    @Volatile var lastAcc = 999.0
    @Volatile var lastFixMs = 0L
    @Volatile var heading: Double? = null

    private const val MAX_RAW = 20_000          // ~ 2.5 h at 2 readings per second

    fun addReading(nowMs: Long, levelDb: Double) {
        if (!running || nowMs - lastFixMs > 4000 || lastLat.isNaN() || !levelDb.isFinite()) return
        raw.add(Raw(nowMs, lastLat, lastLon, lastAcc, heading, levelDb))
        if (raw.size > MAX_RAW) raw.subList(0, raw.size - MAX_RAW).clear()
        if (sweeping) heading?.let { sweepPts.add(it to levelDb) }
        samples = perSecond(raw)
        version++
    }

    fun clear() {
        raw.clear(); samples = emptyList(); fit = null; bearings.clear(); sweepPts.clear(); sweeping = false; lastSweep = null; version++
    }

    /** The user marks the antenna heading: the phone's heading right now is taken as the direction of the signal. */
    fun markAntenna(): Boolean {
        val h = heading ?: return false
        if (lastLat.isNaN()) return false
        bearings.add(BearingLine(lastLat, lastLon, h)); version++
        return true
    }

    fun startSweep() { sweepPts.clear(); lastSweep = null; sweeping = true }

    /** Ends the sweep; with enough coverage the bearing is kept as a line from the current position. Returns the result or null. */
    fun endSweep(): BearingResult? {
        sweeping = false
        val r = bearingFromSweep(sweepPts)
        lastSweep = r
        if (r != null && !lastLat.isNaN()) { bearings.add(BearingLine(lastLat, lastLon, r.bearingDeg)); version++ }
        return r
    }

    val sweepCount get() = sweepPts.size
}
