// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.panels

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import android.view.Window
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import io.github.adswill.onair.Chip
import io.github.adswill.onair.R
import kotlinx.coroutines.delay

private fun Context.findActivity(): Activity? {
    var c: Context? = this
    while (c is ContextWrapper) { if (c is Activity) return c; c = c.baseContext }
    return null
}

/**
 * The TV picture full screen (docs/ANDROID-PLAYER-4.1.md): a dialog over everything, black, the system bars hidden (a swipe from the edge shows
 * them for a moment), landscape while it is open when the picture is wider than tall. Back or the Exit chip closes it. A tap shows the chips
 * for four seconds. [content] draws the picture; the caller keeps only one copy of it on screen (one SurfaceView per player).
 */
@Composable
fun TvFullscreen(landscape: Boolean, onExit: () -> Unit, content: @Composable BoxScope.() -> Unit) {
    val activity = LocalContext.current.findActivity()
    DisposableEffect(activity, landscape) {
        val before = activity?.requestedOrientation
        if (landscape) activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        onDispose { if (before != null) activity?.requestedOrientation = before }
    }
    Dialog(onDismissRequest = onExit, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        val window: Window? = (LocalView.current.parent as? DialogWindowProvider)?.window
        DisposableEffect(window) {
            if (window != null) {
                WindowCompat.setDecorFitsSystemWindows(window, false)
                val c = WindowInsetsControllerCompat(window, window.decorView)
                c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                c.hide(WindowInsetsCompat.Type.systemBars())
            }
            onDispose { }
        }
        var chips by remember { mutableStateOf(true) }
        var tapCount by remember { mutableStateOf(0) }
        LaunchedEffect(tapCount) { chips = true; delay(4000); chips = false }
        Box(
            Modifier.fillMaxSize().background(Color.Black)
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { tapCount++ },
            contentAlignment = Alignment.Center,
        ) {
            content()
            if (chips) Chip(stringResource(R.string.tv_exit_fullscreen), Modifier.align(Alignment.TopEnd).padding(12.dp), color = Color.White, onClick = onExit)
        }
    }
}
