// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.ui.radio

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import io.github.adswill.onair.diag.AppLog
import io.github.adswill.onair.model.SourceKind
import io.github.adswill.onair.ui.NetworkRadio
import java.util.concurrent.ConcurrentHashMap

/** The mDNS service types of the network radios OnAir can use. */
private val serviceTypes = listOf("_rtl_tcp._tcp." to SourceKind.RTL_TCP, "_spyserver._tcp." to SourceKind.SPYSERVER)

/**
 * Looks for rtl_tcp and SpyServer servers on the local network with Android's NsdManager (mDNS), only while [enabled] (the user
 * opted in; off by default) and only while the caller is on screen. Nothing is sent anywhere but the local network's multicast
 * queries. Servers that do not announce themselves are added by address.
 */
@Composable
fun rememberDiscovery(enabled: Boolean): State<List<NetworkRadio>> {
    val ctx = LocalContext.current
    val found = remember { mutableStateOf(emptyList<NetworkRadio>()) }
    DisposableEffect(enabled) {
        if (!enabled) { found.value = emptyList(); return@DisposableEffect onDispose {} }
        val nsd = ctx.applicationContext.getSystemService(Context.NSD_SERVICE) as? NsdManager
        if (nsd == null) return@DisposableEffect onDispose {}
        val seen = ConcurrentHashMap<String, NetworkRadio>()
        val main = android.os.Handler(android.os.Looper.getMainLooper())
        fun publish() { main.post { found.value = seen.values.sortedBy { it.name } } }
        val listeners = serviceTypes.map { (type, kind) ->
            object : NsdManager.DiscoveryListener {
                override fun onDiscoveryStarted(t: String) { AppLog.i("app", "network discovery started: $t") }
                override fun onDiscoveryStopped(t: String) {}
                override fun onStartDiscoveryFailed(t: String, code: Int) { AppLog.w("app", "network discovery failed: $t ($code)") }
                override fun onStopDiscoveryFailed(t: String, code: Int) {}
                override fun onServiceLost(info: NsdServiceInfo) { seen.remove(info.serviceName + kind.code); publish() }
                override fun onServiceFound(info: NsdServiceInfo) {
                    try {
                        @Suppress("DEPRECATION")
                        nsd.resolveService(info, object : NsdManager.ResolveListener {
                            override fun onResolveFailed(i: NsdServiceInfo, code: Int) {}
                            override fun onServiceResolved(i: NsdServiceInfo) {
                                @Suppress("DEPRECATION")
                                val host = i.host?.hostAddress ?: return
                                seen[i.serviceName + kind.code] = NetworkRadio(kind, i.serviceName, host, i.port)
                                publish()
                            }
                        })
                    } catch (e: Exception) { AppLog.w("app", "could not resolve ${info.serviceName}", e) }
                }
            }.also { l -> try { nsd.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, l) } catch (e: Exception) { AppLog.w("app", "discovery not started", e) } }
        }
        onDispose { for (l in listeners) try { nsd.stopServiceDiscovery(l) } catch (_: Exception) {} }
    }
    return found
}
