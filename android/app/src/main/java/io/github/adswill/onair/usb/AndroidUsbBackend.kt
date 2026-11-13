// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import androidx.core.content.ContextCompat
import io.github.adswill.onair.diag.AppLog

/**
 * [UsbBackend] on Android's UsbManager. No runtime permission is involved: each radio gets the system's per-device dialog
 * (requestPermission), or none at all when the user ticked "always open OnAir for this device" in the dialog that the
 * USB_DEVICE_ATTACHED filter of MainActivity brings up (res/xml/usb_device_filter.xml).
 *
 * [listen] registers one receiver for the permission answers (our own broadcast, explicit to this package) and the system's
 * attach / detach broadcasts, and forwards them.
 */
class AndroidUsbBackend(private val ctx: Context) : UsbBackend {
    private val um: UsbManager? = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager

    override val hostSupported: Boolean =
        um != null && ctx.packageManager.hasSystemFeature(PackageManager.FEATURE_USB_HOST)

    private fun device(name: String): UsbDevice? = um?.deviceList?.get(name)

    override fun devices(): List<UsbDeviceRef> = um?.deviceList?.values?.map { ref(it) } ?: emptyList()

    override fun hasPermission(name: String): Boolean = device(name)?.let { um?.hasPermission(it) } ?: false

    override fun requestPermission(name: String): Boolean {
        val d = device(name) ?: return false
        // FLAG_MUTABLE: the system adds EXTRA_DEVICE and EXTRA_PERMISSION_GRANTED to it; explicit package, as Android 14 requires of a mutable intent
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
        val pi = PendingIntent.getBroadcast(ctx, name.hashCode(), Intent(ACTION_PERMISSION).setPackage(ctx.packageName), flags)
        return try { um?.requestPermission(d, pi); true } catch (e: Exception) { AppLog.e("usb", "permission request failed", e); false }
    }

    override fun open(name: String): OpenedUsb? {
        val d = device(name) ?: return null
        val c: UsbDeviceConnection = try { um?.openDevice(d) } catch (e: Exception) { AppLog.e("usb", "openDevice failed", e); null } ?: return null
        val serial = try { c.serial } catch (_: Exception) { null }
        return object : OpenedUsb {
            override val fd: Int = c.fileDescriptor
            override val serial: String? = serial
            override fun close() = c.close()
        }
    }

    /**
     * Registers the receiver; [onAttach] / [onDetach] / [onPermission] run on the main thread (keep them short: post to a scope).
     * Returns a function that unregisters it.
     */
    fun listen(onAttach: (UsbDeviceRef) -> Unit, onDetach: (String) -> Unit, onPermission: (String, Boolean) -> Unit): () -> Unit {
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                val d = deviceOf(i) ?: return
                when (i.action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> onAttach(ref(d))
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> onDetach(d.deviceName)
                    ACTION_PERMISSION -> onPermission(d.deviceName, i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
                }
            }
        }
        val f = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(ACTION_PERMISSION)
        }
        // not exported: the permission answer is our own explicit broadcast; attach/detach are system broadcasts, delivered anyway
        ContextCompat.registerReceiver(ctx, r, f, ContextCompat.RECEIVER_NOT_EXPORTED)
        return { try { ctx.unregisterReceiver(r) } catch (_: Exception) {} }
    }

    companion object {
        const val ACTION_PERMISSION = "io.github.adswill.onair.USB_PERMISSION"

        @Suppress("DEPRECATION")
        fun deviceOf(i: Intent): UsbDevice? =
            if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java) else i.getParcelableExtra(UsbManager.EXTRA_DEVICE)

        /** The strings Android may hide before the permission (serialNumber throws SecurityException on Android 10+): read them carefully. */
        fun ref(d: UsbDevice): UsbDeviceRef {
            fun safe(f: () -> String?): String? = try { f() } catch (_: Exception) { null }
            val power = try { if (d.configurationCount > 0) d.getConfiguration(0).maxPower else 0 } catch (_: Exception) { 0 }
            return UsbDeviceRef(d.deviceName, d.vendorId, d.productId, safe { d.manufacturerName }, safe { d.productName }, safe { d.serialNumber }, power)
        }
    }
}
