// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair

import android.app.Application
import io.github.adswill.onair.data.ReceiverRepository
import io.github.adswill.onair.settings.SettingsStore

/**
 * The process-wide objects, created once from [OnAirApplication.onCreate] (a small hand-written service locator, no DI framework).
 * Everything here lives as long as the process: activities, view models and the service only borrow it.
 */
object AppGraph {
    lateinit var app: Application
        private set
    val settings: SettingsStore by lazy { SettingsStore(app) }
    val updates: io.github.adswill.onair.update.UpdateManager by lazy { io.github.adswill.onair.update.UpdateManager(app, settings) }
    val repository: ReceiverRepository by lazy { ReceiverRepository(app, settings) }
    /** TV picture and sound with the phone's decoders (tv/, the Watch panel of the TV modes). */
    val tv: io.github.adswill.onair.tv.TvController by lazy { io.github.adswill.onair.tv.TvController() }
    /** The pictures of the analogue TV modes (Analog TV, MAC, FPV video) on the same Watch panel. */
    val analogTv: io.github.adswill.onair.tv.AnalogTvController by lazy { io.github.adswill.onair.tv.AnalogTvController() }

    fun init(application: Application) { app = application }
}
