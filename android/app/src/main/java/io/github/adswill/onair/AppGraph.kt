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

    fun init(application: Application) { app = application }
}
