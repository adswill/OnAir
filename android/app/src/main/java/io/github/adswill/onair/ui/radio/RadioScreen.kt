package io.github.adswill.onair.ui.radio

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.BuildConfig
import io.github.adswill.onair.CheckRow
import io.github.adswill.onair.Dropdown
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.OnAirIconButton
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.PanelButton
import io.github.adswill.onair.R
import io.github.adswill.onair.Section
import io.github.adswill.onair.SectionLabel
import io.github.adswill.onair.SliderRow
import io.github.adswill.onair.SwitchRow
import io.github.adswill.onair.TextBox
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.model.RadioOptions
import io.github.adswill.onair.model.RadioSetting
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.ui.NetworkRadio
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.UiHooks
import kotlinx.coroutines.delay
import java.util.Locale

const val DRIVER_PACKAGE = "marto.rtl_tcp_andro"

internal fun openUrl(ctx: Context, vararg urls: String) {
    for (u in urls) {
        try { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(u)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); return } catch (_: ActivityNotFoundException) {}
    }
}

private fun openDriver(ctx: Context, port: Int): Boolean = try {
    ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("iqsrc://-a 127.0.0.1 -p $port -n 1")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); true
} catch (_: ActivityNotFoundException) { false }

private fun rateText(hz: Double) = String.format(Locale.US, "%.3f", hz / 1e6).trimEnd('0').trimEnd('.') + " Msps"

/**
 * The radio (source) picker: test signal, USB radios (from the USB layer through [UiHooks.usb]), saved network radios with
 * "Add a network radio" (discovery is opt-in), IQ files; then the chosen source's own settings and the tuner.
 */
