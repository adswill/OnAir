// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair

import android.content.Intent
import android.graphics.Color as AColor

import android.hardware.usb.UsbManager
import android.os.Bundle
import android.provider.Settings as SysSettings
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.snapshotFlow
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.ui.OnAirApp
import io.github.adswill.onair.ui.ReceiverViewModel
import io.github.adswill.onair.ui.nav.DeepLink
import kotlinx.coroutines.flow.MutableStateFlow

import io.github.adswill.onair.usb.AndroidUsbBackend

/**
 * The only activity: hosts the Compose UI and the system file picker. It owns no receiver state (that is in ReceiverRepository,
 * reached through [ReceiverViewModel]), so a rotation or a re-create changes nothing; the receive session itself lives in
 * ReceiverService and goes on with the screen off. Draws edge to edge; `onair://tune?...` links arrive here (launch or
 * onNewIntent, the activity is singleTop) and are handed to the shell once.
 */
class MainActivity : ComponentActivity() {
    private val vm: ReceiverViewModel by viewModels()
    private val pendingLink = MutableStateFlow<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(AColor.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(AColor.TRANSPARENT),
        )
        super.onCreate(savedInstanceState)
        if (savedInstanceState == null) takeLink(intent)

        if (savedInstanceState == null) usbAttached(intent)
        if (savedInstanceState == null) AppGraph.updates.checkOnStart()   // in-app update check (Settings > Updates turns it off)
        setContent {
            val settings by vm.settings.collectAsStateWithLifecycle()
            val ui by vm.ui.collectAsStateWithLifecycle()
            val state by vm.state.collectAsStateWithLifecycle()
            val link by pendingLink.collectAsStateWithLifecycle()
            LaunchedEffect(Unit) {
                // the screen stays on only while receiving
                snapshotFlow { state.active }.collect { on ->
                    if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
            }
            val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) vm.openFile(uri) }
            val dark = if (ui.followSystemTheme) isSystemInDarkTheme() else settings.dark
            LaunchedEffect(dark) {
                // light bar icons on the dark theme, dark ones on the light theme
                val bar = if (dark) SystemBarStyle.dark(AColor.TRANSPARENT) else SystemBarStyle.light(AColor.TRANSPARENT, AColor.TRANSPARENT)
                enableEdgeToEdge(statusBarStyle = bar, navigationBarStyle = bar)
            }
            OnAirTheme(dark = dark, look = ui.typeLook, reducedMotion = ui.reduceMotion || systemAnimationsOff()) {
                OnAirApp(vm, pickFile = { picker.launch(arrayOf("*/*")) }, pendingLink = link, onLinkHandled = { pendingLink.value = null })
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        takeLink(intent)
        usbAttached(intent)
    }

    private fun takeLink(i: Intent?) {
        val data = i?.data ?: return
        if (i.action == Intent.ACTION_VIEW && data.scheme.equals(DeepLink.SCHEME, true)) pendingLink.value = data.toString()
    }

    /** The system's "Remove animations" (animator duration scale 0). */
    private fun systemAnimationsOff(): Boolean = try {
        SysSettings.Global.getFloat(contentResolver, SysSettings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
    } catch (_: Exception) { false }

    /** Opened by the USB_DEVICE_ATTACHED filter (a radio plugged in, the user picked OnAir): hand the device to the radio layer. */
    private fun usbAttached(i: Intent?) {
        if (i?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) return
        val d = AndroidUsbBackend.deviceOf(i) ?: return
        AppGraph.repository.radios.onAttachIntent(AndroidUsbBackend.ref(d))
    }

    override fun onStart() { super.onStart(); vm.setUiVisible(true) }
    override fun onStop() { vm.setUiVisible(false); super.onStop() }
}
