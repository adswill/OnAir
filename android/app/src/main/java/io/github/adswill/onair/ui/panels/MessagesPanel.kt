package io.github.adswill.onair.ui.panels

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
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
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.R
import io.github.adswill.onair.ui.ReceiverViewModel

/**
 * Message list for every text-carrying mode: ACARS, VDL2, HFDL, Aero, Inmarsat, Iridium, pagers (POCSAG/FLEX), APRS and packet frames,
 * D-STAR, SAME alerts, mesh, FT8/WSPR-style HF digital decodes, JS8. Newest first, search, tap a message for all its fields.
 */
object MessagesPanel : ModePanel {
    override val id = "messages"
    override val order = 20
    @Composable override fun title() = stringResource(R.string.panel_messages)
    override fun appliesTo(ctx: PanelContext) = ctx.standard in Std.messageLists

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val all = remember(t) { MessageParser.parse(MessageParser.pick(t?.tables ?: emptyList())) }
        var query by rememberSaveable { mutableStateOf("") }
        var open by remember { mutableStateOf<Int?>(null) }
        val rows = remember(all, query) { all.filter { MessageParser.matches(it, query) } }
        Column(modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
            PanelSearch(query, { query = it })
            Label(stringResource(R.string.panel_count, rows.size, all.size), Modifier.padding(vertical = 3.dp), size = 11)
            // no return out of the Column's content (#435: early returns from inline composable lambdas broke the slot table)
            when {
                all.isEmpty() -> Label(stringResource(R.string.panel_nothing))
                rows.isEmpty() -> Label(stringResource(R.string.panel_no_match))
                else -> LazyColumn(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    itemsIndexed(rows) { i, m ->
                        Column(Modifier.fillMaxWidth().background(p.panel).clickable { open = if (open == i) null else i }.padding(horizontal = 10.dp, vertical = 6.dp)) {
                            Label((m.time + "  " + m.head).trim(), size = 11)
                            if (m.text.isNotBlank()) Text(m.text, color = p.text, fontFamily = Mono, fontSize = 12.sp, maxLines = if (open == i) Int.MAX_VALUE else 4)
                            if (open == i) m.fields.filter { it.second.isNotBlank() }.forEach { (k, v) -> Label("$k: $v", size = 11, color = p.accent) }
                        }
                    }
                }
            }
        }
    }
}
