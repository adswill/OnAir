// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import io.github.adswill.onair.model.FmInfo
import io.github.adswill.onair.model.ResultTable

/*
 * The pure (Android-free) side of the mode panels: parsing the core's result tables into typed rows, searching, sorting. The panels
 * (TrafficPanel, MessagesPanel, RdsPanel, TablesPanel, MapPanel) only draw what is made here, so all of it is unit-tested on the JVM.
 * Column names are those of native/src/results_json.cpp; a missing column reads as empty, so a core that changes a table does not crash.
 */

/** The engine standards that have a dedicated panel (the numbers of results_json.cpp's dispatch). */
object Std {
    const val FM = 6
    const val ADSB = 12
    const val AIS = 15
    const val ACARS = 17
    const val INMARSAT = 18
    const val AERO = 19
    const val IRIDIUM = 20
    const val MESH = 21
    const val PAGER = 24
    const val PACKET = 25
    const val HFDIG = 26
    const val VDL2 = 28
    const val DSTAR = 34
    const val HFDL = 36
    const val JS8 = 38
    const val SAME = 77
    /** CDR (China Digital Radio): its own Sound panel (CdrPanel); the Results panel keeps the cards and tables. */
    const val CDR = 23

    /** Standards whose message-like table (time, header fields, free text) reads best as a list. */
    val messageLists = setOf(ACARS, INMARSAT, AERO, IRIDIUM, MESH, PAGER, VDL2, HFDL, DSTAR, SAME, HFDIG, JS8, PACKET)
    /** Standards whose rows carry positions, so the full map panel is offered. */
    val mapped = setOf(ADSB, AIS, PACKET, HFDL, VDL2, AERO, MESH, DSTAR)
    val dedicated: Set<Int> = messageLists + mapped + setOf(FM, AIS, ADSB)
}

fun ResultTable.col(name: String): Int = cols.indexOf(name)
fun ResultTable.cell(row: List<String>, name: String): String = row.getOrNull(col(name)) ?: ""
fun List<ResultTable>.named(name: String): ResultTable? = firstOrNull { it.name.equals(name, ignoreCase = true) }

/** Case-insensitive "all words appear somewhere in the row" search; a blank query keeps everything. */
fun filterRows(rows: List<List<String>>, query: String): List<List<String>> {
    val words = query.trim().lowercase().split(' ').filter { it.isNotEmpty() }
    if (words.isEmpty()) return rows
    return rows.filter { r -> val line = r.joinToString(" ").lowercase(); words.all { line.contains(it) } }
}

/** Sorts rows by a column: numbers numerically (blank last), text alphabetically. */
fun sortRows(rows: List<List<String>>, col: Int, descending: Boolean): List<List<String>> {
    if (col < 0) return rows
    fun v(r: List<String>) = r.getOrNull(col)?.trim() ?: ""
    val numeric = rows.map { v(it) }.filter { it.isNotEmpty() }.all { it.toDoubleOrNull() != null }
    val cmp: Comparator<List<String>> = if (numeric)
        Comparator { a, b ->
            val x = v(a).toDoubleOrNull(); val y = v(b).toDoubleOrNull()
            when {
                x == null && y == null -> 0
                x == null -> 1
                y == null -> -1
                descending -> y.compareTo(x)
                else -> x.compareTo(y)
            }
        }
    else Comparator { a, b -> if (descending) v(b).compareTo(v(a), true) else v(a).compareTo(v(b), true) }
    return rows.sortedWith(cmp)
}

/** One aircraft of the ADS-B table. */
data class AircraftRow(
    val icao: String, val callsign: String, val altFt: Int?, val speedKt: Double?, val heading: Double?, val vrateFpm: Int?,
    val lat: Double?, val lon: Double?, val distNm: Double?, val squawk: String, val messages: Int, val ageSec: Double?,
) {
    val hasPosition get() = lat != null && lon != null
    /** 7500 hijack, 7600 radio failure, 7700 general emergency. */
    val emergency get() = squawk == "7500" || squawk == "7600" || squawk == "7700"
    val title get() = callsign.ifBlank { icao }
    /** "FL350" at or above 18 000 ft, else "4200 ft"; empty when unknown. */
    val altText get() = altFt?.let { if (it >= 18000) "FL%03d".format(it / 100) else "$it ft" } ?: ""
    val vrateArrow get() = when { vrateFpm == null -> ""; vrateFpm > 300 -> "↑"; vrateFpm < -300 -> "↓"; else -> "→" }
}

/** One AIS vessel. */
data class VesselRow(
    val name: String, val mmsi: String, val type: String, val sogKn: Double?, val heard: String, val lat: Double?, val lon: Double?, val channel: String,
) {
    val hasPosition get() = lat != null && lon != null
    val title get() = name.ifBlank { mmsi }
}

object TrafficParser {
    private fun d(s: String) = s.trim().toDoubleOrNull()
    private fun i(s: String) = s.trim().toIntOrNull() ?: s.trim().toDoubleOrNull()?.toInt()

