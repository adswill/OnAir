// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair

import android.app.Application
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.diag.CrashLog

class OnAirApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        CrashLog.install(this)                  // first: a crash anywhere after this line is recorded
        AppGraph.init(this)
        AppLog.i("app", CrashLog.header())
        AppGraph.repository                      // starts the receiver thread, reads the settings and the mode table
    }
}
