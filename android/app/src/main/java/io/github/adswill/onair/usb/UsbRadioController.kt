// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Where one plugged-in radio is in the permission / open flow. */
enum class UsbRadioStatus {
    /** Plugged in; Android has not been asked yet (or the dialog was dismissed). */
    NEEDS_PERMISSION,
    /** The system dialog is showing. */
    ASKING,
    /** The user said no. */
    DENIED,
    /** Opened and handed to the core: ready to receive. */
    READY,
    /** Android or the core refused to open it. */
    FAILED,
    /** This build of OnAir has no driver for it (docs/USB.md). */
    NO_DRIVER,
}

/** One supported radio on the USB port. [message] is for the user (plain words), "" when there is nothing to say. */
data class UsbRadio(
    val device: UsbDeviceRef,
    val model: UsbCatalog.Model,
    val status: UsbRadioStatus,
    val message: String = "",
) {
    val key: String get() = device.name
    val ready: Boolean get() = status == UsbRadioStatus.READY
    /** Name for lists: the model, with the end of the serial number when there is one ("HackRF One ...a1b2c3d4"). */
    val label: String get() = model.name + (device.serial?.takeIf { it.isNotBlank() }?.let { " (" + it.takeLast(8) + ")" } ?: "")
    /** A warning about the phone's port power: the configuration's own figure when it says more than the catalog. */
    val powerWarning: String get() =
        if (device.maxPowerMa > model.typicalMa && device.maxPowerMa >= 400) model.copy(typicalMa = device.maxPowerMa).powerWarning else model.powerWarning
}

/**
 * The USB permission and hot-plug flow, without Android classes (tests drive it with fakes):
 *
 *  attach (broadcast, the "open with OnAir" intent, or a scan) -> permission? open : ask (system dialog)
 *  permission answer -> yes: open -> descriptor to the core (READY) / no: DENIED
 *  detach -> [onLost] first (the repository stops a session that uses the radio and waits for it), then the core forgets the
 *            descriptor, then the connection is closed. In that order, so no driver reads a closed descriptor.
 *
 * Every public function is safe to call from any thread; they run one at a time.
 */
