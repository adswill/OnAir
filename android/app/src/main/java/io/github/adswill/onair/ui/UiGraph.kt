// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import io.github.adswill.onair.AppGraph
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.StateFlow

/** The interface layer's process-wide objects (next to AppGraph, which the receiver side owns). */
object UiGraph {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    val prefs: UiPrefsStore by lazy { UiPrefsStore(AppGraph.app, scope) }
}

/** A USB radio as the radio picker shows it. */
data class UsbRadioInfo(
    val id: String, val name: String, val detail: String = "",
    /** The user allowed the app to open it (UsbManager permission). */
    val permitted: Boolean = false,
    /** It is the chosen source now. */
    val selected: Boolean = false,
)

/** What the USB side gives the radio picker. Implemented by the USB work (android-usb-030); null = this build has no USB radios. */
interface UsbRadios {
    val devices: StateFlow<List<UsbRadioInfo>>
    fun refresh()
    /** Ask Android for permission to open [id] (UsbManager.requestPermission). */
    fun requestPermission(id: String)
    /** Make [id] the source. */
    fun select(id: String)
}

/** Recording of IQ or audio. Implemented by the native/service side; null = the record button is shown disabled. */
interface Recorder {
    val recording: StateFlow<Boolean>
    /** Seconds recorded so far, for the button's label. */
    val seconds: StateFlow<Int>
    fun toggle()
}

/**
 * Hooks the other layers fill in at start-up (for example in OnAirApplication or their own initialiser), so the screens can show
 * their features without the UI files changing. Read on the main thread when composing; set before the first activity starts.
 */
object UiHooks {
    @Volatile var usb: UsbRadios? = null
    @Volatile var recorder: Recorder? = null
}
