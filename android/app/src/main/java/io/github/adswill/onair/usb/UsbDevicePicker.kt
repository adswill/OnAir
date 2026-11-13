// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import io.github.adswill.onair.model.RadioInfo
import java.util.Locale

/**
 * The USB device picker: the supported radios on the port, what each can do, and what to do when one is not ready (permission,
 * power, no driver in this build). Stateless: the caller passes the repository's flows (RadioRepository.usbRadios / usbInfo) and
 * the actions; the theme is the app's MaterialTheme (OnAirTheme), so it takes the OnAir look of the screen that opens it.
 *
 * Tapping a ready radio selects it; tapping one that is not ready asks Android for the permission again.
 */
@Composable
fun UsbDevicePicker(
    radios: List<UsbRadio>,
    info: Map<String, RadioInfo>,
    selectedKey: String?,
    hostSupported: Boolean,
    onSelect: (UsbRadio) -> Unit,
    onRequestPermission: (UsbRadio) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } },
        title = { Text("USB radio") },
        text = {
            when {
                !hostSupported -> Text("This phone has no USB host (OTG) support. Use a network radio (rtl_tcp, SpyServer, KiwiSDR) instead.")
                radios.isEmpty() -> Text(
                    "No supported radio is plugged in.\n\nConnect an RTL-SDR, HackRF or Airspy through a USB OTG adapter. " +
                        "Android then asks whether OnAir may use it.\n\nSDRplay radios cannot be used on Android (their driver does not exist for it).",
                )
                else -> LazyColumn {
                    items(radios, key = { it.key }) { r ->
                        RadioRow(r, info[r.key], r.key == selectedKey) { if (r.ready) onSelect(r) else onRequestPermission(r) }
                        HorizontalDivider()
                    }
                }
            }
        },
    )
}

@Composable
private fun RadioRow(r: UsbRadio, i: RadioInfo?, selected: Boolean, onClick: () -> Unit) {
    Column(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(vertical = 8.dp)) {
        Text((if (selected) "● " else "") + (i?.name?.takeIf { it.isNotBlank() } ?: r.label), style = MaterialTheme.typography.titleSmall)
        Text(statusText(r), style = MaterialTheme.typography.bodySmall, color = if (r.ready) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error)
        capsText(r, i)?.let { Text(it, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace) }
        val warn = r.message.ifEmpty { r.powerWarning }
        if (warn.isNotEmpty()) { Spacer(Modifier.height(2.dp)); Text(warn, style = MaterialTheme.typography.bodySmall) }
    }
}

private fun statusText(r: UsbRadio): String = when (r.status) {
    UsbRadioStatus.READY -> "Ready"
    UsbRadioStatus.NEEDS_PERMISSION -> "Tap to allow OnAir to use it"
    UsbRadioStatus.ASKING -> "Waiting for your answer in Android's dialog"
    UsbRadioStatus.DENIED -> "Not allowed: tap to ask again"
    UsbRadioStatus.FAILED -> "Could not be opened: tap to try again"
    UsbRadioStatus.NO_DRIVER -> "Not available in this build"
}

/** "24 - 1766 MHz · up to 2.4 Msps · bias tee" from the driver's report, else from the catalog. */
private fun capsText(r: UsbRadio, i: RadioInfo?): String? {
    val lo = (i?.minFreqHz?.takeIf { it > 0 } ?: r.model.minFreqHz) / 1e6
    val hi = (i?.maxFreqHz?.takeIf { it > 0 } ?: r.model.maxFreqHz) / 1e6
    val rate = (i?.maxRateHz?.takeIf { it > 0 } ?: r.model.maxRateHz) / 1e6
    val bias = i?.biasTee ?: r.model.biasTee
    fun f(v: Double) = if (v >= 100) String.format(Locale.US, "%.0f", v) else String.format(Locale.US, "%.3g", v)
    val stages = i?.stages?.joinToString(" ") { "${it.name} ${f(it.minDb)}-${f(it.maxDb)}" }?.takeIf { it.isNotEmpty() }
    return listOfNotNull("${f(lo)} - ${f(hi)} MHz", "up to ${f(rate)} Msps", if (bias) "bias tee" else null, stages).joinToString(" · ")
}