@Composable
fun RadioScreen(vm: ReceiverViewModel, pickFile: () -> Unit, modifier: Modifier = Modifier) {
    val st by vm.state.collectAsStateWithLifecycle()
    val s by vm.settings.collectAsStateWithLifecycle()
    val ui by vm.ui.collectAsStateWithLifecycle()
    var adding by remember { mutableStateOf(false) }
    Column(modifier.verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 4.dp)) {
        SectionLabel(stringResource(R.string.section_source))
        SourceRow(OnAirIcons.Wave, stringResource(R.string.source_synth), stringResource(R.string.source_synth_short), s.source == SourceKind.TEST_SIGNAL) { vm.chooseSource(SourceKind.TEST_SIGNAL) }
        UsbSection()
        for (r in ui.networkRadios) {
            val sel = s.source == r.kind && endpointOf(s.source, s.rtlHost, s.rtlPort, s.spyHost, s.spyPort) == (r.host to r.port)
            SourceRow(
                OnAirIcons.Network, r.name.ifBlank { r.host }, kindName(r.kind) + "  " + r.host + ":" + r.port, sel,
                trailing = { OnAirIconButton(OnAirIcons.Close, stringResource(R.string.cd_remove_radio, r.name.ifBlank { r.host }), framed = false) { vm.removeNetworkRadio(r) } },
            ) { vm.useNetworkRadio(r) }
        }
        val unsaved = s.source.isNetwork && ui.networkRadios.none { it.kind == s.source && (it.host to it.port) == endpointOf(s.source, s.rtlHost, s.rtlPort, s.spyHost, s.spyPort) }
        if (unsaved) SourceRow(OnAirIcons.Network, kindName(s.source), stringResource(R.string.network_unsaved), true) {}
        SourceRow(OnAirIcons.File, stringResource(R.string.source_file), st.file?.name ?: stringResource(R.string.no_file), s.source == SourceKind.IQ_FILE) { vm.chooseSource(SourceKind.IQ_FILE) }
        Spacer(Modifier.height(6.dp))
        PanelButton(stringResource(R.string.add_network_radio), modifier = Modifier.fillMaxWidth()) { adding = true }

        SectionLabel(stringResource(R.string.section_selected_source))
        Section {
            when (s.source) {
                SourceKind.TEST_SIGNAL -> Label(stringResource(R.string.source_synth_note))
                SourceKind.RTL_TCP -> {
                    Endpoint(vm, SourceKind.RTL_TCP, s.rtlHost, s.rtlPort)
                    Spacer(Modifier.height(6.dp))
                    Label(stringResource(R.string.rtltcp_hint))
                    DriverButtons(s.rtlPort)
                }
                SourceKind.SPYSERVER -> {
                    Endpoint(vm, SourceKind.SPYSERVER, s.spyHost, s.spyPort)
                    Spacer(Modifier.height(6.dp))
                    Label(stringResource(R.string.spy_hint))
                }
                SourceKind.IQ_FILE -> {
                    PanelButton(stringResource(R.string.choose_file), modifier = Modifier.fillMaxWidth()) { pickFile() }
                    Spacer(Modifier.height(4.dp))
                    Label(st.file?.name ?: stringResource(R.string.no_file), color = LocalPalette.current.text)
                    Spacer(Modifier.height(6.dp))
                    val fmts = listOf(R.string.format_cs8, R.string.format_cu8, R.string.format_cf32, R.string.format_ci16, R.string.format_cu16)
                    Dropdown(fmts.map { stringResource(it) }, s.fileFormat.code, { vm.setFileFormat(it) }, label = stringResource(R.string.file_format))
                    Spacer(Modifier.height(6.dp))
                    var rateStr by remember(s.fileRateHz) { mutableStateOf(String.format(Locale.US, "%.4f", s.fileRateHz / 1e6).trimEnd('0').trimEnd('.')) }
                    TextBox(rateStr, { rateStr = it; it.replace(',', '.').toDoubleOrNull()?.let { v -> if (v > 0) vm.setFileRate(v * 1e6) } }, numeric = true, label = stringResource(R.string.file_rate))
                    Spacer(Modifier.height(4.dp))
                    Label(stringResource(R.string.file_hint))
                }
                else -> {}   // USB, network list and sound card: chosen in the rows above, their settings are in the USB and network radio sections
            }
        }

        if (s.source != SourceKind.IQ_FILE) {
            SectionLabel(stringResource(R.string.section_tuner))
            Section {
                val rates = remember(s.source) {
                    when (s.source) {
                        SourceKind.RTL_TCP -> listOf(0.25e6, 0.9e6, 1.024e6, 1.4e6, 1.8e6, 2.048e6, 2.4e6, 2.56e6)
                        SourceKind.SPYSERVER -> listOf(0.5e6, 1e6, 2e6, 4e6, 6e6, 10e6)
                        else -> listOf(0.5e6, 1e6, 2e6, 2.4e6, 4e6, 8e6, 10e6, 20e6)
                    }
                }
                val agcGain by vm.agcGain.collectAsStateWithLifecycle()
                val gmax = if (st.caps.gainMax > 0) st.caps.gainMax else 50.0
                // the slider moves a local value; the gain is set when the finger lifts. With AGC on it follows the AGC's choice.
                val shown = if (s.agc && !agcGain.isNaN()) agcGain else s.gainDb
                var gain by remember(shown) { mutableFloatStateOf(shown.toFloat()) }
                SliderRow(
                    stringResource(R.string.gain), gain.coerceIn(0f, gmax.toFloat()), 0f..gmax.toFloat(),
                    stringResource(R.string.gain_db, String.format(Locale.US, "%.0f", gain)),
                    { gain = it }, { vm.setGain(gain.toDouble(), s.agc) },
                )
                CheckRow(stringResource(R.string.agc), s.agc) { vm.setGain(gain.toDouble(), it) }
                if (st.caps.biasTee) CheckRow(stringResource(R.string.bias_tee), s.biasTee) { vm.setBiasTee(it) }
                // the sample rate and the radio's own settings are chosen automatically; a chosen rate keeps this section open
                var advanced by rememberSaveable(s.source) { mutableStateOf(s.rateHz > 0) }
                CheckRow(stringResource(R.string.section_advanced), advanced) { advanced = it }
                if (advanced) {
                    val items = listOf(stringResource(R.string.rate_auto)) + rates.map { rateText(it) }
                    val sel = if (s.rateHz <= 0) 0 else rates.indexOfFirst { it == s.rateHz } + 1
                    Dropdown(items, sel.coerceAtLeast(0), { i -> vm.setRate(if (i == 0) 0.0 else rates[i - 1]) }, label = stringResource(R.string.rate))
                    Spacer(Modifier.height(4.dp))
                    val radio = s.radioFor(s.source)
                    for (rs in st.caps.settings) SettingRow(vm, s.source, radio, rs)
                }
            }
        }
        Spacer(Modifier.height(24.dp))
    }
    if (adding) AddNetworkRadioDialog(
        discoveryOn = ui.discoveryOptIn, onDiscovery = vm::setDiscovery, onDismiss = { adding = false },
    ) { r -> vm.addNetworkRadio(r); vm.useNetworkRadio(r); adding = false }
}

