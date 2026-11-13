// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.data

import android.content.Context
import io.github.adswill.onair.audio.SoundCardInput
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.RadioInfo
import io.github.adswill.onair.model.UsbFeatures
import io.github.adswill.onair.netradio.AddNetworkRadioForm
import io.github.adswill.onair.netradio.FoundRadio
import io.github.adswill.onair.netradio.NetDiscovery
import io.github.adswill.onair.netradio.NetworkRadio
import io.github.adswill.onair.netradio.NetworkRadioList
import io.github.adswill.onair.settings.Settings
import io.github.adswill.onair.usb.AndroidUsbBackend
import io.github.adswill.onair.usb.NativeUsbLink
import io.github.adswill.onair.usb.RadioNative
import io.github.adswill.onair.usb.UsbDeviceRef
import io.github.adswill.onair.usb.UsbRadio
import io.github.adswill.onair.usb.UsbRadioController
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

/** Runs a block on the receiver thread (ReceiverEngine.call): every RadioNative call goes through it. */
interface EngineExec {
    suspend fun <T> call(name: String, block: () -> T): T
}

/**
 * Radio access for the repository: USB radios (permission, hot-plug, the descriptor handed to the core), the network radio list and
 * its opt-in discovery, the sound-card input. Owned by [ReceiverRepository] (one per process); the UI reads the flows through the
 * view model and never calls this directly.
 *
 * Settings it uses (Settings.kt): usbDevice (which radio, as "vid:pid:serial", so it survives a re-plug that changes the device
 * name), netRadios / netSelected (the list), netDiscovery (mDNS opt-in, default off).
 */
