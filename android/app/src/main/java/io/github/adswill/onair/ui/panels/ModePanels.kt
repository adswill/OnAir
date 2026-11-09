package io.github.adswill.onair.ui.panels

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.ModeDef
import io.github.adswill.onair.Modes
import io.github.adswill.onair.R
import io.github.adswill.onair.ResultsTab
import io.github.adswill.onair.data.ReceiverState
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.groupAccent
import io.github.adswill.onair.ui.ReceiverViewModel
import java.util.ServiceLoader
import java.util.concurrent.CopyOnWriteArrayList

/*
 * The extension point for mode panels (android-modes-030 and later). A mode adds a screen of its own WITHOUT touching the UI files:
 *
 *   1. Write a class that implements [ModePanel] (in your own file, e.g. ui/panels/adsb/AdsbPanel.kt).
 *   2. Register it, either way:
 *        a) a [ModePanelProvider] with a public no-argument constructor, listed in
 *           app/src/main/resources/META-INF/services/io.github.adswill.onair.ui.panels.ModePanelProvider (one class name per line;
 *           R8 keeps providers, see proguard-rules.pro), or
 *        b) ModePanels.register(MyPanel()) from code that runs at start-up (your own initialiser).
 *   3. That is all: the panel host (right pane on tablets, below the spectrum and on the Panels tab on phones) shows every panel whose
 *      [ModePanel.appliesTo] is true for the current mode, as tabs ordered by [ModePanel.order]. The generic Results panel (cards and
 *      tables of every mode) is always there at order 0, so a mode only adds what the generic view cannot show.
 *
 * Rules: read state through the view model's StateFlows (collectAsStateWithLifecycle), send actions through view-model or repository
 * intents, never call OnAirNative from a panel (ARCHITECTURE.md "Threading rules"); use the theme tokens (LocalPalette, MaterialTheme
 * typography, Widgets.kt) so the panel looks like OnAir; give every control a 48 dp target and a content description.
 */

/** What a panel is shown for: the current mode, as plain values (cheap to compare, so the host recomposes only on a mode change). */
@Immutable
data class PanelContext(
    val modeIndex: Int,
    val mode: ModeDef,
    /** The standard as Telemetry.standard reports it (ReceiverState.telemetryStdOf): what [Std] and the panels compare with. */
    val standard: Int,
    /** The protocol id of a shared receiver (ModeDef.subId), or null. */
    val subId: String?,
) {
    companion object {
        fun of(st: ReceiverState) = PanelContext(st.modeIndex, st.mode, ReceiverState.telemetryStdOf(st.mode.family), st.mode.subId)
    }
}

/** One panel: a tab in the panel host. */
interface ModePanel {
    /** Unique and stable (it is saved as the selected tab), e.g. "results" or "adsb.map". */
    val id: String
    /** Lower comes first; the built-in Results panel is 0. */
    val order: Int get() = 100
    @Composable fun title(): String
    fun appliesTo(ctx: PanelContext): Boolean
    @Composable fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier)
}

/** Lists panels; found with java.util.ServiceLoader. */
interface ModePanelProvider {
    fun panels(): List<ModePanel>
}

object ModePanels {
    private val registered = CopyOnWriteArrayList<ModePanel>()

    private val fromProviders: List<ModePanel> by lazy {
        try {
            ServiceLoader.load(ModePanelProvider::class.java, ModePanelProvider::class.java.classLoader).flatMap { it.panels() }
        } catch (e: Throwable) { AppLog.e("app", "mode panel providers failed to load", e); emptyList() }
    }

    /** Adds a panel at run time (idempotent by id). */
    fun register(panel: ModePanel) {
        if (registered.none { it.id == panel.id }) registered.add(panel)
    }

    fun all(): List<ModePanel> = (listOf(ResultsPanel) + fromProviders + registered).distinctBy { it.id }.sortedBy { it.order }

    fun forMode(ctx: PanelContext): List<ModePanel> = all().filter { p ->
        try { p.appliesTo(ctx) } catch (e: Throwable) { AppLog.e("app", "panel ${p.id} failed", e); false }
    }
}

/** The built-in panel: the mode's cards and tables, FM and general-receiver panels, the map (ResultsTab). */
object ResultsPanel : ModePanel {
    override val id = "results"
    override val order = 0
    @Composable override fun title() = stringResource(R.string.tab_results)
    override fun appliesTo(ctx: PanelContext) = true
    @Composable override fun Content(vm: ReceiverViewModel, ctx: PanelContext, modifier: Modifier) = ResultsTab(vm, modifier)
}

/** Shows the panels of the current mode, as a row of tabs when there are several. */
@Composable
fun PanelHost(vm: ReceiverViewModel, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val st by vm.state.collectAsStateWithLifecycle()
    val ctx = remember(st.modeIndex, st.mode) { PanelContext.of(st) }
    val panels = remember(ctx) { ModePanels.forMode(ctx) }
    var selectedId by rememberSaveable(ctx.modeIndex) { mutableStateOf(panels.firstOrNull()?.id ?: "") }
    val sel = panels.firstOrNull { it.id == selectedId } ?: panels.firstOrNull()
    val accent = groupAccent(Modes.all.getOrNull(ctx.modeIndex)?.group ?: 0)
    Column(modifier.background(p.window)) {
        if (panels.size > 1) {
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                for (panel in panels) Chip(panel.title(), color = if (panel == sel) accent else p.muted, selected = panel == sel) { selectedId = panel.id }
            }
        }
        Box(Modifier.fillMaxSize()) { sel?.Content(vm, ctx, Modifier.fillMaxSize()) }
    }
}
