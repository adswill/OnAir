package io.github.adswill.onair.tv

/*
 * MPEG transport stream to PES packets (ISO/IEC 13818-1 2.4.3 and 2.4.3.6), for the PIDs of the chosen service. Pure Kotlin: no
 * Android classes, so the JVM tests run it (TsDemuxTest). The core already parsed PAT/PMT/SDT; this only reassembles the elementary
 * streams that go to the phone's decoders.
 */

const val TS_PACKET = 188
/** PTS and DTS are 33-bit counters of a 90 kHz clock: they wrap after about 26.5 hours. */
const val PTS_MODULUS = 1L shl 33
const val NO_PTS = Long.MIN_VALUE

/** One PES packet: [pts] / [dts] in 90 kHz ticks as sent (33 bits, [NO_PTS] when absent); [data] is the payload after the PES header. */
class Pes(
    val pid: Int,
    val streamId: Int,
    val pts: Long,
    val dts: Long,
    val data: ByteArray,
    /** Packets of it were lost (continuity counter jump), flagged with an error, or it was cut short: the decoder may see garbage. */
    val damaged: Boolean,
    /** The adaptation field of its first packet set random_access_indicator (a key frame starts here, if the multiplexer says so). */
    val randomAccess: Boolean,
)

/** Reads the 33-bit timestamp of a PES header at [o] (the 5 bytes with marker bits). */
fun readPts(b: ByteArray, o: Int): Long {
    val b0 = b[o].toInt() and 0xFF
    val b1 = b[o + 1].toInt() and 0xFF
    val b2 = b[o + 2].toInt() and 0xFF
    val b3 = b[o + 3].toInt() and 0xFF
    val b4 = b[o + 4].toInt() and 0xFF
    return ((b0.toLong() shr 1) and 7L shl 30) or (b1.toLong() shl 22) or ((b2.toLong() shr 1) shl 15) or (b3.toLong() shl 7) or (b4.toLong() shr 1)
}

/** 90 kHz ticks to microseconds. */
fun ptsToUs(ticks: Long): Long = ticks * 100 / 9

/**
 * Turns the wrapping 33-bit PTS into a steadily growing count: each value is placed in the 2^33 period closest to the previous one, so
 * a wrap (and a small step back, e.g. B-frames) never looks like a 26-hour jump. One per clock (one for the whole service: audio and
 * video share the program clock).
 */
class PtsUnwrapper {
    private var last = NO_PTS
    fun reset() { last = NO_PTS }
    fun unwrap(pts33: Long): Long {
        if (pts33 == NO_PTS) return NO_PTS
        val p = pts33 and (PTS_MODULUS - 1)
        if (last == NO_PTS) { last = p; return p }
        val base = last - Math.floorMod(last, PTS_MODULUS)
        var best = base + p
        for (c in longArrayOf(base + p - PTS_MODULUS, base + p + PTS_MODULUS)) if (Math.abs(c - last) < Math.abs(best - last)) best = c
        last = best
        return best
    }
}

/** Reassembles the PES packets of one PID from its transport packets. */
class PesAssembler(val pid: Int, private val sink: (Pes) -> Unit) {
    private var buf = ByteArray(64 * 1024)
    private var len = 0
    private var started = false
    private var damaged = false
    private var rai = false
    private var lastCc = -1
    /** Bytes the PES says it has (6 + PES_packet_length), 0 when unbounded (video). */
    private var expected = 0

    var pesCount = 0L; private set
    var ccErrors = 0L; private set

    /** Feeds one 188-byte packet at [o] of [p] (its PID was checked by the caller). */
    fun feed(p: ByteArray, o: Int) {
        val b1 = p[o + 1].toInt() and 0xFF
        val b3 = p[o + 3].toInt() and 0xFF
        val tei = b1 and 0x80 != 0
        val pusi = b1 and 0x40 != 0
        val afc = (b3 shr 4) and 3
        val cc = b3 and 15
        var payload = 4
        var discontinuity = false
        var randomAccess = false
        if (afc and 2 != 0) {
            val al = p[o + 4].toInt() and 0xFF
            if (al > 183) { damaged = true; return }
            if (al > 0) {
                val f = p[o + 5].toInt() and 0xFF
                discontinuity = f and 0x80 != 0
                randomAccess = f and 0x40 != 0
            }
            payload = 5 + al
        }
        if (afc and 1 == 0) return   // adaptation field only: no payload, the counter does not advance
        if (tei) { damaged = true; lastCc = -1; return }
        if (lastCc >= 0 && !discontinuity) {
            if (cc == lastCc) return   // a duplicate packet (allowed once): ignore
            if (cc != (lastCc + 1) and 15) { ccErrors++; damaged = true }
        }
        lastCc = cc
        if (pusi) {
            if (started) emit()
            started = true; len = 0; damaged = false; rai = randomAccess; expected = 0
        }
        if (!started) return   // joined in the middle of a PES: wait for the next start
        append(p, o + payload, TS_PACKET - payload)
        if (expected == 0 && len >= 6) {
            if (buf[0].toInt() != 0 || buf[1].toInt() != 0 || buf[2].toInt() != 1) { started = false; len = 0; return }   // not a PES
            val pl = ((buf[4].toInt() and 0xFF) shl 8) or (buf[5].toInt() and 0xFF)
            expected = if (pl == 0) -1 else 6 + pl
        }
        if (expected > 0 && len >= expected) { emit(); started = false }
    }