class UsbRadioController(
    private val backend: UsbBackend,
    private val native: NativeUsbLink,
    /** Called (suspending) before a radio that was READY goes away: stop using it. */
    private val onLost: suspend (UsbRadio) -> Unit = {},
    private val log: (String) -> Unit = {},
) {
    private val mu = Mutex()
    private val open = HashMap<String, OpenedUsb>()
    private val _radios = MutableStateFlow<List<UsbRadio>>(emptyList())
    /** The supported radios plugged in now, in the order they arrived. */
    val radios: StateFlow<List<UsbRadio>> = _radios.asStateFlow()

    fun radio(key: String): UsbRadio? = _radios.value.firstOrNull { it.key == key }

    /** Looks at what is plugged in now (app start, or after a missed broadcast): new radios are handled as attached, gone ones as detached. */
    suspend fun rescan(askPermission: Boolean = false) {
        if (!backend.hostSupported) return
        val now = backend.devices().filter { UsbCatalog.family(it.vid, it.pid) != null }
        val names = now.map { it.name }.toSet()
        for (r in _radios.value.filter { it.key !in names }) detached(r.key)
        for (d in now) if (radio(d.name) == null) attached(d, askPermission)
    }

    /** A device was plugged in. [askPermission]: show the system dialog when there is no permission yet (the user is looking). */
    suspend fun attached(dev: UsbDeviceRef, askPermission: Boolean = true) = mu.withLock {
        val model = UsbCatalog.identify(dev.vid, dev.pid, dev.manufacturer, dev.product) ?: return@withLock
        if (radio(dev.name)?.ready == true) return@withLock
        put(UsbRadio(dev, model, UsbRadioStatus.NEEDS_PERMISSION))
        log("attached ${dev.name} ${UsbCatalog.UsbId(dev.vid, dev.pid)} ${model.name}")
        if (!native.supports(model.family.kind)) {
            put(UsbRadio(dev, model, UsbRadioStatus.NO_DRIVER, MSG_NO_DRIVER.format(model.name)))
            return@withLock
        }
        if (backend.hasPermission(dev.name)) openLocked(dev.name)
        else if (askPermission) askLocked(dev.name)
    }

    /** The user picked a radio that has no permission (or was refused before): ask again. */
    suspend fun requestPermission(key: String) = mu.withLock {
        val r = radio(key) ?: return@withLock
        if (r.ready || r.status == UsbRadioStatus.NO_DRIVER) return@withLock
        if (backend.hasPermission(key)) openLocked(key) else askLocked(key)
    }

    /** The answer of the system dialog (ACTION_USB_PERMISSION). */
    suspend fun onPermissionResult(key: String, granted: Boolean) = mu.withLock {
        val r = radio(key) ?: return@withLock
        if (granted) openLocked(key)
        else put(r.copy(status = UsbRadioStatus.DENIED, message = MSG_DENIED))
    }

    /** A device was unplugged (ACTION_USB_DEVICE_DETACHED). */
    suspend fun detached(key: String) {
        val r = mu.withLock { radio(key) } ?: return
        if (r.ready) try { onLost(r) } catch (e: Exception) { log("onLost failed: ${e.message}") }   // outside the lock: it may wait for the engine
        mu.withLock { closeLocked(key); _radios.update { l -> l.filterNot { it.key == key } } }
        log("detached $key")
    }

    /** Closes every connection (process end, or the user turned USB off). */
    suspend fun closeAll() {
        for (r in _radios.value) detached(r.key)
    }

    // ---- inside the lock ----
    private fun put(r: UsbRadio) = _radios.update { l -> if (l.any { it.key == r.key }) l.map { if (it.key == r.key) r else it } else l + r }

    private fun askLocked(key: String) {
        val r = radio(key) ?: return
        if (backend.requestPermission(key)) put(r.copy(status = UsbRadioStatus.ASKING, message = ""))
        else put(r.copy(status = UsbRadioStatus.NEEDS_PERMISSION, message = MSG_CANNOT_ASK))
    }

    private suspend fun openLocked(key: String) {
        val r = radio(key) ?: return
        closeLocked(key)
        val c = backend.open(key)
        if (c == null) {
            put(r.copy(status = UsbRadioStatus.FAILED, message = MSG_OPEN_FAILED))
            return
        }
        val dev = r.device.copy(serial = c.serial ?: r.device.serial)
        val ok = try { native.attach(dev, r.model.family.kind, c.fd, dev.serial ?: "") } catch (e: Exception) { log("attach failed: ${e.message}"); false }
        if (!ok) {
            c.close()
            put(r.copy(device = dev, status = UsbRadioStatus.FAILED, message = MSG_CORE_REFUSED))
            return
        }
        open[key] = c
        // refine the name with the strings read now (the serial and the product are only readable with the permission)
        val model = UsbCatalog.identify(dev.vid, dev.pid, dev.manufacturer, dev.product) ?: r.model
        put(UsbRadio(dev, model, UsbRadioStatus.READY, model.powerWarning))
        log("ready $key fd ${c.fd} ${model.name}")
    }

    private suspend fun closeLocked(key: String) {
        val c = open.remove(key) ?: return
        try { native.detach(key) } catch (e: Exception) { log("detach failed: ${e.message}") }
        try { c.close() } catch (_: Exception) {}
    }

    companion object {
        const val MSG_DENIED = "Android did not give OnAir access to this radio. Tap it to ask again, or unplug it and plug it in again."
        const val MSG_CANNOT_ASK = "OnAir could not ask Android for access to this radio: unplug it and plug it in again."
        const val MSG_OPEN_FAILED = "Android could not open the radio. Another app may be using it (an rtl_tcp driver, another SDR app): " +
            "close that app, then unplug the radio and plug it in again."
        const val MSG_CORE_REFUSED = "The radio opened but OnAir's driver could not use it. Unplug it, plug it in again, and share the log if it happens again."
        const val MSG_NO_DRIVER = "%s is recognised, but this build of OnAir has no driver for it yet (see the USB notes in the help)."
        /** A session was using a radio that was unplugged. */
        const val MSG_UNPLUGGED = "The radio was unplugged (or lost power). Check the cable and the OTG adapter; a powered hub helps radios that draw a lot."
    }
}
