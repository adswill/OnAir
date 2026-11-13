// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.receiver

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.Lamp
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalSpacing
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.MeterBar
import io.github.adswill.onair.Modes
import io.github.adswill.onair.Mono
import io.github.adswill.onair.OnAirIconButton
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.R
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.data.Phase
import io.github.adswill.onair.data.ReceiverState
import io.github.adswill.onair.groupAccent
import io.github.adswill.onair.model.LockState
import io.github.adswill.onair.model.Telemetry
import io.github.adswill.onair.ui.AppLayout
import io.github.adswill.onair.ui.LocalAppLayout
import io.github.adswill.onair.ui.Meters
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.UiHooks
import io.github.adswill.onair.ui.panels.PanelHost
import java.util.Locale

/**
 * The main receiver screen. Phone portrait: header, spectrum + waterfall, the sound strip, then the mode panels under a draggable
 * split. Landscape and tablets: two panes, spectrum left and panels right ([AppLayout.twoPane]).
 */
@Composable
fun ReceiverScreen(vm: ReceiverViewModel, onModeClick: () -> Unit, onToggle: () -> Unit, modifier: Modifier = Modifier, showPanels: Boolean = true) {
    val layout = LocalAppLayout.current
    val p = LocalPalette.current
    val ui by vm.ui.collectAsStateWithLifecycle()
    val sheet by vm.settings.collectAsStateWithLifecycle()
    val spectrum = @Composable { m: Modifier -> SpectrumWaterfall(vm, ui.centreTuning, m, onToggleCentre = { vm.setCentreTuning(!ui.centreTuning) }) }
    if (layout.twoPane) {
        Row(modifier.fillMaxSize().background(p.window)) {
            Column(Modifier.weight(0.58f).fillMaxHeight()) {
                ReceiverHeader(vm, compact = layout.shortHeight, onModeClick = onModeClick, onToggle = onToggle)
                spectrum(Modifier.weight(1f).fillMaxWidth())
                SoundStrip(vm)
            }
            if (showPanels) {
                Box(Modifier.width(1.dp).fillMaxHeight().background(p.border))
                PanelHost(vm, Modifier.weight(0.42f).fillMaxHeight())
            }
        }
    } else {
        Column(modifier.fillMaxSize().background(p.window)) {
            ReceiverHeader(vm, compact = false, onModeClick = onModeClick, onToggle = onToggle)
            if (!showPanels) {
                spectrum(Modifier.weight(1f).fillMaxWidth())
                SoundStrip(vm)
            } else BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                val total = constraints.maxHeight.toFloat().coerceAtLeast(1f)
                var fraction by remember(sheet.sheetFraction) { mutableFloatStateOf(sheet.sheetFraction.coerceIn(0.15f, 0.75f)) }
                Column(Modifier.fillMaxSize()) {
                    spectrum(Modifier.weight(1f - fraction).fillMaxWidth())
                    SoundStrip(vm)
                    val handle = stringResource(R.string.cd_split_handle)
                    Box(
                        Modifier.fillMaxWidth().height(20.dp).background(p.window)
                            .semantics { contentDescription = handle }
                            .pointerInput(total) {
                                detectVerticalDragGestures(onDragEnd = { vm.setSheetFraction(fraction) }) { _, dy -> fraction = (fraction - dy / total).coerceIn(0.15f, 0.75f) }
                            },
                        contentAlignment = Alignment.Center,
                    ) { Box(Modifier.width(40.dp).height(4.dp).clip(RoundedCornerShape(2.dp)).background(p.border)) }
                    PanelHost(vm, Modifier.weight(fraction).fillMaxWidth())
                }
            }
        }
    }
}

