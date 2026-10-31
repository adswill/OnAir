package io.github.adswill.onair.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Colormaps
import io.github.adswill.onair.Dropdown
import io.github.adswill.onair.GroupHeader
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalSpacing
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.OnAirIconButton
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.SliderRow
import io.github.adswill.onair.SwitchRow
import io.github.adswill.onair.TextBox
import io.github.adswill.onair.ToolRow
import io.github.adswill.onair.TypeLook
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.UiPrefs
import io.github.adswill.onair.ui.receiver.StepChoices
import io.github.adswill.onair.ui.receiver.stepText

/** One searchable setting: its group, the words it is found by, and how it draws. */
class SettingItem(val group: Int, val title: String, val keywords: String, val content: @Composable () -> Unit) {
    fun matches(q: String): Boolean = q.isBlank() || title.contains(q, true) || keywords.contains(q, true)
}

/**
 * Settings, grouped like the desktop's Settings window (Display, Tuning, Mode list, Sound, Accessibility, Privacy, Help) and
 * searchable: the search box keeps every setting whose title or keywords contain the text. Radio and tuner settings are on the
 * Radio screen, next to the source they belong to.
 */
@Composable
fun SettingsScreen(vm: ReceiverViewModel, onOpenLog: () -> Unit, onOpenAbout: () -> Unit, onRunSetup: () -> Unit, modifier: Modifier = Modifier) {
    val s by vm.settings.collectAsStateWithLifecycle()
    val ui by vm.ui.collectAsStateWithLifecycle()
    val actions = remember(vm) { vmSettingsActions(vm) }
    SettingsContent(s, ui, actions, onOpenLog, onOpenAbout, onRunSetup, modifier)
}

/** What the settings screen changes; the view model in the app, a no-op in previews. */
interface SettingsActions {
    fun setDark(on: Boolean); fun setFollowSystemTheme(on: Boolean); fun setTypeLook(l: TypeLook); fun setColormap(i: Int)
    fun setRange(lo: Float, hi: Float); fun autoRange(); fun setPeakHold(on: Boolean); fun setCentreTuning(on: Boolean); fun setStepHz(hz: Double)
    fun setHideUnused(on: Boolean); fun setOnlyCapable(on: Boolean); fun setSound(volume: Float, muted: Boolean); fun setReduceMotion(on: Boolean)
    fun setDiscovery(on: Boolean); fun setMapConsent(v: Int); fun setVerbose(on: Boolean)
}

private fun vmSettingsActions(vm: ReceiverViewModel) = object : SettingsActions {
    override fun setDark(on: Boolean) = vm.setDark(on)
    override fun setFollowSystemTheme(on: Boolean) = vm.setFollowSystemTheme(on)
    override fun setTypeLook(l: TypeLook) = vm.setTypeLook(l)
    override fun setColormap(i: Int) = vm.setColormap(i)
    override fun setRange(lo: Float, hi: Float) = vm.setRange(lo, hi)
    override fun autoRange() = vm.autoRange()
    override fun setPeakHold(on: Boolean) = vm.setPeakHold(on)
    override fun setCentreTuning(on: Boolean) = vm.setCentreTuning(on)
    override fun setStepHz(hz: Double) = vm.setStepHz(hz)
    override fun setHideUnused(on: Boolean) = vm.setHideUnused(on)
    override fun setOnlyCapable(on: Boolean) = vm.setOnlyCapable(on)
    override fun setSound(volume: Float, muted: Boolean) = vm.setSound(volume, muted)
    override fun setReduceMotion(on: Boolean) = vm.setReduceMotion(on)
    override fun setDiscovery(on: Boolean) = vm.setDiscovery(on)
    override fun setMapConsent(v: Int) = vm.setMapConsent(v)
    override fun setVerbose(on: Boolean) = vm.setVerbose(on)
}

