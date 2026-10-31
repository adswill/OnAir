package io.github.adswill.onair.ui

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.github.adswill.onair.Colormaps
import io.github.adswill.onair.LocalPalette
import io.github.adswill.onair.OnAirIcons
import io.github.adswill.onair.OnAirTheme
import io.github.adswill.onair.TypeLook
import io.github.adswill.onair.data.Phase
import io.github.adswill.onair.data.ReceiverState
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.LockState
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.model.SpectrumFrame
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.ui.about.AboutContent
import io.github.adswill.onair.ui.about.Contribution
import io.github.adswill.onair.ui.about.Credit
import io.github.adswill.onair.ui.log.LogContent
import io.github.adswill.onair.ui.modes.ModePickerContent
import io.github.adswill.onair.ui.onboarding.Lesson
import io.github.adswill.onair.ui.onboarding.LessonStep
import io.github.adswill.onair.ui.onboarding.LessonViewer
import io.github.adswill.onair.ui.radio.AddNetworkRadioDialog
import io.github.adswill.onair.ui.radio.SourceRow
import io.github.adswill.onair.ui.receiver.HeaderContent
import io.github.adswill.onair.ui.receiver.SoundStripContent
import io.github.adswill.onair.ui.receiver.SpectrumWaterfallContent
import io.github.adswill.onair.ui.settings.SettingsActions
import io.github.adswill.onair.ui.settings.SettingsContent
import kotlin.math.exp
import kotlin.math.sin

/*
 * @Preview of every screen, in the dark (default) and light themes, phone and tablet sizes, and at 200 % text size where the layout
 * is most at risk. They draw the stateless "Content" composables with sample data (no view model, no native library).
 */

private val sampleState = ReceiverState(initialised = true, phase = Phase.RUNNING, modeIndex = 0, freqMhz = 100.5)

private fun sampleFrame(): SpectrumFrame {
    val n = SpectrumFrame.BINS
    val db = FloatArray(n) { i ->
        val x = i / n.toFloat()
        (-92 + 3 * sin(i * 0.7f) + 55 * exp(-((x - 0.5f) * 40) * ((x - 0.5f) * 40)) + 30 * exp(-((x - 0.78f) * 90) * ((x - 0.78f) * 90))).toFloat()
    }
    return SpectrumFrame(1, db, db.copyOf(), 100.5e6, 2.4e6, -24.0, 0.0, -95f, -30f)
}

private fun sampleWaterfall(): Waterfall {
    val wf = Waterfall(SpectrumFrame.BINS, 128)
    val lut = Colormaps.lut(4)
    val f = sampleFrame()
    repeat(128) { k -> wf.push(FloatArray(f.db.size) { i -> f.db[i] + 4 * sin((i + k) * 0.31f) }, -95f, -30f, lut) }
    wf.commit()
    return wf
}

@Composable
private fun PreviewSurface(dark: Boolean = true, content: @Composable () -> Unit) =
    OnAirTheme(dark = dark, look = TypeLook.CURRENT) { Box(Modifier.background(LocalPalette.current.window)) { content() } }

@Composable
private fun ReceiverPreviewBody() {
    val frame = remember { mutableStateOf(sampleFrame()) }
    val version = remember { mutableLongStateOf(1L) }
    val state = remember { mutableStateOf(sampleState) }
    val settings = remember { mutableStateOf(Settings()) }
    val wf = remember { sampleWaterfall() }
    Column(Modifier.fillMaxSize()) {
        HeaderContent(sampleState, "FM broadcast", LockState.LOCKED, 31.4, Meters(-92f, -24f, 0f), 0.0, false, {}, {}, {}, {})
        SpectrumWaterfallContent(frame, version, state, settings, wf, centreTuning = false, onTune = {}, onToggleCentre = {}, modifier = Modifier.weight(1f).fillMaxWidth())
        SoundStripContent(0.8f, false, { _, _ -> }, false, true, true, -60f, { _, _ -> }, recordAvailable = false, recording = false, recordSeconds = 0, onRecord = {})
    }
}

@Preview(name = "Receiver, phone", widthDp = 393, heightDp = 851, showBackground = true)
@Composable
fun PreviewReceiverPhone() = PreviewSurface { ReceiverPreviewBody() }

@Preview(name = "Receiver, phone, light", widthDp = 393, heightDp = 851, showBackground = true, uiMode = Configuration.UI_MODE_NIGHT_NO)
@Composable
fun PreviewReceiverPhoneLight() = PreviewSurface(dark = false) { ReceiverPreviewBody() }

@Preview(name = "Receiver, 200 % text", widthDp = 393, heightDp = 851, fontScale = 2f, showBackground = true)
@Composable
fun PreviewReceiverLargeText() = PreviewSurface { ReceiverPreviewBody() }