/** The header: frequency readout, mode chip, lock lamp and state, SNR, meters, and the run button. */
@Composable
fun ReceiverHeader(vm: ReceiverViewModel, compact: Boolean, onModeClick: () -> Unit, onToggle: () -> Unit) {
    val st by vm.state.collectAsStateWithLifecycle()
    val t by vm.telemetry.collectAsStateWithLifecycle()
    val ui by vm.ui.collectAsStateWithLifecycle()
    val meters by vm.meters.collectAsStateWithLifecycle()
    val names = stringArrayResource(R.array.mode_names)
    // only the lock state and SNR of the telemetry matter here: a derived value, so a new report with the same lock does not recompose
    val lock by remember { derivedStateOf { t?.lock } }
    val snr by remember { derivedStateOf { t?.snrDb?.let { Math.round(it * 10) / 10.0 } } }
    Column {
        HeaderContent(
            st = st, modeName = names.getOrElse(st.modeIndex) { "" }, lock = lock, snrDb = snr, meters = meters, stepHz = ui.stepHz,
            compact = compact, onTune = vm::tune, onStepHz = vm::setStepHz, onModeClick = onModeClick, onToggle = onToggle,
        )
        RecordIqBar(vm)
    }
}

/**
 * The overflow menu of the receiver (the header is the same in every mode): Record IQ / Stop recording, Share recording, Save to
 * Downloads, the size limit; and while recording a line with the time and the size against the limit. Recording only starts when
 * the user asks; the file stays on the phone (the app's own folder) until the user shares it.
 */
@Composable
fun RecordIqBar(vm: ReceiverViewModel) {
    val p = LocalPalette.current
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val st by vm.state.collectAsStateWithLifecycle()
    val rec by vm.rec.collectAsStateWithLifecycle()
    var open by remember { mutableStateOf(false) }
    var capMb by remember { mutableStateOf(vm.recordingCapMb()) }
    var toast by remember { mutableStateOf("") }
    val last = if (open) vm.lastRecording() else null
    Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        val msg = when {
            rec.active -> stringResource(R.string.iq_recording_status, io.github.adswill.onair.data.IqRecording.clock(rec.seconds),
                io.github.adswill.onair.data.IqRecording.size(rec.bytes), io.github.adswill.onair.data.IqRecording.size(capMb * 1048576L))
            toast.isNotEmpty() -> toast
            rec.error.isNotEmpty() -> rec.error
            else -> ""
        }
        Text(
            if (rec.active && rec.nearCap) msg + "  -  " + stringResource(R.string.iq_near_cap) else msg,
            color = if (rec.active) (if (rec.nearCap) p.warn else p.bad) else p.muted, fontFamily = Mono, fontSize = TypeScale.SMALL.sp,
            modifier = Modifier.weight(1f).semantics { if (rec.active) stateDescription = msg },
        )
        Box {
            OnAirIconButton(if (rec.active) OnAirIcons.Record else OnAirIcons.More, stringResource(if (rec.active) R.string.iq_stop else R.string.iq_menu), framed = false,
                tint = if (rec.active) p.bad else p.text) {
                if (rec.active) vm.stopRecording() else { toast = ""; capMb = vm.recordingCapMb(); open = true }
            }
            androidx.compose.material3.DropdownMenu(open, { open = false }) {
                androidx.compose.material3.DropdownMenuItem(
                    text = { Text(stringResource(R.string.iq_record)) }, enabled = st.running,
                    onClick = { open = false; vm.startRecording() },
                )
                if (!st.running) Text(stringResource(R.string.iq_needs_running), color = p.muted, fontSize = TypeScale.SMALL.sp, modifier = Modifier.padding(horizontal = 16.dp))
                androidx.compose.material3.DropdownMenuItem(
                    text = { Text(stringResource(R.string.iq_share)) }, enabled = last != null,
                    onClick = { open = false; last?.let { ctx.startActivity(io.github.adswill.onair.data.IqFiles.shareIntent(ctx, it)) } },
                )
                androidx.compose.material3.DropdownMenuItem(
                    text = { Text(stringResource(R.string.iq_save_downloads)) }, enabled = last != null,
                    onClick = {
                        open = false
                        toast = ctx.getString(if (last != null && io.github.adswill.onair.data.IqFiles.saveToDownloads(ctx, last)) R.string.iq_saved else R.string.iq_save_failed)
                    },
                )
                val next = io.github.adswill.onair.data.IqRecording.capChoicesMb.let { c -> c[(c.indexOf(capMb) + 1) % c.size] }
                androidx.compose.material3.DropdownMenuItem(
                    text = { Text(stringResource(R.string.iq_cap_title) + ": " + io.github.adswill.onair.data.IqRecording.size(capMb * 1048576L)) },
                    onClick = { vm.setRecordingCapMb(next); capMb = next },
                )
                Text(stringResource(R.string.iq_privacy), color = p.muted, fontSize = TypeScale.CAPTION.sp, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp).widthIn(max = 260.dp))
            }
        }
    }
}

