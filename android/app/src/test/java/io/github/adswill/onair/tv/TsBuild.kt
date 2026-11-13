package io.github.adswill.onair.tv

import java.io.ByteArrayOutputStream

/** Builds transport streams for the tests: PES packets with PTS/DTS, cut into 188-byte packets with adaptation-field stuffing. */
object TsBuild {
    private fun ts5(prefix: Int, v: Long): ByteArray {
        val t = v and (PTS_MODULUS - 1)
        return byteArrayOf(
            ((prefix shl 4) or (((t shr 30) and 7).toInt() shl 1) or 1).toByte(),
            (t shr 22).toByte(),
            ((((t shr 15) and 0x7F).toInt() shl 1) or 1).toByte(),
            (t shr 7).toByte(),
            (((t and 0x7F).toInt() shl 1) or 1).toByte(),
        )
    }

    /** A PES packet; [bounded] false gives PES_packet_length 0 (allowed for video). */
    fun pes(streamId: Int, pts: Long?, dts: Long?, payload: ByteArray, bounded: Boolean = true): ByteArray {
        val hdr = ByteArrayOutputStream()
        val flags = if (pts != null && dts != null) 0xC0 else if (pts != null) 0x80 else 0
        if (pts != null && dts != null) { hdr.write(ts5(3, pts)); hdr.write(ts5(1, dts)) } else if (pts != null) hdr.write(ts5(2, pts))
        val h = hdr.toByteArray()
        val len = 3 + h.size + payload.size
        val out = ByteArrayOutputStream()
        out.write(byteArrayOf(0, 0, 1, streamId.toByte()))
        val pl = if (bounded) len else 0
        out.write(pl shr 8); out.write(pl and 0xFF)
        out.write(0x80); out.write(flags); out.write(h.size)
        out.write(h); out.write(payload)
        return out.toByteArray()
    }

    class Packetizer(val pid: Int, var cc: Int = 0) {
        /** The packets of one PES. [randomAccess] sets random_access_indicator on the first. */
        fun packets(pes: ByteArray, randomAccess: Boolean = false): List<ByteArray> {
            val out = ArrayList<ByteArray>()
            var i = 0
            var first = true
            while (i < pes.size) {
                val p = ByteArray(TS_PACKET) { 0xFF.toByte() }
                p[0] = 0x47
                p[1] = ((if (first) 0x40 else 0) or (pid shr 8)).toByte()
                p[2] = pid.toByte()
                val remaining = pes.size - i
                val wantAf = (first && randomAccess) || remaining < 184
                var o = 4
                if (wantAf) {
                    val payloadRoom = minOf(remaining, if (first && randomAccess) 182 else 183)
                    val afLen = 183 - payloadRoom
                    p[3] = (0x30 or (cc and 15)).toByte()
                    p[4] = afLen.toByte()
                    if (afLen > 0) p[5] = (if (first && randomAccess) 0x40 else 0).toByte()
                    o = 5 + afLen
                } else p[3] = (0x10 or (cc and 15)).toByte()
                val n = TS_PACKET - o
                System.arraycopy(pes, i, p, o, n)
                i += n
                cc = (cc + 1) and 15
                first = false
                out += p
            }
            return out
        }
    }

    fun join(parts: List<ByteArray>): ByteArray { val o = ByteArrayOutputStream(); parts.forEach { o.write(it) }; return o.toByteArray() }
}

/** MSB-first bit writer for building headers and parameter sets in the tests. */
class BitWriter {
    private val out = ByteArrayOutputStream()
    private var cur = 0
    private var n = 0
    var bitCount = 0L; private set
    fun bits(v: Long, count: Int): BitWriter {
        for (k in count - 1 downTo 0) {
            cur = (cur shl 1) or ((v shr k) and 1).toInt(); n++; bitCount++
            if (n == 8) { out.write(cur); cur = 0; n = 0 }
        }
        return this
    }
    fun bits(v: Int, count: Int) = bits(v.toLong(), count)
    fun bit(b: Boolean) = bits(if (b) 1 else 0, 1)
    fun ue(v: Int): BitWriter {
        val x = v + 1
        val len = 32 - Integer.numberOfLeadingZeros(x)
        bits(0, len - 1); bits(x, len)
        return this
    }
    fun bytes(b: ByteArray) = apply { for (x in b) bits(x.toInt() and 0xFF, 8) }
    /** Pads to a byte with zeros (or the rbsp stop bit and zeros when [stopBit]). */
    fun toBytes(stopBit: Boolean = false): ByteArray {
        if (stopBit) bits(1, 1)
        while (n != 0) bits(0, 1)
        return out.toByteArray()
    }
}