private fun endpointOf(kind: SourceKind, rh: String, rp: Int, sh: String, sp: Int): Pair<String, Int> =
    if (kind == SourceKind.SPYSERVER) sh to sp else rh to rp

@Composable
private fun kindName(k: SourceKind): String = stringResource(if (k == SourceKind.SPYSERVER) R.string.source_spy else R.string.source_rtltcp)

/** One selectable source: icon, name, a line of detail, the selected mark; the whole row is the target (radio-button semantics). */
@Composable
fun SourceRow(icon: ImageVector, title: String, detail: String, selected: Boolean, trailing: (@Composable () -> Unit)? = null, onClick: () -> Unit) {
    val p = LocalPalette.current
    val shape = RoundedCornerShape(4.dp)
    Row(
        Modifier.fillMaxWidth().padding(vertical = 3.dp).clip(shape).background(if (selected) p.panel else p.window)
            .border(1.dp, if (selected) p.accent else p.border, shape)
            .selectable(selected, role = Role.RadioButton, onClick = onClick).heightIn(min = 56.dp).padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, contentDescription = null, tint = if (selected) p.accent else p.muted, modifier = Modifier.size(22.dp))
        Column(Modifier.weight(1f).padding(horizontal = 10.dp, vertical = 6.dp)) {
            Text(title, color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.BODY_LARGE.sp)
            Text(detail, color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp)
        }
        if (selected) Icon(OnAirIcons.Check, contentDescription = null, tint = p.accent, modifier = Modifier.size(20.dp))
        trailing?.invoke()
    }
}

/** USB radios from the USB layer, or a short note while this build has none. */
@Composable
private fun UsbSection() {
    val usb = UiHooks.usb
    if (usb == null) {
        SourceRow(OnAirIcons.Usb, stringResource(R.string.source_usb), stringResource(R.string.usb_not_in_build), false) {}
        return
    }
    val devices by usb.devices.collectAsState()
    LaunchedEffect(usb) { usb.refresh() }
    if (devices.isEmpty()) SourceRow(OnAirIcons.Usb, stringResource(R.string.source_usb), stringResource(R.string.usb_none), false) { usb.refresh() }
    for (d in devices) SourceRow(
        OnAirIcons.Usb, d.name, d.detail.ifBlank { stringResource(if (d.permitted) R.string.usb_ready else R.string.usb_needs_permission) }, d.selected,
    ) { if (d.permitted) usb.select(d.id) else usb.requestPermission(d.id) }
}

@Composable
private fun DriverButtons(port: Int) {
    val ctx = LocalContext.current
    val missing = stringResource(R.string.driver_missing)
    Spacer(Modifier.height(6.dp))
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        PanelButton(stringResource(R.string.open_play_store)) { openUrl(ctx, "market://details?id=$DRIVER_PACKAGE", "https://play.google.com/store/apps/details?id=$DRIVER_PACKAGE") }
        PanelButton(stringResource(R.string.open_fdroid)) { openUrl(ctx, "https://f-droid.org/packages/$DRIVER_PACKAGE/") }
        PanelButton(stringResource(R.string.open_driver)) { if (!openDriver(ctx, port)) Toast.makeText(ctx, missing, Toast.LENGTH_SHORT).show() }
    }
}

/** Host and port of a network source. Typed text is UI state; it is stored (and the source asked again) a moment after typing stops. */
@Composable
private fun Endpoint(vm: ReceiverViewModel, kind: SourceKind, host: String, port: Int) {
    var h by remember(kind) { mutableStateOf(host) }
    var pt by remember(kind) { mutableStateOf(port.toString()) }
    LaunchedEffect(kind, h, pt) {
        val pn = pt.toIntOrNull() ?: return@LaunchedEffect
        if (h == host && pn == port) return@LaunchedEffect
        delay(600)
        vm.setEndpoint(kind, h.trim(), pn)
        vm.refreshCaps()
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        TextBox(h, { h = it }, Modifier.weight(2f), label = stringResource(R.string.host))
        TextBox(pt, { pt = it.filter(Char::isDigit).take(5) }, Modifier.weight(1f).widthIn(min = 72.dp), numeric = true, label = stringResource(R.string.port))
    }
}