@Preview(name = "Receiver, tablet two-pane", widthDp = 1280, heightDp = 800, showBackground = true)
@Composable
fun PreviewReceiverTablet() = PreviewSurface {
    CompositionLocalProvider(LocalAppLayout provides AppLayout.Tablet) {
        Row(Modifier.fillMaxSize()) {
            Box(Modifier.weight(0.58f).fillMaxHeight()) { ReceiverPreviewBody() }
            Box(Modifier.weight(0.42f).fillMaxHeight().padding(12.dp)) {
                LogContent(emptyList(), "", false, {}, {}, {})   // stands in for the panels (they need the view model)
            }
        }
    }
}

@Preview(name = "Mode picker", widthDp = 393, heightDp = 851, showBackground = true)
@Composable
fun PreviewModePicker() = PreviewSurface {
    ModePickerContent(
        selected = 0, source = SourceKind.RTL_TCP, favourites = setOf(0, 24), hidden = setOf(3), hideUnused = false, onlyCapable = false,
        needsWide = { it.group == 1 }, minRateMsps = { 8.0 }, onPick = {}, onFavourite = {}, onHide = { _, _ -> }, onHideUnused = {}, onOnlyCapable = {},
    )
}

@Preview(name = "Radio picker", widthDp = 393, heightDp = 700, showBackground = true)
@Composable
fun PreviewRadio() = PreviewSurface {
    Column(Modifier.padding(12.dp)) {
        SourceRow(OnAirIcons.Wave, "Test signal", "No radio needed", false) {}
        SourceRow(OnAirIcons.Usb, "RTL-SDR Blog V4", "Ready", true) {}
        SourceRow(OnAirIcons.Network, "Roof antenna", "rtl_tcp  192.168.1.20:1234", false) {}
        SourceRow(OnAirIcons.File, "IQ file", "No file chosen", false) {}
    }
}

@Preview(name = "Add network radio", widthDp = 393, heightDp = 700, showBackground = true)
@Composable
fun PreviewAddNetworkRadio() = PreviewSurface { AddNetworkRadioDialog(discoveryOn = false, onDiscovery = {}, onDismiss = {}, onAdd = {}) }

private object NoSettings : SettingsActions {
    override fun setDark(on: Boolean) {}; override fun setFollowSystemTheme(on: Boolean) {}; override fun setTypeLook(l: TypeLook) {}
    override fun setColormap(i: Int) {}; override fun setRange(lo: Float, hi: Float) {}; override fun autoRange() {}
    override fun setPeakHold(on: Boolean) {}; override fun setCentreTuning(on: Boolean) {}; override fun setStepHz(hz: Double) {}; override fun setVerbose(on: Boolean) {}
    override fun setHideUnused(on: Boolean) {}; override fun setOnlyCapable(on: Boolean) {}; override fun setSound(volume: Float, muted: Boolean) {}
    override fun setReduceMotion(on: Boolean) {}; override fun setDiscovery(on: Boolean) {}; override fun setMapConsent(v: Int) {}
}

@Preview(name = "Settings", widthDp = 393, heightDp = 851, showBackground = true)
@Composable
fun PreviewSettings() = PreviewSurface { SettingsContent(Settings(), UiPrefs(), NoSettings, {}, {}, {}) }

@Preview(name = "Settings, light", widthDp = 393, heightDp = 851, showBackground = true)
@Composable
fun PreviewSettingsLight() = PreviewSurface(dark = false) { SettingsContent(Settings(dark = false), UiPrefs(), NoSettings, {}, {}, {}) }

@Preview(name = "Log", widthDp = 393, heightDp = 700, showBackground = true)
@Composable
fun PreviewLog() = PreviewSurface {
    LogContent(
        listOf(
            AppLog.Entry(0, AppLog.Level.INFO, "engine", "start RTL_TCP 100.500000 MHz, mode 0"),
            AppLog.Entry(1, AppLog.Level.WARN, "core", "rtl_tcp: 12 samples dropped"),
            AppLog.Entry(2, AppLog.Level.ERROR, "engine", "start failed: connection refused"),
        ),
        error = "", crash = true, onShare = {}, onClear = {}, onCrashSeen = {},
    )
}

@Preview(name = "About and credits", widthDp = 393, heightDp = 700, showBackground = true)
@Composable
fun PreviewAbout() = PreviewSurface {
    AboutContent(
        "0.3.0", listOf(Credit("Dear ImGui", "Omar Cornut", "MIT", "https://github.com/ocornut/imgui")),
        listOf(Contribution("andriux26", "https://github.com/andriux26", "pull request", "SpyServer network radio")), listOf("ikkim"), "",
        onLink = {},
    )
}

@Preview(name = "Lesson", widthDp = 393, heightDp = 700, showBackground = true)
@Composable
fun PreviewLesson() = PreviewSurface {
    LessonViewer(
        Lesson("fm-first", "Your first FM station", "", listOf(LessonStep("Tune", "Tap the spectrum on a bright line, or type 100.5 in the readout.", "FM broadcast", "100.5M"))),
        onTry = {}, onClose = {},
    )
}