@Composable
fun SettingsContent(
    s: Settings, ui: UiPrefs, vm: SettingsActions, onOpenLog: () -> Unit, onOpenAbout: () -> Unit, onRunSetup: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val p = LocalPalette.current
    var query by rememberSaveable { mutableStateOf("") }
    val groups = listOf(
        R.string.settings_display, R.string.settings_tuning, R.string.settings_modes, R.string.settings_sound,
        R.string.settings_access, R.string.settings_privacy, R.string.settings_help,
    ).map { stringResource(it) }

    val looks = listOf(stringResource(R.string.look_current), stringResource(R.string.look_clean), stringResource(R.string.look_compact))
    val consent = listOf(stringResource(R.string.map_ask), stringResource(R.string.map_yes), stringResource(R.string.map_no))
    val items = listOf(
        SettingItem(0, stringResource(R.string.dark_theme), "theme dark light colour color night") {
            SwitchRow(stringResource(R.string.dark_theme), s.dark, vm::setDark, enabled = !ui.followSystemTheme)
        },
        SettingItem(0, stringResource(R.string.follow_system_theme), "theme system dark light") {
            SwitchRow(stringResource(R.string.follow_system_theme), ui.followSystemTheme, vm::setFollowSystemTheme)
        },
        SettingItem(0, stringResource(R.string.font_look), "font type text look clean compact inter mono") {
            Dropdown(looks, ui.typeLook.ordinal, { vm.setTypeLook(TypeLook.entries[it]) }, label = stringResource(R.string.font_look))
        },
        SettingItem(0, stringResource(R.string.colours), "waterfall colour color map turbo viridis") {
            Dropdown(Colormaps.names, s.colormap, { vm.setColormap(it) }, label = stringResource(R.string.colours))
        },
        SettingItem(0, stringResource(R.string.spectrum_range), "spectrum range db level min max auto") {
            var lo by remember(s.minDb) { mutableFloatStateOf(s.minDb) }
            var hi by remember(s.maxDb) { mutableFloatStateOf(s.maxDb) }
            Column {
                SliderRow(stringResource(R.string.range_min), lo, -140f..-20f, "${lo.toInt()}", { lo = it }, { vm.setRange(lo, hi) })
                SliderRow(stringResource(R.string.range_max), hi, -80f..10f, "${hi.toInt()}", { hi = it }, { vm.setRange(lo, hi) })
                ToolRow { PanelButton(stringResource(R.string.auto)) { vm.autoRange() } }
            }
        },
        SettingItem(0, stringResource(R.string.peak_hold), "peak hold spectrum") {
            SwitchRow(stringResource(R.string.peak_hold), s.peakHold, vm::setPeakHold)
        },
        SettingItem(1, stringResource(R.string.tuning_centre_setting), "tuning centre center normal tap click") {
            SwitchRow(stringResource(R.string.tuning_centre_setting), ui.centreTuning, vm::setCentreTuning, help = stringResource(R.string.tuning_centre_help))
        },
        SettingItem(1, stringResource(R.string.step), "step frequency raster") {
            val names = StepChoices.map { if (it == 0.0) stringResource(R.string.step_auto_plain) else stepText(it) }
            Dropdown(names, StepChoices.indexOfFirst { it == ui.stepHz }.coerceAtLeast(0), { vm.setStepHz(StepChoices[it]) }, label = stringResource(R.string.step))
        },
        SettingItem(2, stringResource(R.string.filter_hide_unused), "hide modes unused list") {
            SwitchRow(stringResource(R.string.filter_hide_unused), ui.hideUnused, vm::setHideUnused, help = stringResource(R.string.filter_hide_unused_help))
        },
        SettingItem(2, stringResource(R.string.filter_only_capable), "radio capable sample rate modes") {
            SwitchRow(stringResource(R.string.filter_only_capable), ui.onlyCapable, vm::setOnlyCapable, help = stringResource(R.string.filter_only_capable_help))
        },
        SettingItem(3, stringResource(R.string.volume), "sound volume audio") {
            var vol by remember(s.volume) { mutableFloatStateOf(s.volume) }
            SliderRow(stringResource(R.string.volume), vol, 0f..1f, "${(vol * 100).toInt()} %", { vol = it }, { vm.setSound(vol, s.muted) })
        },
        SettingItem(3, stringResource(R.string.mute), "sound mute silence") {
            SwitchRow(stringResource(R.string.mute), s.muted, { vm.setSound(s.volume, it) })
        },
        SettingItem(4, stringResource(R.string.reduce_motion), "motion animation accessibility") {
            SwitchRow(stringResource(R.string.reduce_motion), ui.reduceMotion, vm::setReduceMotion, help = stringResource(R.string.reduce_motion_help))
        },
        SettingItem(5, stringResource(R.string.discovery_title), "network discovery mdns privacy") {
            SwitchRow(stringResource(R.string.discovery_title), ui.discoveryOptIn, vm::setDiscovery, help = stringResource(R.string.discovery_help))
        },
        SettingItem(5, stringResource(R.string.map_tiles), "map tiles online openstreetmap privacy") {
            Dropdown(consent, s.mapConsent.coerceIn(0, 2), { vm.setMapConsent(it) }, label = stringResource(R.string.map_tiles))
        },
        SettingItem(6, stringResource(R.string.run_setup), "setup wizard first run lessons help") { LinkRow(OnAirIcons.Glyph, stringResource(R.string.run_setup), onRunSetup) },
        SettingItem(6, stringResource(R.string.verbose_log), "verbose logging debug usb diagnostics advanced") {
            SwitchRow(stringResource(R.string.verbose_log), s.verboseLog, vm::setVerbose, help = stringResource(R.string.verbose_log_help))
        },
        SettingItem(6, stringResource(R.string.log_title), "log crash debug share") { LinkRow(OnAirIcons.Log, stringResource(R.string.log_title), onOpenLog) },
        SettingItem(6, stringResource(R.string.about_title), "about credits licences supporters version") { LinkRow(OnAirIcons.Info, stringResource(R.string.about_title), onOpenAbout) },
    )
    val shown = items.filter { it.matches(query) }
    Column(modifier.fillMaxSize().background(p.window)) {
        Row(Modifier.padding(horizontal = 12.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            TextBox(query, { query = it }, Modifier.weight(1f), placeholder = stringResource(R.string.search_settings))
            if (query.isNotEmpty()) OnAirIconButton(OnAirIcons.Close, stringResource(R.string.cd_clear_search), framed = false) { query = "" }
        }
        if (shown.isEmpty()) Text(stringResource(R.string.no_settings_match), Modifier.padding(16.dp), color = p.muted, fontFamily = LocalTextFamily.current)
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 12.dp)) {
            var last = -1
            for (si in shown) {
                if (si.group != last) {
                    val g = si.group
                    item(key = "g$g") { GroupHeader(groups[g], p.series.getOrElse(g) { p.accent }) }
                    last = g
                }
                item(key = "s" + si.group + si.title) { si.content() }
            }
            item { Spacer(Modifier.height(24.dp)) }
        }
    }
}

@Composable
private fun LinkRow(icon: ImageVector, title: String, onClick: () -> Unit) {
    val p = LocalPalette.current
    Row(
        Modifier.fillMaxWidth().heightIn(min = LocalSpacing.current.touch).clickable(role = Role.Button, onClick = onClick).padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, contentDescription = null, tint = p.muted, modifier = Modifier.size(20.dp))
        Text(title, Modifier.padding(start = 12.dp).weight(1f), color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
        Icon(OnAirIcons.ChevronRight, contentDescription = null, tint = p.muted, modifier = Modifier.size(18.dp))
    }
}