class RadioRepository(
    private val ctx: Context,
    private val scope: CoroutineScope,
    private val engine: EngineExec,
    private val settings: StateFlow<Settings>,
    private val updateSettings: ((Settings) -> Settings) -> Unit,
    /** A radio in use was unplugged: stop the session (suspends until stopped), with this message for the user. */
    private val onRadioLost: suspend (message: String) -> Unit,
    /** The USB radios changed (one became ready or went away): the source's capabilities may have changed. */
    private val onRadiosChanged: () -> Unit,
    /** A radio became ready (permission given, opened): the repository may switch the source to it. */
    private val onRadioReady: (UsbRadio) -> Unit = {},
) {
    private val backend = AndroidUsbBackend(ctx)
    private val _features = MutableStateFlow(UsbFeatures.none)
    /** What this build of the native library can drive. */
    val features: StateFlow<UsbFeatures> = _features.asStateFlow()

    private val link = object : NativeUsbLink {
        override suspend fun attach(dev: UsbDeviceRef, kind: String, fd: Int, serial: String): Boolean =
            RadioNative.loaded && engine.call("usbAttach") { RadioNative.nativeUsbAttach(dev.name, kind, fd, dev.vid, dev.pid, dev.manufacturer ?: "", dev.product ?: "", serial) }
        override suspend fun detach(name: String) { if (RadioNative.loaded) engine.call("usbDetach") { RadioNative.nativeUsbDetach(name) } }
        override suspend fun supports(kind: String): Boolean = loadFeatures().supports(kind)
    }

    @Volatile private var featuresLoaded = false
    /** Asked once (a radio can be plugged in before start() ran: the "open with OnAir" intent of a cold start). */
    private suspend fun loadFeatures(): UsbFeatures {
        if (!featuresLoaded && RadioNative.loaded) {
            _features.value = try { UsbFeatures.parse(engine.call("usbFeatures") { RadioNative.nativeUsbFeatures() }) } catch (_: Throwable) { UsbFeatures.none }
            featuresLoaded = true
        }
        return _features.value
    }

    val usb = UsbRadioController(
        backend, link,
        onLost = { r -> if (r.key == activeUsbKey) onRadioLost(UsbRadioController.MSG_UNPLUGGED) },
        log = { AppLog.i("usb", it) },
    )
    /** The supported radios on the USB port now. */
    val usbRadios: StateFlow<List<UsbRadio>> get() = usb.radios

    private val _usbInfo = MutableStateFlow<Map<String, RadioInfo>>(emptyMap())
    /** The drivers' report of the ready radios (rates, range, gain stages, bias tee), by device key. */
    val usbInfo: StateFlow<Map<String, RadioInfo>> = _usbInfo.asStateFlow()

    /** The device key the running session uses ("" = none). Set by the repository at start. */
    @Volatile var activeUsbKey: String = ""

    val hostSupported: Boolean get() = backend.hostSupported

    private val discovery = NetDiscovery(ctx)
    val discovered: StateFlow<List<FoundRadio>> get() = discovery.found
    private val sound = SoundCardInput(ctx)
    val soundPermitted: Boolean get() = sound.permitted

    private var unlisten: (() -> Unit)? = null

    /** After the engine is initialised: what the library can drive, the receiver for USB events, the radios already plugged in. */
    fun start() {
        scope.launch {
            loadFeatures()
            AppLog.i("usb", "host ${backend.hostSupported}, drivers ${_features.value}")
            if (!backend.hostSupported) return@launch
            scope.launch(Dispatchers.Main) {
                unlisten = backend.listen(
                    onAttach = { d -> scope.launch { usb.attached(d, askPermission = true) } },
                    onDetach = { n -> scope.launch { usb.detached(n) } },
                    onPermission = { n, ok -> scope.launch { usb.onPermissionResult(n, ok) } },
                )
            }
            usb.rescan(askPermission = false)
        }
        // keep the drivers' view in step with what is ready
        scope.launch {
            var known = emptySet<String>()
            usb.radios.map { l -> l.filter { it.ready }.map { it.key } }.distinctUntilChanged().collect { keys ->
                val fresh = keys.filter { it !in known }
                known = keys.toSet()
                refreshUsbInfo(); onRadiosChanged()
                fresh.firstOrNull()?.let { k -> usb.radio(k)?.let(onRadioReady) }
            }
        }
    }

    /** MainActivity got USB_DEVICE_ATTACHED (the user picked OnAir in the system's "open with" dialog: the permission comes with it). */
    fun onAttachIntent(d: UsbDeviceRef) { scope.launch { usb.attached(d, askPermission = true) } }

    /** The user tapped a radio that is not ready: ask Android again. */
    fun requestUsbPermission(key: String) { scope.launch { usb.requestPermission(key) } }

    /** Selects a USB radio for the USB source (stored by its id and serial). */
    fun selectUsb(r: UsbRadio) = updateSettings { it.copy(usbDevice = prefOf(r)) }

    /** The ready radio the USB source uses now: the chosen one if plugged in, else the first ready one. null: none ready. */
    fun usbFor(s: Settings): UsbRadio? {
        val ready = usb.radios.value.filter { it.ready }
        return ready.firstOrNull { prefOf(it) == s.usbDevice } ?: ready.firstOrNull { prefOf(it).substringBeforeLast(':') == s.usbDevice.substringBeforeLast(':') } ?: ready.firstOrNull()
    }

    /** Why the USB source cannot start, in plain words ("" = it can). */
    fun usbProblem(s: Settings): String {
        if (!backend.hostSupported) return "This phone has no USB host support (USB OTG): use a network radio instead."
        if (usbFor(s) != null) return ""
        val l = usb.radios.value
        return when {
            l.isEmpty() -> "No USB radio is plugged in. Connect it through a USB OTG adapter (USB-C or micro-USB to USB-A)."
            else -> l.first().message.ifEmpty { "The radio is not ready yet: tap it and allow OnAir to use it." }
        }
    }

    private suspend fun refreshUsbInfo() {
        // the listing opens each radio for a moment: never while one streams (the report of the one in use is kept)
        if (!RadioNative.loaded || activeUsbKey.isNotEmpty()) return
        val (list, err) = try { RadioInfo.parseList(engine.call("usbRadios") { RadioNative.nativeUsbRadios() }) } catch (_: Throwable) { emptyList<RadioInfo>() to "" }
        if (err.isNotEmpty()) AppLog.w("usb", "drivers: $err")
        _usbInfo.value = list.associateBy { it.usbName }
    }

    // ---- network radios -------------------------------------------------------------------------------------------------------
    fun networkRadios(s: Settings = settings.value): List<NetworkRadio> = NetworkRadioList.decode(s.netRadios)

    /** The radio the network source uses: the selected one, else the first. */
    fun networkFor(s: Settings): NetworkRadio? = networkRadios(s).let { it.getOrNull(s.netSelected) ?: it.firstOrNull() }

    /** Checks the form; adds the radio (and selects it) when it is valid. Returns the check, so the dialog can show the field errors. */
    fun addNetworkRadio(form: AddNetworkRadioForm): AddNetworkRadioForm.Result {
        val r = form.validate()
        val radio = r.radio ?: return r
        updateSettings { s ->
            val l = networkRadios(s).filterNot { it.spec == radio.spec } + radio
            s.copy(netRadios = NetworkRadioList.encode(l), netSelected = l.size - 1)
        }
        return r
    }

    fun removeNetworkRadio(index: Int) = updateSettings { s ->
        val l = networkRadios(s).toMutableList()
        if (index in l.indices) l.removeAt(index)
        s.copy(netRadios = NetworkRadioList.encode(l), netSelected = s.netSelected.coerceIn(0, maxOf(0, l.size - 1)))
    }

    fun selectNetworkRadio(index: Int) = updateSettings { it.copy(netSelected = index) }

    /** Whether the radio answers (within the core's 8 s): "" yes, else why not. On the receiver thread; does not block the caller's thread. */
    suspend fun probe(r: NetworkRadio): String =
        if (!RadioNative.loaded) "The receiver library is missing" else try { engine.call("netProbe") { RadioNative.nativeNetProbe(r.spec, 8000) } } catch (e: Throwable) { e.message ?: "probe failed" }

    /** The radio's report (rates, gains) for the device sheet, or why there is none. */
    suspend fun describe(r: NetworkRadio): Result<RadioInfo> =
        if (!RadioNative.loaded) Result.failure(IllegalStateException("The receiver library is missing"))
        else try { RadioInfo.parseOne(engine.call("netDevice") { RadioNative.nativeNetDevice(r.spec) }) } catch (e: Throwable) { Result.failure(e) }

    /** The mDNS opt-in (default off). Turning it off stops a running discovery at once. */
    fun setDiscoveryAllowed(on: Boolean) {
        updateSettings { it.copy(netDiscovery = on) }
        if (!on) discovery.stop()
    }

    /** The "Add network radio" dialog opened / closed: discovery runs only in between, and only when the user allowed it. */
    fun discoveryWanted(dialogOpen: Boolean) {
        if (dialogOpen && settings.value.netDiscovery) discovery.start() else discovery.stop()
    }

    // ---- sound card -----------------------------------------------------------------------------------------------------------
    /** Starts recording for a sound-card session: "" or why not. */
    fun startSoundCard(): String = sound.start()
    fun stopSoundCard() = sound.stop()

    fun close() {
        unlisten?.invoke(); unlisten = null
        discovery.stop()
        sound.stop()
    }

    companion object {
        /** How the chosen USB radio is remembered: "vid:pid:serial" (hex ids). */
        fun prefOf(r: UsbRadio): String = "%04x:%04x:%s".format(r.device.vid, r.device.pid, r.device.serial ?: "")
    }
}