@Composable
fun HeaderContent(
    st: ReceiverState, modeName: String, lock: LockState?, snrDb: Double?, meters: Meters, stepHz: Double, compact: Boolean,
    onTune: (Double) -> Unit, onStepHz: (Double) -> Unit, onModeClick: () -> Unit, onToggle: () -> Unit,
) {
    val p = LocalPalette.current
    val running = st.running
    val lamp = when {
        !running -> p.border
        lock == LockState.LOCKED -> p.ok
        lock == LockState.CONVERGING -> p.warn
        else -> p.warn.copy(alpha = 0.55f)
    }
    val stateText = when {
        st.phase == Phase.STARTING -> R.string.state_starting
        st.phase == Phase.STOPPING -> R.string.state_stopping
        !running -> R.string.state_off
        lock == LockState.LOCKED -> R.string.state_locked
        lock == LockState.CONVERGING -> R.string.state_converging
        else -> R.string.state_searching
    }
    val accent = groupAccent(st.mode.group)
    Column(Modifier.fillMaxWidth().background(p.window).padding(horizontal = 4.dp, vertical = if (compact) 0.dp else 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            FrequencyDisplay(
                st.freqMhz, stepHz, autoStepHz(st.channelHz()), onTune, onStepHz, Modifier.weight(1f), maxSize = if (compact) 26 else 34,
            )
            RunButton(st.active, onToggle)
        }
        Row(
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.padding(horizontal = 8.dp),
        ) {
            Chip(modeName, color = accent, selected = true, onClick = onModeClick)
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.semantics(mergeDescendants = true) {}) {
                Lamp(lamp, 10)
                Text(" " + stringResource(stateText), color = p.muted, fontFamily = LocalTextFamily.current, fontSize = TypeScale.SMALL.sp)
            }
            if (running && snrDb != null) Text(
                stringResource(R.string.snr_db, String.format(Locale.US, "%.1f", snrDb)), color = p.muted, fontFamily = Mono, fontSize = TypeScale.SMALL.sp,
            )
        }
        if (running) MetersRow(meters, Modifier.padding(horizontal = 8.dp, vertical = 2.dp))
    }
}

/** Noise floor and ADC level (with the clip warning), like the desktop's status bar meters. */
@Composable
fun MetersRow(m: Meters, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
        val nf = if (m.noiseFloorDb.isNaN()) "-" else String.format(Locale.US, "%.0f dB", m.noiseFloorDb)
        Label(stringResource(R.string.noise_floor) + " " + nf, size = TypeScale.CAPTION)
        val adc = m.adcDb
        val frac = if (adc.isNaN()) 0f else ((adc + 60f) / 60f).coerceIn(0f, 1f)       // -60 .. 0 dBFS
        val color = when { m.clip > 0.001f || frac > 0.95f -> p.bad; frac > 0.8f -> p.warn; else -> p.ok }
        MeterBar(
            stringResource(R.string.adc), frac,
            if (adc.isNaN()) "-" else String.format(Locale.US, "%.0f dBFS", adc) + if (m.clip > 0.001f) " " + stringResource(R.string.clipping) else "",
            color, Modifier.weight(1f),
        )
    }
}

/** The run / stop button: 56 dp, green play or red stop, as the desktop's toolbar. */
@Composable
fun RunButton(active: Boolean, onToggle: () -> Unit) {
    val p = LocalPalette.current
    val desc = stringResource(if (active) R.string.stop else R.string.run)
    Box(
        Modifier.padding(4.dp).size(56.dp).clip(RoundedCornerShape(4.dp)).background(p.button).border(1.dp, p.border, RoundedCornerShape(4.dp))
            .clickable(role = Role.Button, onClickLabel = desc, onClick = onToggle).semantics { contentDescription = desc },
        contentAlignment = Alignment.Center,
    ) { Icon(if (active) OnAirIcons.Stop else OnAirIcons.Play, contentDescription = null, tint = if (active) p.bad else p.ok, modifier = Modifier.size(28.dp)) }
}

