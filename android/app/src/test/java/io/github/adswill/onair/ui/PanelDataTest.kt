// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import io.github.adswill.onair.model.FmInfo
import io.github.adswill.onair.model.ResultTable
import io.github.adswill.onair.ui.panels.MessageParser
import io.github.adswill.onair.ui.panels.RdsView
import io.github.adswill.onair.ui.panels.Std
import io.github.adswill.onair.ui.panels.TrafficParser
import io.github.adswill.onair.ui.panels.filterRows
import io.github.adswill.onair.ui.panels.named
import io.github.adswill.onair.ui.panels.sortRows
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PanelDataTest {
    private val adsbCols = listOf("ICAO", "Callsign", "Alt ft", "Spd kt", "Hdg", "VS fpm", "Lat", "Lon", "Dist nm", "Sqk", "Msgs", "Age s")
    private val adsb = ResultTable("Aircraft", adsbCols, listOf(
        listOf("4CA123", "RYR12AB", "35000", "450", "90", "0", "54.1000", "24.2000", "40", "1234", "50", "2"),
        listOf("4CA999", "", "4200", "120", "180", "-900", "", "", "5", "7700", "7", "1"),
        listOf("400001", "BAW9", "", "", "", "", "55.0", "25.0", "", "", "3", "30"),
    ))

    @Test fun aircraftParse() {
        val a = TrafficParser.aircraft(adsb)
        assertEquals(3, a.size)
        assertEquals("FL350", a[0].altText)
        assertEquals("4200 ft", a[1].altText)
        assertEquals("", a[2].altText)
        assertTrue(a[0].hasPosition); assertFalse(a[1].hasPosition)
        assertTrue(a[1].emergency); assertFalse(a[0].emergency)
        assertEquals("4CA999", a[1].title)
        assertEquals("↓", a[1].vrateArrow)
        assertEquals("→", a[0].vrateArrow)
    }

    @Test fun aircraftSortPutsEmergencyFirst() {
        val a = TrafficParser.aircraft(adsb)
        assertEquals("4CA999", TrafficParser.sort(a, TrafficParser.AircraftSort.ALTITUDE)[0].icao)
        val byAlt = TrafficParser.sort(a.filter { !it.emergency }, TrafficParser.AircraftSort.ALTITUDE)
        assertEquals(listOf("4CA123", "400001"), byAlt.map { it.icao })
        val near = TrafficParser.sort(a.filter { !it.emergency }, TrafficParser.AircraftSort.NEAREST)
        assertEquals("4CA123", near[0].icao)
        assertEquals("400001", near[1].icao) // unknown distance last
    }

    @Test fun aircraftSearchAndMissingTable() {
        val a = TrafficParser.aircraft(adsb)
        assertEquals(1, a.count { TrafficParser.matches(it, "ryr") })
        assertEquals(1, a.count { TrafficParser.matches(it, "7700") })
        assertEquals(3, a.count { TrafficParser.matches(it, " ") })
        assertTrue(TrafficParser.aircraft(null).isEmpty())
        // a table missing columns does not throw
        assertEquals(1, TrafficParser.aircraft(ResultTable("Aircraft", listOf("ICAO"), listOf(listOf("ABCDEF")))).size)
    }

    @Test fun vesselPosition() {
        val t = ResultTable("Vessels", listOf("Name", "MMSI", "Type", "SOG kn", "Heard", "Pos", "Ch"), listOf(
            listOf("MS TEST", "219000001", "Cargo", "12.5", "30 s", "55.1234, 21.5678", "A"),
            listOf("", "219000002", "", "", "5 min", "", "B"),
        ))
        val v = TrafficParser.vessels(t)
        assertEquals(55.1234, v[0].lat!!, 1e-9); assertEquals(21.5678, v[0].lon!!, 1e-9)
        assertTrue(v[0].hasPosition); assertFalse(v[1].hasPosition)
        assertEquals("219000002", v[1].title)
        assertNull(v[1].sogKn)
        assertTrue(TrafficParser.matches(v[0], "cargo")); assertFalse(TrafficParser.matches(v[0], "tanker"))
    }

    @Test fun messageParse() {
        val t = ResultTable("Messages", listOf("Time UTC", "MHz", "Reg", "Flight", "Label", "Meaning", "Text"), listOf(
            listOf("12:00:01", "131.550", "N123AB", "UAL1", "H1", "", "POS N5412W02412"),
        ))
        val m = MessageParser.parse(t)[0]
        assertEquals("12:00:01", m.time)
        assertEquals("POS N5412W02412", m.text)
        assertEquals("131.550  N123AB  UAL1  H1", m.head) // blank Meaning dropped
        assertTrue(MessageParser.matches(m, "ual1 pos"))
        assertFalse(MessageParser.matches(m, "xyz"))
        assertEquals(7, m.fields.size)
    }

    @Test fun messageTablePick() {
        val a = ResultTable("Aircraft", listOf("Reg"), emptyList())
        val m = ResultTable("Messages", listOf("Text"), emptyList())
        assertEquals("Messages", MessageParser.pick(listOf(a, m))!!.name)
        assertEquals("Aircraft", MessageParser.pick(listOf(a))!!.name)
        assertNull(MessageParser.pick(emptyList()))
        val d = ResultTable("Decodes", listOf("UTC", "Message"), listOf(listOf("1200", "CQ DX K1ABC FN42")))
        val e = MessageParser.parse(d)[0]
        assertEquals("1200", e.time); assertEquals("CQ DX K1ABC FN42", e.text)
    }

    @Test fun messageWithoutTextOrTimeColumn() {
        val t = ResultTable("Events", listOf("Event"), listOf(listOf("cell 1 found")))
        val e = MessageParser.parse(t)[0]
        assertEquals("", e.time); assertEquals("", e.text); assertEquals("cell 1 found", e.head)
    }

    @Test fun searchAndSortRows() {
        val rows = listOf(listOf("b", "10"), listOf("a", "9"), listOf("c", ""))
        assertEquals(listOf("a"), filterRows(rows, "a 9").map { it[0] })
        assertEquals(3, filterRows(rows, "").size)
        assertEquals(listOf("a", "b", "c"), sortRows(rows, 0, false).map { it[0] })
        assertEquals(listOf("c", "b", "a"), sortRows(rows, 0, true).map { it[0] })
        // numeric, blank last in both directions
        assertEquals(listOf("a", "b", "c"), sortRows(rows, 1, false).map { it[0] })
        assertEquals(listOf("b", "a", "c"), sortRows(rows, 1, true).map { it[0] })
        assertEquals(rows, sortRows(rows, -1, false))
        assertEquals(3, sortRows(rows, 7, false).size) // column out of range does not throw
    }

    @Test fun rds() {
        assertNull(RdsView.of(null))
        val v = RdsView.of(FmInfo("  ", " Now playing ", true, true, 0x5410, "Pop"))!!
        assertEquals("--------", v.ps); assertEquals("Now playing", v.radioText); assertEquals("5410", v.pi)
        assertEquals("", RdsView.of(FmInfo("BBC", "", false, false, 0, ""))!!.pi)
    }

    @Test fun routing() {
        assertTrue(Std.ACARS in Std.messageLists); assertTrue(Std.ADSB in Std.mapped)
        assertTrue(Std.ADSB in Std.dedicated); assertFalse(Std.HFDIG !in Std.dedicated)
        assertEquals(listOf("a"), listOf(ResultTable("A", emptyList(), emptyList())).named("a")?.let { listOf("a") })
    }
}
