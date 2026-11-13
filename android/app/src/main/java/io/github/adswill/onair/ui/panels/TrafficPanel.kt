// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.R
import io.github.adswill.onair.ui.ReceiverViewModel

/** ADS-B aircraft list: search, sort chips, emergency squawks on top. The map of the same aircraft is the [MapPanel]. */
object AircraftPanel : ModePanel {
    override val id = "adsb.list"
    override val order = 10
    @Composable override fun title() = stringResource(R.string.panel_traffic_aircraft)
    override fun appliesTo(ctx: PanelContext) = ctx.standard == Std.ADSB

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val all = remember(t) { TrafficParser.aircraft(t?.tables?.named("Aircraft")) }
        var query by rememberSaveable { mutableStateOf("") }
        var sortIx by rememberSaveable { mutableStateOf(0) }
        val sorts = TrafficParser.AircraftSort.entries
        val rows = remember(all, query, sortIx) { TrafficParser.sort(all.filter { TrafficParser.matches(it, query) }, sorts[sortIx]) }
        val names = listOf("Nearest", "Altitude", "Speed", "Newest", "Messages")
        Column(modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
            PanelSearch(query, { query = it })
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                sorts.forEachIndexed { i, _ -> Chip(names[i], color = p.accent, selected = sortIx == i) { sortIx = i } }
            }
            Label(stringResource(R.string.panel_count, rows.size, all.size), Modifier.padding(vertical = 3.dp), size = 11)
            // no return out of the Column's content (#435: early returns from inline composable lambdas broke the slot table)
            when {
                all.isEmpty() -> Label(stringResource(R.string.panel_nothing))
                rows.isEmpty() -> Label(stringResource(R.string.panel_no_match))
                else -> LazyColumn(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    items(rows, key = { it.icao }) { a ->
                        Column(Modifier.fillMaxWidth().background(p.panel).padding(horizontal = 10.dp, vertical = 6.dp)) {
                            Row(horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                                Text(a.title, color = if (a.emergency) p.bad else p.accent, fontFamily = Mono, fontSize = 14.sp)
                                Text(a.altText + " " + a.vrateArrow, color = p.text, fontFamily = Mono, fontSize = 13.sp)
                            }
                            val line = listOfNotNull(
                                a.icao, a.speedKt?.let { "%.0f kt".format(it) }, a.heading?.let { "%.0f°".format(it) },
                                a.distNm?.let { "%.0f nm".format(it) }, a.squawk.takeIf { it.isNotBlank() }?.let { "sq $it" },
                                "${a.messages} msg",
                            ).joinToString("   ")
                            Label(line, size = 11)
                            if (a.emergency) Label(stringResource(R.string.panel_emergency), color = p.bad, size = 11)
                        }
                    }
                }
            }
        }
    }
}

/** AIS vessel list: search by name, MMSI or type. */
object VesselPanel : ModePanel {
    override val id = "ais.list"
    override val order = 10
    @Composable override fun title() = stringResource(R.string.panel_traffic_vessels)
    override fun appliesTo(ctx: PanelContext) = ctx.standard == Std.AIS

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val all = remember(t) { TrafficParser.vessels(t?.tables?.named("Vessels")) }
        var query by rememberSaveable { mutableStateOf("") }
        val rows = remember(all, query) { all.filter { TrafficParser.matches(it, query) } }
        Column(modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
            PanelSearch(query, { query = it })
            Label(stringResource(R.string.panel_count, rows.size, all.size), Modifier.padding(vertical = 3.dp), size = 11)
            // no return out of the Column's content (#435: early returns from inline composable lambdas broke the slot table)
            when {
                all.isEmpty() -> Label(stringResource(R.string.panel_nothing))
                rows.isEmpty() -> Label(stringResource(R.string.panel_no_match))
                else -> LazyColumn(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    items(rows, key = { it.mmsi + it.name }) { v ->
                        Column(Modifier.fillMaxWidth().background(p.panel).padding(horizontal = 10.dp, vertical = 6.dp)) {
                            Row(horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                                Text(v.title, color = p.accent, fontFamily = Mono, fontSize = 14.sp)
                                Text(v.sogKn?.let { "%.1f kn".format(it) } ?: "", color = p.text, fontFamily = Mono, fontSize = 13.sp)
                            }
                            Label(listOf(v.mmsi, v.type, v.heard, if (v.hasPosition) "%.3f, %.3f".format(v.lat, v.lon) else "").filter { it.isNotBlank() }.joinToString("   "), size = 11)
                        }
                    }
                }
            }
        }
    }
}
