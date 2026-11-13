// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

/*
 * The seams of the USB layer, so the logic (UsbRadioController) runs on the JVM with fakes:
 *  - UsbBackend: what Android's UsbManager does (list, permission, open) — AndroidUsbBackend is the real one.
 *  - NativeUsbLink: handing an opened descriptor to the core (RadioNative on the receiver thread) — data/RadioRepository is the real one.
 */

/** A USB device as the phone sees it. [manufacturer] / [product] / [serial] can be null before the permission (Android hides them). */
data class UsbDeviceRef(
    /** Android's device name, e.g. "/dev/bus/usb/001/004": the key everywhere (also in the core's registry). */
    val name: String,
    val vid: Int,
    val pid: Int,
    val manufacturer: String? = null,
    val product: String? = null,
    val serial: String? = null,
    /** What the device's first configuration says it draws (mA), 0 = not known. */
    val maxPowerMa: Int = 0,
)

/** An opened device: its usbfs descriptor stays valid until [close]. */
interface OpenedUsb {
    val fd: Int
    /** The serial number string, readable once opened. */
    val serial: String?
    fun close()
}

interface UsbBackend {
    /** false on a phone without USB host support (android.hardware.usb.host). */
    val hostSupported: Boolean
    fun devices(): List<UsbDeviceRef>
    fun hasPermission(name: String): Boolean
    /** Shows the system dialog; the answer comes back through UsbRadioController.onPermissionResult. false: could not ask. */
    fun requestPermission(name: String): Boolean
    /** Opens the device (the permission must be granted). null: Android refused (another app has it, or it is gone). */
    fun open(name: String): OpenedUsb?
}

interface NativeUsbLink {
    /** Registers the opened radio with the core's drivers. false: the core refused it (bad descriptor). */
    suspend fun attach(dev: UsbDeviceRef, kind: String, fd: Int, serial: String): Boolean
    suspend fun detach(name: String)
    /** What this build of the native library can drive ("rtlsdr", "hackrf", ...). */
    suspend fun supports(kind: String): Boolean
}
