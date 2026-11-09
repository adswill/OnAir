package io.github.adswill.onair

import android.content.res.Configuration
import androidx.compose.foundation.clickable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.model.Demod
import io.github.adswill.onair.model.ResultTable
import io.github.adswill.onair.model.Telemetry
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.ui.ReceiverViewModel

/** The active mode's results. FM and the general receiver have a screen of their own on top; every mode gets its cards and tables from the JSON. */
@Composable
fun ResultsTab(vm: ReceiverViewModel, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val t = vm.telemetry.collectAsStateWithLifecycle().value
    val settings by vm.settings.collectAsStateWithLifecycle()
    Column(modifier.verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 8.dp)) {
        if (t == null) { Label(stringResource(R.string.results_waiting)); return@Column }
        when (t.standard) {
            Telemetry.STD_FM -> FmPanel(t)
            Telemetry.STD_GENERAL -> GeneralPanel(vm, settings, t)
        }
        var sel by remember { mutableStateOf<String?>(null) }
        val land = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
        val mapTabs = t.tables.filter { hasPositions(it) }
        var bigMap by remember { mutableStateOf(false) }
        if (mapTabs.isNotEmpty() && !land) {
            Text(stringResource(if (bigMap) R.string.map_smaller else R.string.map_larger), Modifier.clickable { bigMap = !bigMap }.heightIn(min = LocalSpacing.current.touch).padding(vertical = 12.dp), color = p.accent, fontFamily = Mono, fontSize = 14.sp)
            PositionMap(mapTabs, sel, { sel = it }, settings.mapConsent, vm::setMapConsent, Modifier.fillMaxWidth().height(if (bigMap) 520.dp else 300.dp)); Spacer(Modifier.height(8.dp))
        }
        if (t.summary.isNotEmpty()) {
            SectionLabel(stringResource(R.string.mode_summary))
            Text(t.summary, color = p.text, fontFamily = Mono, fontSize = 14.sp)
            Spacer(Modifier.height(6.dp))
        }
        if (t.cards.isNotEmpty()) {
            Spacer(Modifier.height(4.dp))
            // two cards to a row
            t.cards.chunked(2).forEach { pair ->
                Row(Modifier.fillMaxWidth().padding(bottom = 6.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    pair.forEach { Card(it.first, it.second, Modifier.weight(1f)) }
                    if (pair.size == 1) Spacer(Modifier.weight(1f))
                }
            }
        }
        if (mapTabs.isNotEmpty() && land) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PositionMap(mapTabs, sel, { sel = it }, settings.mapConsent, vm::setMapConsent, Modifier.weight(1f).height(300.dp))
            Column(Modifier.weight(1f)) { for (tab in t.tables) ResultTableView(tab, sel) { sel = it } }
        } else if (mapTabs.isNotEmpty()) { for (tab in t.tables) ResultTableView(tab, sel) { sel = it } }
        else for (tab in t.tables) ResultTableView(tab)
        if (t.cards.isEmpty() && t.tables.isEmpty() && t.summary.isEmpty() && t.standard != Telemetry.STD_FM && t.standard != Telemetry.STD_GENERAL) Label(stringResource(R.string.results_none))
        Spacer(Modifier.height(16.dp))
    }
}

private fun colWidth(name: String): Dp = when (name) {
    "Message", "Text", "Info", "Comment", "Meaning", "From > To,Path" -> 260.dp
    "Name", "Callsign", "Registration", "Position" -> 140.dp
    "Pos", "Type" -> 125.dp
    else -> 88.dp
}

