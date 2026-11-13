// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair.netradio

/**
 * Network radios on the phone: the core's own network sources (core/include/dect2/net/netradio.h). The list is kept in the settings
 * in the core's text form ("rtltcp,host,port;kiwisdr,host,port,password;..."), so the app and the desktop read the same thing.
 * Pure Kotlin (tested on the JVM). The connect timeout is the core's (8 s, source_rtltcp.cpp kConnectMs).
 */
enum class NetRadioType(
    /** NetRadio::Type in the core. */
    val code: Int,
    /** The key in the settings text (rust/onair_net/src/radios.rs type_key). */
    val key: String,
    val label: String,
    val defaultPort: Int,
    /** Whether the fourth field is used: KiwiSDR password, SoapyRemote driver. */
    val hasRemote: Boolean = false,
    /** Whether the Android build can use it (SoapyRemote needs SoapySDR, which the app does not have). */
    val availableOnAndroid: Boolean = true,
    val help: String = "",
) {
    RTL_TCP(1, "rtltcp", "rtl_tcp", 1234, help = "An RTL-SDR (or another radio) served by rtl_tcp on a computer, a Raspberry Pi or a driver app on this phone."),
    SPYSERVER(3, "spyserver", "SpyServer", 5555, help = "Airspy's SpyServer (Airspy or RTL-SDR on a server)."),
    KIWISDR(8, "kiwisdr", "KiwiSDR", 8073, hasRemote = true, help = "A KiwiSDR web receiver (HF). The password is only for receivers that need one."),
    TCI(10, "tci", "TCI", 50001, help = "A Transceiver Control Interface server (ExpertSDR3 and others), receive only."),
    SDRPP(6, "sdrpp", "SDR++ server", 5259, help = "An SDR++ server (sdrpp --server)."),
    SOAPY_REMOTE(2, "soapyremote", "SoapyRemote", 55132, hasRemote = true, availableOnAndroid = false,
        help = "Needs SoapySDR, which OnAir for Android does not include: run rtl_tcp or SpyServer on the server instead."),
    ;

    companion object {
        fun ofKey(k: String): NetRadioType? = entries.firstOrNull { it.key == k }
        fun ofCode(c: Int): NetRadioType? = entries.firstOrNull { it.code == c }
        /** What the "Add network radio" dialog offers. */
        val offered: List<NetRadioType> get() = entries.filter { it.availableOnAndroid }
    }
}

/** One network radio of the list. [remote]: KiwiSDR password / SoapyRemote driver ("" = none). [label]: the user's own name ("" = none). */
data class NetworkRadio(val type: NetRadioType, val host: String, val port: Int, val remote: String = "", val label: String = "") {
    val portOrDefault: Int get() = if (port > 0) port else type.defaultPort
    /** For lists; the password is never shown. */
    val title: String get() = label.ifBlank { "${type.label}  $host:$portOrDefault" }
    /** The core's text form of this one radio (what the bridge's network source takes). */
    val spec: String get() = buildString {
        append(type.key).append(',').append(host.trim()).append(',').append(portOrDefault)
        if (type.hasRemote && remote.isNotEmpty()) append(',').append(remote)
    }

    companion object {
        /** The list from the settings text; items the core would skip are skipped here too. Labels are kept separately (see [NetworkRadioList]). */
        fun parseList(s: String): List<NetworkRadio> = s.split(';').mapNotNull { item ->
            val p = item.split(',', limit = 4)
            if (p.size < 3) return@mapNotNull null
            val t = NetRadioType.ofKey(p[0].trim()) ?: return@mapNotNull null
            val host = p[1].trim().takeIf { it.isNotEmpty() } ?: return@mapNotNull null
            val port = p[2].trim().toIntOrNull()?.takeIf { it in 0..65535 } ?: 0
            NetworkRadio(t, host, port, if (t.hasRemote && p.size == 4) p[3] else "")
        }

        fun formatList(v: List<NetworkRadio>): String = v.joinToString(";") { it.spec }
    }
}

/**
 * The form of the "Add network radio" dialog (the UI draws it, this holds and checks it). Immutable; [validate] gives the radio or
 * the problems in plain words, per field.
 */
data class AddNetworkRadioForm(
    val type: NetRadioType = NetRadioType.RTL_TCP,
    val host: String = "",
    val port: String = "",
    val remote: String = "",
    val label: String = "",
) {
    enum class Field { HOST, PORT, REMOTE, TYPE }

    data class Result(val radio: NetworkRadio?, val errors: Map<Field, String>)

    /** The type changed: the port field follows the new default unless the user typed one. */
    fun withType(t: NetRadioType): AddNetworkRadioForm = copy(type = t, port = if (port.isBlank() || port == type.defaultPort.toString()) "" else port)

    /** A "host:port" typed in the host field is split (people paste "192.168.1.20:1234"). */
    fun normalised(): AddNetworkRadioForm {
        val h = host.trim().removePrefix("http://").removePrefix("https://").removePrefix("tcp://").trimEnd('/')
        val m = Regex("""^([^:\[\]]+|\[[0-9a-fA-F:]+]):(\d{1,5})$""").find(h)
        return if (m != null && port.isBlank()) copy(host = m.groupValues[1], port = m.groupValues[2]) else copy(host = h)
    }

    fun validate(): Result {
        val f = normalised()
        val e = LinkedHashMap<Field, String>()
        if (!f.type.availableOnAndroid) e[Field.TYPE] = f.type.help
        val h = f.host
        when {
            h.isEmpty() -> e[Field.HOST] = "Type the address of the computer or receiver (a name or an IP address)."
            h.any { it == ',' || it == ';' || it.isWhitespace() } -> e[Field.HOST] = "The address cannot contain spaces, commas or semicolons."
            !HOST.matches(h) -> e[Field.HOST] = "This does not look like an address: use a name like sdr.example.org or an IP like 192.168.1.20."
        }
        val p = if (f.port.isBlank()) 0 else f.port.trim().toIntOrNull() ?: -1
        if (p !in 0..65535) e[Field.PORT] = "The port is a number from 1 to 65535 (empty: ${f.type.defaultPort})."
        if (f.remote.contains(';')) e[Field.REMOTE] = "The password cannot contain a semicolon."
        val radio = if (e.isEmpty()) NetworkRadio(f.type, h, p, if (f.type.hasRemote) f.remote else "", f.label.trim()) else null
        return Result(radio, e)
    }

    companion object {
        private val HOST = Regex("""^(\[[0-9a-fA-F:]+]|[0-9a-fA-F:]*:[0-9a-fA-F:]+|[A-Za-z0-9]([A-Za-z0-9-]{0,62})?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,62})?)*\.?)$""")
    }
}

/** The list as the settings keep it: the core's text plus the user's labels ("spec\tlabel" per line, so the core's text stays clean). */
object NetworkRadioList {
    fun encode(v: List<NetworkRadio>): String = v.joinToString("\n") { it.spec + if (it.label.isNotBlank()) "\t" + it.label.replace('\n', ' ').replace('\t', ' ') else "" }
    fun decode(s: String): List<NetworkRadio> = s.split('\n').filter { it.isNotBlank() }.mapNotNull { line ->
        val tab = line.indexOf('\t')
        val spec = if (tab >= 0) line.substring(0, tab) else line
        NetworkRadio.parseList(spec).firstOrNull()?.copy(label = if (tab >= 0) line.substring(tab + 1) else "")
    }
}