@Composable
private fun SettingRow(vm: ReceiverViewModel, kind: SourceKind, radio: RadioOptions, s: RadioSetting) {
    when (s.type) {
        "bool" -> CheckRow(s.label, radio.value(s.key, s.def) == "1") { vm.setRadioSetting(s.key, if (it) "1" else "0") }
        "choice" -> {
            val cur = radio.value(s.key, s.def)
            Spacer(Modifier.height(4.dp))
            Dropdown(s.names.ifEmpty { s.values }, s.values.indexOf(cur).coerceAtLeast(0), { vm.setRadioSetting(s.key, s.values[it]) }, label = s.label)
        }
        else -> {
            var text by remember(s.key, kind) { mutableStateOf(radio.value(s.key, s.def)) }
            Spacer(Modifier.height(4.dp))
            TextBox(text, { text = it; if (it.toDoubleOrNull() != null) vm.setRadioSetting(s.key, it) }, numeric = true, label = s.label + if (s.unit.isNotEmpty()) " (" + s.unit + ")" else "")
        }
    }
}

/**
 * The desktop's "Add a network radio" dialog: kind, name, address, port. With the user's consent it also lists servers that announce
 * themselves on the local network (mDNS); tapping one fills the fields.
 */
@Composable
fun AddNetworkRadioDialog(discoveryOn: Boolean, onDiscovery: (Boolean) -> Unit, onDismiss: () -> Unit, onAdd: (NetworkRadio) -> Unit) {
    val p = LocalPalette.current
    var kind by remember { mutableStateOf(SourceKind.RTL_TCP) }
    var name by remember { mutableStateOf("") }
    var host by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("1234") }
    val found by rememberDiscovery(discoveryOn)
    val portN = port.toIntOrNull()?.takeIf { it in 1..65535 }
    val ok = host.isNotBlank() && portN != null
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.add_network_radio)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                val kinds = listOf(SourceKind.RTL_TCP, SourceKind.SPYSERVER)
                Dropdown(kinds.map { kindName(it) }, kinds.indexOf(kind), {
                    val was = kind
                    kind = kinds[it]
                    if (was != kind && (port == "1234" || port == "5555")) port = if (kind == SourceKind.SPYSERVER) "5555" else "1234"
                }, label = stringResource(R.string.network_kind))
                Spacer(Modifier.height(6.dp))
                TextBox(name, { name = it }, label = stringResource(R.string.network_name), placeholder = stringResource(R.string.network_name_hint))
                Spacer(Modifier.height(6.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextBox(host, { host = it.trim() }, Modifier.weight(2f), label = stringResource(R.string.host), placeholder = "192.168.1.20")
                    TextBox(port, { port = it.filter(Char::isDigit).take(5) }, Modifier.weight(1f), numeric = true, label = stringResource(R.string.port))
                }
                Spacer(Modifier.height(8.dp))
                SwitchRow(stringResource(R.string.discovery_title), discoveryOn, onDiscovery, help = stringResource(R.string.discovery_help))
                if (discoveryOn) {
                    if (found.isEmpty()) Label(stringResource(R.string.discovery_searching))
                    for (r in found) SourceRow(OnAirIcons.Network, r.name, kindName(r.kind) + "  " + r.host + ":" + r.port, r.host == host && portN == r.port) {
                        kind = r.kind; name = r.name; host = r.host; port = r.port.toString()
                    }
                }
                Label(stringResource(R.string.network_privacy), Modifier.padding(top = 6.dp), color = p.muted)
            }
        },
        confirmButton = { TextButton({ if (ok) onAdd(NetworkRadio(kind, name.trim(), host.trim(), portN!!)) }, enabled = ok) { Text(stringResource(R.string.add)) } },
        dismissButton = { TextButton(onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

/** The build's version, for screens that show it. */
fun appVersion(core: String): String = core.ifEmpty { BuildConfig.VERSION_NAME }
