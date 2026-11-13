// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.Mono
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.SectionLabel
import io.github.adswill.onair.ui.ReceiverViewModel

/**
 * CDR (China Digital Radio, #463): the services of the multiplex, which one plays, and the DRA+ add-on. OnAir ships no DRA+ decoder
 * (docs/CODECS.md): without the add-on the sound is silent, and this panel says so and how to install it instead of leaving the phone quiet.
 * The add-on is a .so the user builds for arm64 and imports here; it is copied into the app's own codecs folder (shared storage cannot hold
 * code that runs) and the core looks for it again at once.
 */
object CdrPanel : ModePanel {
    override val id = "cdr.sound"
    /** Before Results: the sound and the service list are what a listener wants first. */
    override val order = -1
    @Composable override fun title() = stringResource(R.string.panel_cdr)
    override fun appliesTo(ctx: PanelContext) = ctx.standard == Std.CDR

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val p = LocalPalette.current
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val codec by vm.codecs.collectAsStateWithLifecycle()
        val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) vm.importCodec(uri) }
        val cdr = t?.cdr
        // the telemetry says what the receiver sees; the import result can be newer (the receiver reports again within a second)
        val found = codec.found || cdr?.draFound == true
        val problem = codec.problem.ifEmpty { cdr?.draProblem.orEmpty() }
        Column(modifier.verticalScroll(rememberScrollState()).padding(12.dp)) {
            SectionLabel(stringResource(R.string.cdr_sound))
            Column(Modifier.fillMaxWidth().background(p.panel).padding(12.dp)) {
                if (found) {
                    Text(stringResource(R.string.cdr_dra_loaded), color = p.ok, fontFamily = Mono, fontSize = 14.sp)
                    val path = codec.path.ifEmpty { cdr?.draPath.orEmpty() }
                    if (path.isNotEmpty()) Label(path, size = 11)
                    if (cdr != null && cdr.anyAudio && cdr.playing != 0 && cdr.pcmFrames == 0L) Label(stringResource(R.string.cdr_dra_waiting), size = 12)
                } else {
                    Text(
                        if (problem.isEmpty()) stringResource(R.string.cdr_dra_missing) else stringResource(R.string.cdr_dra_unusable, problem),
                        color = p.warn, fontFamily = Mono, fontSize = 14.sp,
                    )
                    Spacer(Modifier.height(6.dp))
                    Label(stringResource(R.string.cdr_dra_steps), size = 12, color = p.text)
                }
                Spacer(Modifier.height(8.dp))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    PanelButton(stringResource(if (found) R.string.cdr_dra_replace else R.string.cdr_dra_import)) { pick.launch(arrayOf("*/*")) }
                    if (found) PanelButton(stringResource(R.string.cdr_dra_remove)) { vm.removeCodec() }
                }
                Spacer(Modifier.height(4.dp))
                Label(stringResource(R.string.cdr_dra_note), size = 11)
            }
            Spacer(Modifier.height(10.dp))
            SectionLabel(stringResource(R.string.cdr_services))
            val services = cdr?.services.orEmpty()
            if (services.isEmpty()) Label(stringResource(if (t == null) R.string.results_waiting else R.string.cdr_no_services))
            else {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Chip(stringResource(R.string.cdr_auto), color = p.accent, selected = cdr?.listen == -1) { vm.cdrListen(-1) }
                }
                Spacer(Modifier.height(6.dp))
                for (s in services) {
                    val playing = cdr?.playing == s.id
                    Column(Modifier.fillMaxWidth().padding(bottom = 6.dp).background(p.panel).padding(horizontal = 10.dp, vertical = 6.dp)) {
                        Text(s.name + if (playing) "  ♪" else "", color = if (playing) p.accent else p.text, fontFamily = Mono, fontSize = 14.sp)
                        if (s.format.isNotEmpty()) Label(s.format, size = 11)
                        if (s.audio) Chip(
                            stringResource(if (cdr?.listen == s.id) R.string.cdr_listening else R.string.cdr_listen), color = p.accent,
                            selected = cdr?.listen == s.id,
                        ) { vm.cdrListen(s.id) }
                    }
                }
            }
        }
    }
}
