// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.NavigationRailItemDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.LocalReducedMotion
import io.github.adswill.onair.LocalTextFamily
import io.github.adswill.onair.Modes
import io.github.adswill.onair.Mono
import io.github.adswill.onair.OnAirIconButton
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.R
import io.github.adswill.onair.TypeScale
import io.github.adswill.onair.diag.CrashLog
import io.github.adswill.onair.ui.settings.Battery
import io.github.adswill.onair.ui.about.AboutScreen
import io.github.adswill.onair.ui.log.LogScreen
import io.github.adswill.onair.ui.modes.ModePickerScreen
import io.github.adswill.onair.ui.nav.DeepLink
import io.github.adswill.onair.ui.nav.Dest
import io.github.adswill.onair.ui.nav.Navigator
import io.github.adswill.onair.ui.onboarding.OnboardingScreen
import io.github.adswill.onair.ui.panels.PanelHost
import io.github.adswill.onair.ui.radio.RadioScreen
import io.github.adswill.onair.ui.receiver.ReceiverScreen
import io.github.adswill.onair.ui.settings.SettingsScreen

private class TopItem(val dest: Dest, val label: Int, val icon: ImageVector)

private val topItems = listOf(
    TopItem(Dest.RECEIVER, R.string.tab_receiver, OnAirIcons.Spectrum),
    TopItem(Dest.PANELS, R.string.tab_results, OnAirIcons.Panels),
    TopItem(Dest.MODES, R.string.tab_mode, OnAirIcons.Modes),
    TopItem(Dest.RADIO, R.string.tab_radio, OnAirIcons.Radio),
    TopItem(Dest.SETTINGS, R.string.tab_settings, OnAirIcons.Settings),
)

/**
 * The app shell: adaptive navigation (bottom bar on phones in portrait, rail in landscape and on tablets), the back stack, deep
 * links, the error and crash banners, edge-to-edge insets and the first-run guide.
 *
 * @param pendingLink an `onair://` link the activity received (launch or onNewIntent); handled once, then [onLinkHandled].
 */
@Composable
fun OnAirApp(vm: ReceiverViewModel, pickFile: () -> Unit, pendingLink: String? = null, onLinkHandled: () -> Unit = {}) {
    val p = LocalPalette.current
    val ctx = LocalContext.current
    val st by vm.state.collectAsStateWithLifecycle()
    val ui by vm.ui.collectAsStateWithLifecycle()
    val uiLoaded by vm.uiLoaded.collectAsStateWithLifecycle()
    val nav = rememberSaveable(saver = Navigator.Saver) { Navigator() }
    val ask = rememberPermissionRequest(ctx)
    // the play button: the first start asks for the notification permission (the foreground service shows one); it starts either way
    val toggle = { if (!st.active && vm.shouldAskNotifications()) ask(listOf(AppPermission.NOTIFICATIONS)) { vm.toggle() } else vm.toggle() }
    var crashBanner by remember { mutableStateOf(CrashLog.hasUnseen(ctx)) }
    val names = stringArrayResource(R.array.mode_names).toList()

    LaunchedEffect(pendingLink) {
        val link = pendingLink?.let { DeepLink.parse(it) }
        if (pendingLink != null) {
            if (link != null) {
                val idx = link.mode?.let { DeepLink.resolveMode(it, names, Modes.all.map { m -> m.subId }) }
                vm.tuneTo(idx, link.freqMhz)
                // debug builds only: lets a script start a mode (the all-modes smoke test); a link from another app never starts the radio
                if (link.run && io.github.adswill.onair.BuildConfig.DEBUG) vm.startIfIdle()
                nav.navigate(Dest.RECEIVER)
            }
            onLinkHandled()
        }
    }
    BackHandler(enabled = nav.canGoBack) { nav.back() }

    BoxWithConstraints(Modifier.fillMaxSize().background(p.window)) {
        val layout = AppLayout.of(maxWidth, maxHeight)
        CompositionLocalProvider(LocalAppLayout provides layout) {
            when {
                !uiLoaded -> Box(Modifier.fillMaxSize())       // a few ms while the preferences are read: no flash of the wrong screen
                !ui.onboardingDone -> Box(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
                    OnboardingScreen(vm, pickFile, onDone = {})
                }
                else -> MainShell(vm, nav, layout, pickFile, toggle, crashBanner, { crashBanner = false })
            }
        }
    }
}

