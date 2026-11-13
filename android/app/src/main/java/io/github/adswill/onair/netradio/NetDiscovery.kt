// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.netradio

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import io.github.adswill.onair.diag.AppLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/** A radio that answered a discovery: what the "Add network radio" dialog can fill in. */
data class FoundRadio(val type: NetRadioType, val name: String, val host: String, val port: Int) {
    fun toForm() = AddNetworkRadioForm(type, host, port.toString(), label = name)
}

/**
 * mDNS / DNS-SD discovery of radio servers on the local network (Android's NsdManager).
 *
 * SAFETY (0.2.9 decision): OFF by default. Nothing is sent to the network unless the user turned "Find radios on this network" on
 * (Settings.netDiscovery) and the dialog that shows the results is open; [stop] ends it. No radio is contacted: only the service
 * announcements are listened to and resolved.
 *
 * The service types are the ones servers announce in practice; rtl_tcp and SpyServer have no official one, so a server is only found
 * when its operator announces it (avahi, a Raspberry Pi image). Not finding one does not mean there is none: the address can be typed.
 */
class NetDiscovery(ctx: Context) {
    private val nsd = ctx.applicationContext.getSystemService(Context.NSD_SERVICE) as? NsdManager
    private val listeners = ArrayList<NsdManager.DiscoveryListener>()
    private val _found = MutableStateFlow<List<FoundRadio>>(emptyList())
    val found: StateFlow<List<FoundRadio>> = _found.asStateFlow()
    @Volatile var running = false
        private set

    fun start() {
        val m = nsd ?: return
        if (running) return
        running = true
        _found.value = emptyList()
        for ((service, type) in SERVICES) {
            val l = object : NsdManager.DiscoveryListener {
                override fun onDiscoveryStarted(t: String) {}
                override fun onDiscoveryStopped(t: String) {}
                override fun onStartDiscoveryFailed(t: String, e: Int) { AppLog.w("net", "discovery $t failed ($e)") }
                override fun onStopDiscoveryFailed(t: String, e: Int) {}
                override fun onServiceLost(s: NsdServiceInfo) { _found.update { l -> l.filterNot { it.name == s.serviceName && it.type == type } } }
                override fun onServiceFound(s: NsdServiceInfo) { if (running) resolve(m, s, type) }
            }
            try { m.discoverServices(service, NsdManager.PROTOCOL_DNS_SD, l); listeners += l } catch (e: Exception) { AppLog.e("net", "discovery $service", e) }
        }
        AppLog.i("net", "mDNS discovery on (${SERVICES.size} service types)")
    }

    fun stop() {
        val m = nsd ?: return
        if (!running) return
        running = false
        for (l in listeners) try { m.stopServiceDiscovery(l) } catch (_: Exception) {}
        listeners.clear()
        AppLog.i("net", "mDNS discovery off")
    }

    @Suppress("DEPRECATION")   // resolveService: the callback form; its replacement (registerServiceInfoCallback) is Android 14+ only
    private fun resolve(m: NsdManager, s: NsdServiceInfo, type: NetRadioType) {
        try {
            m.resolveService(s, object : NsdManager.ResolveListener {
                override fun onResolveFailed(si: NsdServiceInfo, e: Int) {}
                override fun onServiceResolved(si: NsdServiceInfo) {
                    val host = (if (Build.VERSION.SDK_INT >= 34) si.hostAddresses.firstOrNull() else si.host)?.hostAddress ?: return
                    val f = FoundRadio(type, si.serviceName, host.substringBefore('%'), si.port)
                    _found.update { l -> (l.filterNot { it.host == f.host && it.port == f.port && it.type == f.type } + f).sortedBy { it.name } }
                }
            })
        } catch (_: Exception) {}
    }

    companion object {
        /** DNS-SD service type -> the radio type it is. */
        val SERVICES: List<Pair<String, NetRadioType>> = listOf(
            "_rtl_tcp._tcp." to NetRadioType.RTL_TCP,
            "_spyserver._tcp." to NetRadioType.SPYSERVER,
            "_kiwisdr._tcp." to NetRadioType.KIWISDR,
            "_sdrpp._tcp." to NetRadioType.SDRPP,
        )
    }
}