@Composable
fun ResultTableView(tab: ResultTable, selected: String? = null, onSelect: ((String?) -> Unit)? = null) {
    val p = LocalPalette.current
    Column(Modifier.padding(top = 6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            SectionLabel(tab.name)
            Label(stringResource(R.string.results_rows, tab.rows.size), Modifier.padding(start = 8.dp, top = 6.dp), size = 10)
        }
        // one column (a list of text lines): full width and wrapped, nothing to scroll sideways
        val oneCol = tab.cols.size == 1
        val cellMod: (String) -> Modifier = { n -> if (oneCol) Modifier.fillMaxWidth().padding(horizontal = 6.dp) else Modifier.width(colWidth(n)).padding(horizontal = 6.dp) }
        Column(Modifier.fillMaxWidth().background(p.field).border(1.dp, p.border).then(if (oneCol) Modifier else Modifier.horizontalScroll(rememberScrollState()))) {
            Row(Modifier.background(p.panel).padding(vertical = 6.dp)) {
                tab.cols.forEach { Text(it, cellMod(it), color = p.accent, fontFamily = Mono, fontSize = 13.sp, fontWeight = FontWeight.Bold, maxLines = 1) }
            }
            tab.rows.forEachIndexed { i, row ->
                val hit = selected != null && hasPositions(tab) && row.firstOrNull() == selected
                Row(Modifier.background(if (hit) p.accent.copy(alpha = 0.3f) else if (i % 2 == 1) p.grid.copy(alpha = 0.05f) else p.field)
                    .then(if (onSelect != null && hasPositions(tab)) Modifier.clickable { onSelect(if (hit) null else row.firstOrNull()) } else Modifier)
                    .padding(vertical = 5.dp)) {
                    row.forEachIndexed { c, cell ->
                        Text(cell, cellMod(tab.cols.getOrElse(c) { "" }), color = p.text, fontFamily = Mono, fontSize = 13.sp, maxLines = if (oneCol) 8 else 2)
                    }
                }
            }
        }
    }
}

@Composable
private fun FmPanel(t: Telemetry) {
    val p = LocalPalette.current
    val fm = t.fm ?: return
    Column(Modifier.fillMaxWidth().background(p.panel).border(1.dp, p.border).padding(12.dp)) {
        Label(stringResource(R.string.fm_station))
        Text(fm.ps.ifBlank { "--------" }, color = p.accent, fontFamily = Mono, fontSize = 26.sp)
        Spacer(Modifier.height(6.dp))
        Label(stringResource(R.string.fm_radiotext))
        Text(fm.rt.ifBlank { "-" }, color = p.text, fontFamily = Mono, fontSize = 13.sp)
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Card(stringResource(R.string.fm_rds), stringResource(if (fm.rdsSync) R.string.fm_rds_sync else R.string.fm_rds_none), Modifier.weight(1f))
            Card("Sound", stringResource(if (fm.stereo) R.string.fm_stereo else R.string.fm_mono), Modifier.weight(1f))
            if (fm.pi != 0) Card(stringResource(R.string.fm_pi), "%04X".format(fm.pi), Modifier.weight(1f))
        }
        if (fm.pty.isNotEmpty()) { Spacer(Modifier.height(6.dp)); Label(stringResource(R.string.fm_programme_type) + ": " + fm.pty, color = p.text) }
    }
    Spacer(Modifier.height(8.dp))
}

@Composable
private fun GeneralPanel(vm: ReceiverViewModel, s: Settings, t: Telemetry) {
    val p = LocalPalette.current
    val names = listOf(R.string.demod_nfm, R.string.demod_wfm, R.string.demod_am, R.string.demod_usb, R.string.demod_lsb, R.string.demod_cw)
    Column(Modifier.fillMaxWidth().background(p.panel).border(1.dp, p.border).padding(12.dp)) {
        Dropdown(names.map { stringResource(it) }, s.genDemod.ordinal, { vm.setGeneral(Demod.entries[it], s.genSquelch, s.genSquelchAuto, s.genSquelchDb) }, label = stringResource(R.string.general_demod))
        Spacer(Modifier.height(6.dp))
        CheckRow(stringResource(R.string.general_squelch), s.genSquelch) { vm.setGeneral(s.genDemod, it, s.genSquelchAuto, s.genSquelchDb) }
        if (s.genSquelch) {
            CheckRow(stringResource(R.string.general_squelch_auto), s.genSquelchAuto) { vm.setGeneral(s.genDemod, s.genSquelch, it, s.genSquelchDb) }
            if (!s.genSquelchAuto) {
                var level by remember(s.genSquelchDb) { mutableStateOf(s.genSquelchDb) }
                SliderRow(stringResource(R.string.general_squelch_level), level, -120f..0f, "${level.toInt()}", { level = it }, { vm.setGeneral(s.genDemod, s.genSquelch, s.genSquelchAuto, level) })
            }
        }
        val g = t.general
        if (g != null) {
            Spacer(Modifier.height(6.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Card(stringResource(R.string.general_squelch), stringResource(if (g.squelchOpen) R.string.general_open else R.string.general_closed), Modifier.weight(1f))
                Card(stringResource(R.string.general_level), "%.0f dBFS".format(g.levelDb), Modifier.weight(1f))
                Card(stringResource(R.string.general_audio), "%.0f dBFS".format(g.audioDb), Modifier.weight(1f))
            }
        }
    }
    Spacer(Modifier.height(8.dp))
}
