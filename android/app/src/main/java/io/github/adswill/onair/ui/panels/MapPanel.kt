// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.PositionMap
import io.github.adswill.onair.R
import io.github.adswill.onair.hasPositions
import io.github.adswill.onair.ui.ReceiverViewModel

/** The full-size map of the mode's positions (aircraft, vessels, APRS stations, HFDL/VDL2/Aero aircraft, mesh nodes), using the existing PositionMap. */
object MapPanel : ModePanel {
    override val id = "map"
    override val order = 15
    @Composable override fun title() = stringResource(R.string.panel_map)
    override fun appliesTo(ctx: PanelContext) = ctx.standard in Std.mapped

    @Composable
    override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) {
        val t by vm.telemetry.collectAsStateWithLifecycle()
        val settings by vm.settings.collectAsStateWithLifecycle()
        var sel by remember { mutableStateOf<String?>(null) }
        val tabs = (t?.tables ?: emptyList()).filter { hasPositions(it) }
        PositionMap(tabs, sel, { sel = it }, settings.mapConsent, vm::setMapConsent, modifier)
    }
}

/** Registers the built-in dedicated panels (found by ServiceLoader, see META-INF/services). */
class BuiltinPanelProvider : ModePanelProvider {
    override fun panels(): List<ModePanel> = listOf(TvPanel, CdrPanel, RdsPanel, AircraftPanel, VesselPanel, MapPanel, MessagesPanel, TablesPanel, io.github.adswill.onair.ui.panels.walkdf.WalkDfPanel)
}
