package io.github.adswill.onair.ui.panels

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
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

/**
 * The generic table browser: for ANY mode that returns tables (about 40 of them) and has no dedicated list. One chip per table, search,
 * tap a column chip to sort (tap again to reverse), each row as a card of "column: value". Complements the Results panel (cards + summary).
 */
object TablesPanel : ModePanel {
    override val id = "tables"
    override val order = 90
    @Composable override fun title() = stringResource(R.string.panel_tables)
    // Every mode: a mode without tables just shows the empty note, and Results always shows the summary and cards. Dedicated panels
    // already cover their own table, so the browser is hidden there to keep the tab row short.
    override fun appliesTo(ctx: PanelContext) = ctx.standard !in Std.dedicated

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val tables = t?.tables ?: emptyList()
        var tabIx by rememberSaveable { mutableStateOf(0) }
        var query by rememberSaveable { mutableStateOf("") }
        var sortCol by rememberSaveable { mutableStateOf(-1) }
        var desc by rememberSaveable { mutableStateOf(false) }
        Column(modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
            // no return out of the Column's content (#435: early returns from inline composable lambdas broke the slot table)
            if (tables.isEmpty()) Label(stringResource(R.string.panel_nothing))
            else {
                val tab = tables[tabIx.coerceIn(0, tables.size - 1)]
                if (tables.size > 1) Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    tables.forEachIndexed { i, x -> Chip(x.name, color = p.accent, selected = tab === x) { tabIx = i; sortCol = -1 } }
                }
                PanelSearch(query, { query = it })
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    tab.cols.forEachIndexed { i, c ->
                        Chip(c + if (sortCol == i) (if (desc) " ▼" else " ▲") else "", color = p.muted, selected = sortCol == i) {
                            if (sortCol == i) desc = !desc else { sortCol = i; desc = false }
                        }
                    }
                }
                val rows = remember(tab, query, sortCol, desc) { sortRows(filterRows(tab.rows, query), sortCol, desc) }
                Label(stringResource(R.string.panel_count, rows.size, tab.rows.size), Modifier.padding(vertical = 3.dp), size = 11)
                if (rows.isEmpty()) Label(stringResource(R.string.panel_no_match))
                else LazyColumn(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    itemsIndexed(rows) { _, r ->
                        Column(Modifier.fillMaxWidth().background(p.panel).padding(horizontal = 10.dp, vertical = 6.dp)) {
                            tab.cols.forEachIndexed { c, name ->
                                val v = r.getOrNull(c).orEmpty()
                                if (v.isNotBlank()) Row { Label("$name  ", size = 11); Text(v, color = p.text, fontFamily = Mono, fontSize = 12.sp) }
                            }
                        }
                    }
                }
            }
        }
    }
}
