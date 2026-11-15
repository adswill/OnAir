// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import io.github.adswill.onair.data.RadioRepository
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.ui.UsbRadioInfo
import io.github.adswill.onair.ui.UsbRadios
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import io.github.adswill.onair.settings.Settings

/**
 * What the radio picker (ui/radio/RadioScreen.kt, through UiHooks.usb) shows of the USB radios: every supported radio that is plugged in,
 * ready or not (a radio without permission is tapped to ask Android again). Nothing filled UiHooks.usb before, so the list stayed empty
 * and no USB radio could be chosen (#435).
 */
class UsbRadiosBridge(
    private val radios: RadioRepository,
    settings: StateFlow<Settings>,
    private val scope: CoroutineScope,
    private val chooseUsbSource: () -> Unit,
) : UsbRadios {
    override val devices: StateFlow<List<UsbRadioInfo>> =
        combine(radios.usbRadios, settings) { list, s ->
            val using = radios.usbFor(s)?.key
            list.map { r ->
                UsbRadioInfo(
                    id = r.key, name = r.label,
                    detail = r.message.ifBlank { if (r.ready) r.model.notes else "" },
                    permitted = r.ready,
                    selected = r.ready && s.source == SourceKind.USB && r.key == using,
                )
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    override fun refresh() { scope.launch { radios.usb.rescan(askPermission = false) } }
    override fun requestPermission(id: String) = radios.requestUsbPermission(id)
    override fun select(id: String) {
        val r = radios.usb.radio(id) ?: return
        if (!r.ready) { radios.requestUsbPermission(id); return }
        radios.selectUsb(r)
        chooseUsbSource()
    }
}