/** Volume, mute, squelch (general receiver) and record: the controls a listener needs without leaving the spectrum. */
@Composable
fun SoundStrip(vm: ReceiverViewModel) {
    val s by vm.settings.collectAsStateWithLifecycle()
    val st by vm.state.collectAsStateWithLifecycle()
    val general = ReceiverState.telemetryStdOf(st.mode.family) == Telemetry.STD_GENERAL
    val rec = UiHooks.recorder
    val recording = rec?.recording?.collectAsState()?.value ?: false
    val recSeconds = rec?.seconds?.collectAsState()?.value ?: 0
    SoundStripContent(
        volume = s.volume, muted = s.muted, onSound = vm::setSound,
        squelchShown = general, squelchOn = s.genSquelch, squelchAuto = s.genSquelchAuto, squelchDb = s.genSquelchDb,
        onSquelch = { on, db -> vm.setGeneral(s.genDemod, on, s.genSquelchAuto && db == s.genSquelchDb, db) },
        recordAvailable = rec != null, recording = recording, recordSeconds = recSeconds, onRecord = { rec?.toggle() },
    )
}

@Composable
fun SoundStripContent(
    volume: Float, muted: Boolean, onSound: (Float, Boolean) -> Unit,
    squelchShown: Boolean, squelchOn: Boolean, squelchAuto: Boolean, squelchDb: Float, onSquelch: (Boolean, Float) -> Unit,
    recordAvailable: Boolean, recording: Boolean, recordSeconds: Int, onRecord: () -> Unit,
) {
    val p = LocalPalette.current
    var vol by remember(volume) { mutableFloatStateOf(volume) }
    var sq by remember(squelchDb) { mutableFloatStateOf(squelchDb) }
    val volDesc = stringResource(R.string.volume)
    val sqDesc = stringResource(R.string.general_squelch)
    val sliderColors = SliderDefaults.colors(thumbColor = p.accent, activeTrackColor = p.accent, inactiveTrackColor = p.border)
    Row(
        Modifier.fillMaxWidth().background(p.window).heightIn(min = LocalSpacing.current.touch).padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OnAirIconButton(
            if (muted) OnAirIcons.SpeakerOff else OnAirIcons.Speaker, stringResource(if (muted) R.string.cd_unmute else R.string.cd_mute),
            tint = if (muted) p.warn else p.text, framed = false,
        ) { onSound(vol, !muted) }
        Slider(
            vol, { vol = it }, onValueChangeFinished = { onSound(vol, muted) }, valueRange = 0f..1f, colors = sliderColors,
            modifier = Modifier.weight(1f).semantics { contentDescription = volDesc; stateDescription = "${(vol * 100).toInt()} %" },
        )
        if (squelchShown) {
            Spacer(Modifier.width(4.dp))
            OnAirIconButton(
                OnAirIcons.Squelch, stringResource(if (squelchOn) R.string.cd_squelch_off else R.string.cd_squelch_on),
                tint = if (squelchOn) p.accent else p.muted, on = squelchOn, framed = false,
            ) { onSquelch(!squelchOn, sq) }
            Slider(
                sq, { sq = it }, onValueChangeFinished = { onSquelch(squelchOn, sq) }, valueRange = -120f..0f, enabled = squelchOn, colors = sliderColors,
                modifier = Modifier.weight(0.8f).semantics { contentDescription = sqDesc; stateDescription = String.format(Locale.US, "%.0f dB", sq) + if (squelchAuto) " auto" else "" },
            )
        }
        val recDesc = stringResource(
            if (!recordAvailable) R.string.cd_record_unavailable else if (recording) R.string.cd_record_stop else R.string.cd_record_start,
        )
        OnAirIconButton(OnAirIcons.Record, recDesc, tint = if (recording) p.bad else p.muted, enabled = recordAvailable, framed = false, onClick = onRecord)
        if (recording) Text(String.format(Locale.US, "%d:%02d", recordSeconds / 60, recordSeconds % 60), color = p.bad, fontFamily = Mono, fontSize = TypeScale.SMALL.sp)
    }
}

/** For previews: a mode's accent by index. */
internal fun modeAccent(idx: Int): Color = groupAccent(Modes.all.getOrNull(idx)?.group ?: 0)