    /** Hands out a PES still being collected (end of stream, service change). */
    fun flush() { if (started) emit(); started = false; len = 0 }

    /** Forgets everything (a jump in the stream: the next PES start begins afresh). */
    fun reset() { started = false; len = 0; lastCc = -1; damaged = false }

    private fun append(src: ByteArray, from: Int, n: Int) {
        if (n <= 0) return
        if (len + n > buf.size) {
            if (buf.size >= 8 shl 20) { damaged = true; return }   // a runaway PES (lost start): stop growing at 8 MB
            buf = buf.copyOf(maxOf(buf.size * 2, len + n))
        }
        System.arraycopy(src, from, buf, len, n)
        len += n
    }

    private fun emit() {
        if (len < 9) return
        val end = if (expected > 0) minOf(expected, len) else len
        val cut = expected > 0 && len < expected
        val sid = buf[3].toInt() and 0xFF
        // stream ids without the optional header (13818-1 table 2-21 and 2.4.3.7): program_stream_map, padding, private_stream_2, ECM, EMM, DSM-CC, H.222.1 type E, directory
        val noHeader = sid == 0xBC || sid == 0xBE || sid == 0xBF || sid == 0xF0 || sid == 0xF1 || sid == 0xF2 || sid == 0xF8 || sid == 0xFF
        var pts = NO_PTS
        var dts = NO_PTS
        val start: Int
        if (noHeader) start = 6 else {
            val flags = buf[7].toInt() and 0xFF
            val hl = buf[8].toInt() and 0xFF
            start = 9 + hl
            if (start > end) return
            val ptsDts = flags shr 6
            if (ptsDts >= 2 && hl >= 5) pts = readPts(buf, 9)
            if (ptsDts == 3 && hl >= 10) dts = readPts(buf, 14)
        }
        pesCount++
        sink(Pes(pid, sid, pts, if (dts == NO_PTS) pts else dts, buf.copyOfRange(start, end), damaged || cut, rai))
    }
}

/**
 * The packets of several PIDs to their PES packets. Finds the 188-byte packet boundaries itself (a buffer that starts out of step is
 * re-synchronised on two sync bytes 188 apart), so it can be fed any chunk of the stream.
 */
class TsDemux(private val sink: (Pes) -> Unit) {
    private val assemblers = HashMap<Int, PesAssembler>()
    private val carry = ByteArray(TS_PACKET)
    private var carryLen = 0
    var packets = 0L; private set
    var syncLosses = 0L; private set

    fun setPids(pids: Collection<Int>) {
        assemblers.keys.retainAll(pids.toSet())
        for (p in pids) assemblers.getOrPut(p) { PesAssembler(p, sink) }
    }

    fun pids(): Set<Int> = assemblers.keys.toSet()
    fun ccErrors(): Long = assemblers.values.sumOf { it.ccErrors }

    fun feed(b: ByteArray, n: Int = b.size) {
        var i = 0
        if (carryLen > 0) {   // finish the packet cut at the end of the last chunk
            val need = TS_PACKET - carryLen
            if (n < need) { System.arraycopy(b, 0, carry, carryLen, n); carryLen += n; return }
            System.arraycopy(b, 0, carry, carryLen, need)
            carryLen = 0
            if (carry[0] == SYNC) packet(carry, 0)
            i = need
        }
        while (i + TS_PACKET <= n) {
            if (b[i] != SYNC || (i + TS_PACKET < n && b[i + TS_PACKET] != SYNC)) {
                syncLosses++
                var j = i + 1
                while (j + TS_PACKET <= n && !(b[j] == SYNC && (j + TS_PACKET >= n || b[j + TS_PACKET] == SYNC))) j++
                for (a in assemblers.values) a.reset()
                i = j
                continue
            }
            packet(b, i)
            i += TS_PACKET
        }
        if (i < n) { System.arraycopy(b, i, carry, 0, n - i); carryLen = n - i }
    }

    fun flush() { for (a in assemblers.values) a.flush() }
    fun reset() { carryLen = 0; for (a in assemblers.values) a.reset() }

    private fun packet(b: ByteArray, o: Int) {
        packets++
        val pid = ((b[o + 1].toInt() and 0x1F) shl 8) or (b[o + 2].toInt() and 0xFF)
        assemblers[pid]?.feed(b, o)
    }

    private companion object { const val SYNC: Byte = 0x47 }
}
