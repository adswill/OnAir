// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Card
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.R
import io.github.adswill.onair.ui.ReceiverViewModel

/** FM broadcast: the station name large, RadioText, programme type, PI, stereo and RDS lock. */
object RdsPanel : ModePanel {
    override val id = "fm.rds"
    override val order = 5
    @Composable override fun title() = stringResource(R.string.panel_rds)
    override fun appliesTo(ctx: PanelContext) = ctx.standard == Std.FM

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val v = RdsView.of(t?.fm)
        Column(modifier.verticalScroll(rememberScrollState()).padding(12.dp)) {
            // no return out of the Column's content (#435: early returns from inline composable lambdas broke the slot table)
            if (v == null) Label(stringResource(R.string.results_waiting))
            else {
                Column(Modifier.fillMaxWidth().background(p.panel).padding(14.dp)) {
                    Label(stringResource(R.string.fm_station))
                    Text(v.ps, color = p.accent, fontFamily = Mono, fontSize = 32.sp)
                    Spacer(Modifier.height(8.dp))
                    Label(stringResource(R.string.fm_radiotext))
                    Text(v.radioText.ifBlank { "-" }, color = p.text, fontFamily = Mono, fontSize = 14.sp)
                }
                Spacer(Modifier.height(8.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Card(stringResource(R.string.fm_rds), stringResource(if (v.synced) R.string.fm_rds_sync else R.string.fm_rds_none), Modifier.weight(1f))
                    Card("Sound", stringResource(if (v.stereo) R.string.fm_stereo else R.string.fm_mono), Modifier.weight(1f))
                }
                Spacer(Modifier.height(6.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Card(stringResource(R.string.fm_pi), v.pi.ifBlank { "-" }, Modifier.weight(1f))
                    Card(stringResource(R.string.fm_programme_type), v.pty.ifBlank { "-" }, Modifier.weight(1f))
                }
                if (v.rows.isNotEmpty()) {   // #436: everything else the station sends, as the desktop's "RDS details"
                    Spacer(Modifier.height(8.dp))
                    Column(Modifier.fillMaxWidth().background(p.panel).padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Label(stringResource(R.string.fm_rds_details))
                        v.rows.forEach { (k, value) ->
                            Column {
                                Label(k)
                                Text(value, color = p.text, fontFamily = Mono, fontSize = 14.sp)
                            }
                        }
                    }
                }
            }
        }
    }
}
