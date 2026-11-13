// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.core.content.ContextCompat
import io.github.adswill.onair.diag.AppLog

/**
 * Runtime permissions the app asks for, in one place. A permission is only asked for when the manifest declares it and the phone's
 * Android version has it; anything else counts as granted, so callers never need version checks.
 *
 *  - NOTIFICATIONS (Android 13+): the foreground service's notification. Asked on the first start; the receiver runs either way.
 *  - RECORD_AUDIO: audio-input modes (sound card / microphone as a source). Not declared yet: the modes agent declares it with its mode.
 *  - LOCATION: direction finding and look angles later. Not declared yet.
 *  - USB radios use UsbManager.requestPermission per device, not a runtime permission: the USB agent adds it next to the device code.
 */
enum class AppPermission(val manifest: String, val sinceSdk: Int) {
    NOTIFICATIONS(Manifest.permission.POST_NOTIFICATIONS, 33),   // a compile-time constant: fine on older Android
    RECORD_AUDIO(Manifest.permission.RECORD_AUDIO, 1),
    LOCATION(Manifest.permission.ACCESS_FINE_LOCATION, 1),
}

object Permissions {
    fun declared(ctx: Context, p: AppPermission): Boolean = try {
        @Suppress("DEPRECATION")
        ctx.packageManager.getPackageInfo(ctx.packageName, PackageManager.GET_PERMISSIONS).requestedPermissions?.contains(p.manifest) == true
    } catch (_: Exception) { false }

    fun granted(ctx: Context, p: AppPermission): Boolean =
        Build.VERSION.SDK_INT < p.sinceSdk || ContextCompat.checkSelfPermission(ctx, p.manifest) == PackageManager.PERMISSION_GRANTED

    /** Declared, available on this Android, and not granted yet. */
    fun needsRequest(ctx: Context, p: AppPermission): Boolean = declared(ctx, p) && !granted(ctx, p)
}

/**
 * A function that asks for [AppPermission]s (only those that need it) and then runs the callback with the result, granted or not.
 * Use from a composable: `val ask = rememberPermissionRequest(ctx); ask(listOf(AppPermission.NOTIFICATIONS)) { vm.toggle() }`.
 */
@Composable
fun rememberPermissionRequest(ctx: Context): (List<AppPermission>, (Map<AppPermission, Boolean>) -> Unit) -> Unit {
    val pending = remember { arrayOfNulls<Pair<List<AppPermission>, (Map<AppPermission, Boolean>) -> Unit>>(1) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { res ->
        val (asked, done) = pending[0] ?: return@rememberLauncherForActivityResult
        pending[0] = null
        val out = asked.associateWith { res[it.manifest] ?: Permissions.granted(ctx, it) }
        out.forEach { (p, ok) -> AppLog.i("app", "permission ${p.name}: ${if (ok) "granted" else "denied"}") }
        done(out)
    }
    return remember(launcher) {
        val ask: (List<AppPermission>, (Map<AppPermission, Boolean>) -> Unit) -> Unit = { perms, done ->
            val need = perms.filter { Permissions.needsRequest(ctx, it) }
            if (need.isEmpty()) done(perms.associateWith { Permissions.granted(ctx, it) })
            else { pending[0] = need to done; launcher.launch(need.map { it.manifest }.toTypedArray()) }
        }
        ask
    }
}
