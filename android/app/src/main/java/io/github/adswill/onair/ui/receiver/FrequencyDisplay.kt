// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.receiver

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.github.adswill.onair.Chip
import io.github.adswill.onair.Label
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalSpacing
import io.github.adswill.onair.Mono
import io.github.adswill.onair.OnAirIconButton
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.R
import io.github.adswill.onair.TextBox
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.ui.nav.DeepLink
import java.util.Locale
import kotlin.math.roundToLong

/** The step choices of the step chip (Hz); 0 = automatic, from the mode's channel. */
val StepChoices = doubleArrayOf(0.0, 1.0, 10.0, 100.0, 1e3, 5e3, 9e3, 10e3, 12.5e3, 25e3, 100e3, 1e6)

/** The automatic step for a channel [channelHz] wide: about half a channel, from the usual raster. */
fun autoStepHz(channelHz: Double): Double {
    if (channelHz <= 0) return 1e3
    val target = channelHz / 2
    return StepChoices.filter { it > 0 && it <= target }.maxOrNull() ?: 1.0
}

fun stepText(hz: Double): String = when {
    hz >= 1e6 -> String.format(Locale.US, "%.0f MHz", hz / 1e6)
    hz >= 1e3 -> String.format(Locale.US, "%.1f kHz", hz / 1e3).replace(".0 ", " ")
    else -> String.format(Locale.US, "%.0f Hz", hz)
}

/** The readout as the desktop draws it: 12 digits in groups of three, the leading zero groups dimmed, the MHz digits bright. */
fun freqDigits(mhz: Double, dim: Color, bright: Color): AnnotatedString {
    val hz = (mhz * 1e6).roundToLong().coerceAtLeast(0)
    val digits = String.format(Locale.US, "%012d", hz)
    val s = digits.chunked(3).joinToString(".")
    val firstLit = digits.indexOfFirst { it != '0' }.let { if (it < 0) 11 else minOf(it, 5) }   // the MHz units digit and below stay lit
    return buildAnnotatedString {
        var d = 0
        for (c in s) {
            if (c == '.') { withStyle(SpanStyle(color = if (d <= firstLit) dim else bright)) { append(c) }; continue }
            withStyle(SpanStyle(color = if (d < firstLit) dim else bright)) { append(c) }
            d++
        }
    }
}

/** Speakable frequency: "100.5 megahertz" style for TalkBack. */
fun freqSpoken(mhz: Double): String = String.format(Locale.US, "%.6f", mhz).trimEnd('0').trimEnd('.') + " MHz"

/**
 * The big frequency readout with step buttons: tap the digits to type a frequency (100.5, 100.5M, 7074k, 1.09G), − / + move by
 * the step, the step chip picks the step. The digits shrink to fit the width (also at 200 % text size) instead of wrapping.
 */
@Composable
fun FrequencyDisplay(
    freqMhz: Double, stepHz: Double, autoStep: Double, onTune: (Double) -> Unit, onStepHz: (Double) -> Unit,
    modifier: Modifier = Modifier, maxSize: Int = 34,
) {
    val p = LocalPalette.current
    var editing by remember { mutableStateOf(false) }
    var stepMenu by remember { mutableStateOf(false) }
    val step = if (stepHz > 0) stepHz else autoStep
    val spoken = freqSpoken(freqMhz)
    val editDesc = stringResource(R.string.cd_edit_frequency, spoken)
    Column(modifier) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            OnAirIconButton(OnAirIcons.Minus, stringResource(R.string.cd_step_down, stepText(step)), framed = false) { onTune(freqMhz - step / 1e6) }
            BoxWithConstraints(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                val density = LocalDensity.current
                // 15 characters (12 digits, 3 dots) of a monospace font are about 0.6 em each
                val fitPx = constraints.maxWidth / (15 * 0.62f)
                val size = with(density) { minOf(maxSize.sp.toPx(), fitPx).toSp() }
                Text(
                    freqDigits(freqMhz, p.muted.copy(alpha = 0.45f), p.bright), fontFamily = Mono, fontSize = size, maxLines = 1, softWrap = false,
                    modifier = Modifier.heightIn(min = LocalSpacing.current.touch).clickable(role = Role.Button, onClickLabel = editDesc) { editing = true }
                        .semantics { contentDescription = editDesc }.padding(vertical = 4.dp),
                )
            }
            OnAirIconButton(OnAirIcons.Plus, stringResource(R.string.cd_step_up, stepText(step)), framed = false) { onTune(freqMhz + step / 1e6) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(start = 8.dp)) {
            Label(stringResource(R.string.mhz) + "   " + stringResource(R.string.step), size = TypeScale.CAPTION)
            Chip(
                if (stepHz > 0) stepText(stepHz) else stringResource(R.string.step_auto, stepText(autoStep)),
                color = p.text, onClick = { stepMenu = true },
            )
        }
    }
    if (editing) FrequencyDialog(freqMhz, onDismiss = { editing = false }) { onTune(it); editing = false }
    if (stepMenu) StepDialog(stepHz, autoStep, onDismiss = { stepMenu = false }) { onStepHz(it); stepMenu = false }
}

@Composable
fun FrequencyDialog(freqMhz: Double, onDismiss: () -> Unit, onDone: (Double) -> Unit) {
    var text by remember { mutableStateOf(String.format(Locale.US, "%.6f", freqMhz).trimEnd('0').trimEnd('.')) }
    val parsed = DeepLink.parseFreqMhz(text)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.enter_frequency)) },
        text = {
            Column {
                TextBox(text, { text = it }, numeric = false, label = stringResource(R.string.frequency_mhz), onDone = { parsed?.let(onDone) })
                Label(stringResource(R.string.frequency_hint), Modifier.padding(top = 6.dp))
                if (parsed == null && text.isNotBlank()) Label(stringResource(R.string.frequency_invalid), color = LocalPalette.current.bad)
            }
        },
        confirmButton = { TextButton({ parsed?.let(onDone) }, enabled = parsed != null) { Text(stringResource(R.string.ok)) } },
        dismissButton = { TextButton(onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun StepDialog(stepHz: Double, autoStep: Double, onDismiss: () -> Unit, onPick: (Double) -> Unit) {
    val p = LocalPalette.current
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.step)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                for (c in StepChoices) {
                    val name = if (c == 0.0) stringResource(R.string.step_auto, stepText(autoStep)) else stepText(c)
                    val sel = c == stepHz
                    Text(
                        name, color = if (sel) p.accent else p.text, fontFamily = Mono, fontSize = TypeScale.BODY_LARGE.sp,
                        modifier = Modifier.heightIn(min = LocalSpacing.current.touch).clickable(role = Role.RadioButton) { onPick(c) }.padding(vertical = 12.dp, horizontal = 4.dp),
                    )
                }
            }
        },
        confirmButton = { TextButton(onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}