    fun aircraft(tab: ResultTable?): List<AircraftRow> = tab?.rows?.map { r ->
        AircraftRow(
            tab.cell(r, "ICAO"), tab.cell(r, "Callsign"), i(tab.cell(r, "Alt ft")), d(tab.cell(r, "Spd kt")), d(tab.cell(r, "Hdg")),
            i(tab.cell(r, "VS fpm")), d(tab.cell(r, "Lat")), d(tab.cell(r, "Lon")), d(tab.cell(r, "Dist nm")), tab.cell(r, "Sqk"),
            i(tab.cell(r, "Msgs")) ?: 0, d(tab.cell(r, "Age s")),
        )
    } ?: emptyList()

    /** The AIS "Pos" cell is "lat, lon" or empty. */
    fun vessels(tab: ResultTable?): List<VesselRow> = tab?.rows?.map { r ->
        val pos = tab.cell(r, "Pos").split(',').map { d(it) }
        VesselRow(tab.cell(r, "Name"), tab.cell(r, "MMSI"), tab.cell(r, "Type"), d(tab.cell(r, "SOG kn")), tab.cell(r, "Heard"),
            pos.getOrNull(0), pos.getOrNull(1), tab.cell(r, "Ch"))
    } ?: emptyList()

    enum class AircraftSort { NEAREST, ALTITUDE, SPEED, NEWEST, MESSAGES }

    /** Emergencies always first, then [by]. */
    fun sort(list: List<AircraftRow>, by: AircraftSort): List<AircraftRow> {
        val sorted = when (by) {
            AircraftSort.NEAREST -> list.sortedWith(compareBy(nullsLast<Double>()) { it.distNm })
            AircraftSort.ALTITUDE -> list.sortedByDescending { it.altFt ?: Int.MIN_VALUE }
            AircraftSort.SPEED -> list.sortedByDescending { it.speedKt ?: Double.NEGATIVE_INFINITY }
            AircraftSort.NEWEST -> list.sortedWith(compareBy(nullsLast<Double>()) { it.ageSec })
            AircraftSort.MESSAGES -> list.sortedByDescending { it.messages }
        }
        return sorted.sortedByDescending { it.emergency }
    }

    fun matches(a: AircraftRow, q: String) = q.isBlank() || listOf(a.icao, a.callsign, a.squawk).any { it.contains(q.trim(), true) }
    fun matches(v: VesselRow, q: String) = q.isBlank() || listOf(v.name, v.mmsi, v.type).any { it.contains(q.trim(), true) }
}

/** One entry of a message list: when, who/what (the short fields), the text. */
data class MessageEntry(val time: String, val head: String, val text: String, val fields: List<Pair<String, String>>)

object MessageParser {
    private val timeCols = listOf("Time UTC", "Time", "UTC")
    private val textCols = listOf("Text", "Message", "Info", "Comment", "Payload", "Data")

    /** [tab] rows into entries: the time column, the text column and the other non-empty cells as the head line. */
    fun parse(tab: ResultTable?): List<MessageEntry> {
        if (tab == null) return emptyList()
        val tc = timeCols.map { tab.col(it) }.firstOrNull { it >= 0 } ?: -1
        val xc = textCols.map { tab.col(it) }.firstOrNull { it >= 0 } ?: -1
        return tab.rows.map { r ->
            val head = tab.cols.indices.filter { it != tc && it != xc }.mapNotNull { c -> r.getOrNull(c)?.takeIf { it.isNotBlank() } }.joinToString("  ")
            MessageEntry(
                if (tc >= 0) r.getOrNull(tc).orEmpty() else "", head, if (xc >= 0) r.getOrNull(xc).orEmpty() else "",
                tab.cols.indices.map { tab.cols[it] to r.getOrNull(it).orEmpty() },
            )
        }
    }

    fun matches(e: MessageEntry, q: String): Boolean {
        val words = q.trim().lowercase().split(' ').filter { it.isNotEmpty() }
        if (words.isEmpty()) return true
        val line = "${e.time} ${e.head} ${e.text}".lowercase()
        return words.all { line.contains(it) }
    }

    /** The table a message panel shows: "Messages", else "Decodes", "Frames", "Alerts", "Pages" or the first table. */
    fun pick(tables: List<ResultTable>): ResultTable? =
        listOf("Messages", "Decodes", "Frames", "Alerts", "Pages").firstNotNullOfOrNull { tables.named(it) } ?: tables.firstOrNull()
}

/** The FM panel's RDS view. */
data class RdsView(val ps: String, val radioText: String, val pi: String, val pty: String, val synced: Boolean, val stereo: Boolean,
                   val rows: List<Pair<String, String>> = emptyList()) {
    companion object {
        fun of(f: FmInfo?) = f?.let {
            RdsView(it.ps.trim().ifEmpty { "--------" }, it.rt.trim(), if (it.pi != 0) "%04X".format(it.pi) else "", it.pty, it.rdsSync, it.stereo, it.rdsRows)
        }
    }
}