@Composable
private fun MainShell(
    vm: ReceiverViewModel, nav: Navigator, layout: AppLayout, pickFile: () -> Unit, toggle: () -> Unit, crashBanner: Boolean, onCrashSeen: () -> Unit,
) {
    val p = LocalPalette.current
    val ctx = LocalContext.current
    val st by vm.state.collectAsStateWithLifecycle()
    val items = if (layout.twoPane) topItems.filter { it.dest != Dest.PANELS } else topItems
    val selected = nav.selectedTop.let { if (layout.twoPane && it == Dest.PANELS) Dest.RECEIVER else it }
    val content = @Composable { m: Modifier ->
        Column(m) {
            if (crashBanner) Banner(stringResource(R.string.crash_banner), p.bad) { CrashLog.markSeen(ctx); onCrashSeen(); nav.navigate(Dest.LOG) }
            if (st.error.isNotEmpty()) Banner(st.error, p.bad) { vm.clearError(); nav.navigate(Dest.LOG) }
            val ui by vm.ui.collectAsStateWithLifecycle()
            if (st.running && !ui.batteryHintShown && !Battery.exempt(ctx)) Banner(stringResource(R.string.battery_hint), p.accent) { vm.markBatteryHintShown(); Battery.request(ctx) }
            Destination(vm, nav, pickFile, toggle, Modifier.weight(1f).fillMaxWidth())
        }
    }
    if (layout.useRail) {
        Row(Modifier.fillMaxSize()) {
            NavigationRail(containerColor = p.rail, contentColor = p.text) {
                for (item in items) NavigationRailItem(
                    selected = selected == item.dest, onClick = { nav.navigate(item.dest) },
                    icon = { Icon(item.icon, contentDescription = null) }, label = { NavLabel(item.label) },
                    colors = NavigationRailItemDefaults.colors(
                        selectedIconColor = p.accent, selectedTextColor = p.accent, indicatorColor = p.selected,
                        unselectedIconColor = p.muted, unselectedTextColor = p.muted,
                    ),
                )
            }
            Box(Modifier.width(1.dp).fillMaxHeight().background(p.border))
            content(Modifier.weight(1f).fillMaxHeight().windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Bottom + WindowInsetsSides.End)))
        }
    } else {
        Column(Modifier.fillMaxSize()) {
            content(Modifier.weight(1f).fillMaxWidth().windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal)))
            NavigationBar(containerColor = p.rail, contentColor = p.text, tonalElevation = 0.dp) {
                for (item in items) NavigationBarItem(
                    selected = selected == item.dest, onClick = { nav.navigate(item.dest) },
                    icon = { Icon(item.icon, contentDescription = null) }, label = { NavLabel(item.label) },
                    colors = NavigationBarItemDefaults.colors(
                        selectedIconColor = p.accent, selectedTextColor = p.accent, indicatorColor = p.selected,
                        unselectedIconColor = p.muted, unselectedTextColor = p.muted,
                    ),
                )
            }
        }
    }
}

@Composable
private fun NavLabel(res: Int) = Text(stringResource(res), fontFamily = LocalTextFamily.current, fontSize = TypeScale.CAPTION.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)

@Composable
private fun Destination(vm: ReceiverViewModel, nav: Navigator, pickFile: () -> Unit, toggle: () -> Unit, modifier: Modifier) {
    val layout = LocalAppLayout.current
    val reduced = LocalReducedMotion.current
    Crossfade(targetState = nav.current, modifier = modifier, animationSpec = if (reduced) snap() else tween(150), label = "dest") { d ->
        when (d) {
            Dest.RECEIVER, Dest.ONBOARDING -> ReceiverScreen(vm, onModeClick = { nav.navigate(Dest.MODES) }, onToggle = toggle, modifier = Modifier.fillMaxSize())
            Dest.PANELS -> if (layout.twoPane) ReceiverScreen(vm, onModeClick = { nav.navigate(Dest.MODES) }, onToggle = toggle, modifier = Modifier.fillMaxSize())
                else Column(Modifier.fillMaxSize()) {
                    // the panels full screen, with the receiver header on top so tuning stays at hand
                    io.github.adswill.onair.ui.receiver.ReceiverHeader(vm, compact = true, onModeClick = { nav.navigate(Dest.MODES) }, onToggle = toggle)
                    PanelHost(vm, Modifier.weight(1f).fillMaxWidth())
                }
            Dest.MODES -> ModePickerScreen(vm, onPicked = { nav.navigate(Dest.RECEIVER) }, modifier = Modifier.fillMaxSize())
            Dest.RADIO -> RadioScreen(vm, pickFile, Modifier.fillMaxSize())
            Dest.SETTINGS -> SettingsScreen(
                vm, onOpenLog = { nav.navigate(Dest.LOG) }, onOpenAbout = { nav.navigate(Dest.ABOUT) },
                onRunSetup = { vm.restartOnboarding() }, modifier = Modifier.fillMaxSize(),
            )
            Dest.LOG -> Secondary(stringResource(R.string.log_title), nav) { LogScreen(vm, it) }
            Dest.ABOUT -> Secondary(stringResource(R.string.about_title), nav) { AboutScreen(vm, it) }
        }
    }
}

/** A pushed screen: a slim title bar with Back, then the content. */
@Composable
private fun Secondary(title: String, nav: Navigator, content: @Composable (Modifier) -> Unit) {
    val p = LocalPalette.current
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).background(p.window), verticalAlignment = Alignment.CenterVertically) {
            OnAirIconButton(OnAirIcons.Back, stringResource(R.string.back), framed = false) { nav.back() }
            Text(title, color = p.text, fontFamily = LocalTextFamily.current, fontSize = TypeScale.TITLE.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.semantics { heading() })
        }
        content(Modifier.weight(1f).fillMaxWidth())
    }
}

@Composable
private fun Banner(text: String, color: androidx.compose.ui.graphics.Color, onClick: () -> Unit) {
    val p = LocalPalette.current
    Text(
        text, Modifier.fillMaxWidth().background(p.panel).clickable(role = Role.Button, onClick = onClick).heightIn(min = 48.dp).padding(horizontal = 12.dp, vertical = 10.dp),
        color = color, fontFamily = Mono, fontSize = TypeScale.SMALL.sp, maxLines = 3, overflow = TextOverflow.Ellipsis,
    )
}
