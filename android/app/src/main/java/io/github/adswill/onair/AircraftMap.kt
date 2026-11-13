// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

package io.github.adswill.onair

import io.github.adswill.onair.model.ResultTable

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.location.LocationManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.TextUnitType
import androidx.compose.ui.unit.dp
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import kotlin.math.*

/** One marker: an aircraft (ICAO) or an AIS vessel (MMSI). */
private class Ac(val icao: String, val call: String, val lat: Double, val lon: Double, val hdg: Double, val alt: Int?, val layer: String = "Aircraft", val color: Color? = null)

/** Low = warm, high = cool, as on the desktop map. */
private fun altColor(alt: Int?): Color {
    if (alt == null) return Color(0xFF8C9095)
    val f = (alt / 40000f).coerceIn(0f, 1f)
    val hue = 20f + f * 200f // orange-red -> cyan-blue
    return Color.hsv(hue, 0.75f, 1f)
}
private val layerColors = listOf(Color(0xFF00A3A3), Color(0xFFE040A0), Color(0xFFB8A000), Color(0xFF7B61FF), Color(0xFF3DA35D), Color(0xFFE67E22))
private fun markColor(a: Ac) = a.color ?: altColor(a.alt)

private fun lastLocation(ctx: Context): Pair<Double, Double>? = try {
    if (ctx.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
        ctx.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) null
    else {
        val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        lm.getProviders(true).mapNotNull { lm.getLastKnownLocation(it) }.maxByOrNull { it.time }?.let { it.latitude to it.longitude }
    }
} catch (_: Throwable) { null }

// ---- OpenStreetMap raster tiles: on demand for the visible area only, disk cache >= 7 days, 2 downloads at a time ----

private val TILE_UA = "OnAir-Android/" + io.github.adswill.onair.BuildConfig.VERSION_NAME + " (+https://github.com/adswill/OnAir)"
private const val MIN_AGE_MS = 7L * 24 * 3600 * 1000

private object TileStore {
    private val mem = object : LinkedHashMap<Long, ImageBitmap>(96, 0.75f, true) {
        override fun removeEldestEntry(e: MutableMap.MutableEntry<Long, ImageBitmap>?) = size > 80
    }
    private val lock = Object()
    private val pending = LinkedHashSet<Long>()
    private val inflight = HashSet<Long>()
    private val failedAt = HashMap<Long, Long>()
    private var started = false
    var dir: File? = null
    var tick by mutableIntStateOf(0)
    @Volatile var loadedAny = false
    @Volatile var failures = 0

    fun key(z: Int, x: Int, y: Int) = (z.toLong() shl 56) or (x.toLong() shl 28) or y.toLong()
    private fun kz(k: Long) = (k shr 56).toInt()
    private fun kx(k: Long) = ((k shr 28) and 0xFFFFFFF).toInt()
    private fun ky(k: Long) = (k and 0xFFFFFFF).toInt()

    fun get(k: Long): ImageBitmap? = synchronized(lock) { mem[k] }

    /** Forget what was queued for earlier frames: only what is on screen now gets downloaded. */
    fun beginFrame() = synchronized(lock) { pending.clear() }

    fun want(k: Long) {
        synchronized(lock) {
            if (mem.containsKey(k) || inflight.contains(k)) return
            val f = failedAt[k]; if (f != null && System.currentTimeMillis() - f < 30_000) return
            pending.add(k); lock.notifyAll()
        }
        startWorkers()
    }

    private fun startWorkers() {
        synchronized(lock) { if (started) return; started = true }
        repeat(2) { i ->
            Thread({
                while (true) {
                    val k: Long
                    synchronized(lock) {
                        while (pending.isEmpty()) lock.wait()
                        k = pending.last() // newest request first
                        pending.remove(k); inflight.add(k)
                    }
                    try { load(k) } catch (_: Throwable) { fail(k) }
                    synchronized(lock) { inflight.remove(k) }
                }
            }, "osm-tile-$i").apply { isDaemon = true; start() }
        }
    }

    private fun fail(k: Long) { synchronized(lock) { failedAt[k] = System.currentTimeMillis() }; failures++; tick++ }

    @Volatile private var blocked = false   // a 403 or 429 from the tile server: no more requests this session (OSM tile usage policy)

    private fun load(k: Long) {
        val z = kz(k); val x = kx(k); val y = ky(k)
        val f = dir?.let { File(it, "$z/$x/$y.png") }
        var stale = false
        if (f != null && f.exists() && f.length() > 0) {
            stale = System.currentTimeMillis() - f.lastModified() > MIN_AGE_MS
            val bmp = BitmapFactory.decodeFile(f.path)
            if (bmp != null) {
                synchronized(lock) { mem[k] = bmp.asImageBitmap() }; loadedAny = true; tick++
                if (!stale) return
            }
        }
        if (blocked) { if (!stale) fail(k); return }
        val c = URL("https://tile.openstreetmap.org/$z/$x/$y.png").openConnection() as HttpURLConnection
        try {
            c.setRequestProperty("User-Agent", TILE_UA)
            c.connectTimeout = 8000; c.readTimeout = 8000
            val rc = c.responseCode
            if (rc == 403 || rc == 429) blocked = true
            if (rc != 200) { if (!stale) fail(k); return }
            val bytes = c.inputStream.use { it.readBytes() }
            val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            if (bmp == null) { if (!stale) fail(k); return }
            if (f != null) { f.parentFile?.mkdirs(); val t = File(f.path + ".tmp"); t.writeBytes(bytes); t.renameTo(f) }
            synchronized(lock) { mem[k] = bmp.asImageBitmap(); failedAt.remove(k) }
            loadedAny = true; tick++
        } catch (e: Throwable) { if (!stale) fail(k) } finally { c.disconnect() }
    }
}

private fun mercX(lon: Double) = (lon + 180.0) / 360.0
private fun mercY(lat: Double): Double {
    val s = sin(Math.toRadians(lat.coerceIn(-85.0, 85.0)))
    return 0.5 - ln((1 + s) / (1 - s)) / (4 * PI)
}

private val latNames = listOf("Lat", "Latitude"); private val lonNames = listOf("Lon", "Long", "Longitude")
private val posNames = listOf("Pos", "Position")
private val labelNames = listOf("Callsign", "Name", "Flight", "Registration", "Satellite", "Short", "Id", "ICAO")

/** True when the table carries positions: Lat/Lon columns or a "Position" column. Detected by name, so new modes need nothing extra. */
fun hasPositions(tab: ResultTable): Boolean =
    (latNames.any { tab.cols.contains(it) } && lonNames.any { tab.cols.contains(it) }) || posNames.any { tab.cols.contains(it) }

private fun layerName(tab: ResultTable) = when {
    tab.cols.contains("ICAO") && tab.cols.contains("Hdg") -> "Aircraft"
    tab.cols.contains("MMSI") -> "Vessels"
    tab.cols.contains("Symbol") -> "APRS"
    else -> tab.name
}

private fun parseMarks(tabs: List<ResultTable>): List<Ac> {
    val out = ArrayList<Ac>()
    tabs.filter { hasPositions(it) }.forEachIndexed { ti, tab ->
        val layer = layerName(tab)
        val col = if (layer == "Aircraft") null else layerColors[ti % layerColors.size]
        fun c(r: List<String>, n: String) = r.getOrNull(tab.cols.indexOf(n)) ?: ""
        val latC = latNames.firstOrNull { tab.cols.contains(it) }; val lonC = lonNames.firstOrNull { tab.cols.contains(it) }
        val posC = posNames.firstOrNull { tab.cols.contains(it) }
        val lab = labelNames.firstOrNull { tab.cols.contains(it) }
        for (r in tab.rows) {
            val lat: Double?; val lon: Double?
            if (latC != null && lonC != null) { lat = c(r, latC).toDoubleOrNull(); lon = c(r, lonC).toDoubleOrNull() }
            else { val q = c(r, posC!!).split(",", " ").filter { it.isNotBlank() }.map { it.trim().toDoubleOrNull() }; lat = q.getOrNull(0); lon = q.getOrNull(1) }
            if (lat == null || lon == null || abs(lat) > 90 || abs(lon) > 180 || (lat == 0.0 && lon == 0.0)) continue
            out.add(Ac(r.firstOrNull() ?: "", (if (lab != null) c(r, lab) else "").trim(), lat, lon, c(r, "Hdg").toDoubleOrNull() ?: Double.NaN,
                (c(r, "Alt ft").toIntOrNull()), layer, col))
        }
    }
    return out
}

/** Aircraft (or AIS vessels) on an OpenStreetMap slippy map. Until the user says yes, and when tiles cannot be had, the offline plot is shown. */
@Composable
fun PositionMap(
    tabs: List<ResultTable>, selected: String?, onSelect: (String?) -> Unit, consent: Int, onConsent: (Int) -> Unit, modifier: Modifier = Modifier,
    /** Extra drawing on top of the tile map (walk-around DF heat map); receives the lat/lon -> screen function. Not used by the offline plot. */
    overlay: (androidx.compose.ui.graphics.drawscope.DrawScope.((Double, Double) -> Offset) -> Unit)? = null,
    /** Centre the map here instead of on the last known location, at [startZoom]. */
    homeOverride: Pair<Double, Double>? = null, startZoom: Float? = null,
) {
    val p = LocalPalette.current
    val ctx = LocalContext.current
    val all = remember(tabs) { parseMarks(tabs) }
    val layers = remember(all) { all.map { it.layer }.distinct() }
    var hidden by remember { mutableStateOf(setOf<String>()) }
    val list = remember(all, hidden) { all.filter { it.layer !in hidden } }
    val trails = remember { HashMap<String, ArrayList<Pair<Double, Double>>>() }
    remember(list) {
        for (a in list) {
            val t = trails.getOrPut(a.icao) { ArrayList() }
            val l = t.lastOrNull()
            if (l == null || l.first != a.lat || l.second != a.lon) { t.add(a.lat to a.lon); if (t.size > 30) t.removeAt(0) }
        }
        0
    }
    val home = homeOverride ?: remember { lastLocation(ctx) }
    TileStore.dir = File(ctx.cacheDir, "osm-tiles")
    val failed = TileStore.failures >= 4 && !TileStore.loadedAny
    Column(modifier) {
        if (consent == 0) {
            Row(Modifier.fillMaxWidth().padding(bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Show the map with OpenStreetMap tiles? Tiles are downloaded from tile.openstreetmap.org for the area you look at.",
                    Modifier.weight(1f), color = p.text, fontSize = TextUnit(11f, TextUnitType.Sp))
                Text("Yes", Modifier.clickable { onConsent(1) }.padding(8.dp), color = p.accent, fontFamily = Mono)
                Text("No", Modifier.clickable { onConsent(2) }.padding(8.dp), color = p.muted, fontFamily = Mono)
            }
        }
        if (layers.size > 1) Row(Modifier.fillMaxWidth().padding(bottom = 4.dp)) {
            for (l in layers) {
                val on = l !in hidden
                Text((if (on) "\u25CF " else "\u25CB ") + l, Modifier.clickable { hidden = if (on) hidden + l else hidden - l }.padding(end = 12.dp, top = 2.dp, bottom = 2.dp),
                    color = if (on) p.text else p.muted, fontFamily = Mono, fontSize = TextUnit(11f, TextUnitType.Sp))
            }
        }
        val m = Modifier.fillMaxWidth().weight(1f)
        if (consent != 1 || failed) OfflinePlot(list, trails, home, selected, onSelect, m, if (consent == 1) "offline - map tiles unavailable" else null)
        else SlippyMap(list, trails, home, selected, onSelect, m, overlay, startZoom)
    }
}

@Composable
private fun SlippyMap(list: List<Ac>, trails: Map<String, List<Pair<Double, Double>>>, home: Pair<Double, Double>?, selected: String?, onSelect: (String?) -> Unit, modifier: Modifier,
    overlay: (androidx.compose.ui.graphics.drawscope.DrawScope.((Double, Double) -> Offset) -> Unit)? = null, startZoom: Float? = null) {
    val p = LocalPalette.current
    val dens = LocalDensity.current.density
    var size by remember { mutableStateOf(IntSize.Zero) }
    var cx by remember { mutableDoubleStateOf(0.5) }
    var cy by remember { mutableDoubleStateOf(0.5) }
    var z by remember { mutableFloatStateOf(3f) }
    var inited by remember { mutableStateOf(false) }
    val cur by rememberUpdatedState(list)
    fun worldPx(zz: Float = z) = 256.0 * dens * 2.0.pow(zz.toDouble())
    fun recentre() {
        if (size.width == 0) return
        val pts = list.map { mercX(it.lon) to mercY(it.lat) }
        val c = if (home != null) mercX(home.second) to mercY(home.first) else if (pts.isEmpty()) 0.5 to 0.5 else pts.sumOf { it.first } / pts.size to pts.sumOf { it.second } / pts.size
        cx = c.first; cy = c.second
        val span = (pts.maxOfOrNull { max(abs(it.first - c.first), abs(it.second - c.second)) } ?: 0.0) * 2.4
        z = if (pts.isEmpty()) (if (home != null) (startZoom ?: 8f) else 3f)
            else if (span <= 1e-7) 8f
            else log2(min(size.width, size.height) / (256.0 * dens) / span).toFloat().coerceIn(3f, 12f)
    }
    fun toScreen(lat: Double, lon: Double): Offset {
        val w = worldPx()
        return Offset((size.width / 2.0 + (mercX(lon) - cx) * w).toFloat(), (size.height / 2.0 + (mercY(lat) - cy) * w).toFloat())
    }
    LaunchedEffect(size, list.isEmpty()) { if (size.width > 0 && !inited && (list.isNotEmpty() || home != null)) { recentre(); inited = true } }
    Box(modifier.background(p.plate).border(1.dp, p.border)) {
        Canvas(Modifier.fillMaxSize()
            .pointerInput(Unit) {
                detectTransformGestures { cen, d, zf, _ ->
                    val oldW = worldPx()
                    val nz = (z + log2(zf.toDouble()).toFloat()).coerceIn(3f, 12f)
                    val nw = worldPx(nz)
                    val dx = cen.x - size.width / 2.0; val dy = cen.y - size.height / 2.0
                    val fx = cx + dx / oldW; val fy = cy + dy / oldW
                    cx = (fx - dx / nw - d.x / nw).coerceIn(0.0, 1.0)
                    cy = (fy - dy / nw - d.y / nw).coerceIn(0.0, 1.0)
                    z = nz
                }
            }
            .pointerInput(Unit) {
                detectTapGestures { o ->
                    val l = cur
                    val hit = l.minByOrNull { (toScreen(it.lat, it.lon) - o).getDistance() }
                    onSelect(if (hit != null && (toScreen(hit.lat, hit.lon) - o).getDistance() < 48f) hit.icao else null)
                }
            }) {
            size = IntSize(this.size.width.toInt(), this.size.height.toInt())
            @Suppress("UNUSED_VARIABLE") val t = TileStore.tick // redraw when a tile arrives
            val tz = floor(z).toInt().coerceIn(3, 12)
            val ts = (256.0 * dens * 2.0.pow((z - tz).toDouble()))
            val w = worldPx()
            val n = 1 shl tz
            val left = cx * w - size.width / 2.0; val top = cy * w - size.height / 2.0
            val x0 = floor(left / ts).toInt().coerceAtLeast(0); val x1 = floor((left + size.width) / ts).toInt().coerceAtMost(n - 1)
            val y0 = floor(top / ts).toInt().coerceAtLeast(0); val y1 = floor((top + size.height) / ts).toInt().coerceAtMost(n - 1)
            TileStore.beginFrame()
            for (ty in y0..y1) for (tx in x0..x1) {
                val k = TileStore.key(tz, tx, ty)
                val img = TileStore.get(k)
                if (img == null) { TileStore.want(k); continue }
                val ox = (tx * ts - left).roundToInt(); val oy = (ty * ts - top).roundToInt()
                val ex = ((tx + 1) * ts - left).roundToInt(); val ey = ((ty + 1) * ts - top).roundToInt()
                drawImage(img, dstOffset = IntOffset(ox, oy), dstSize = IntSize(ex - ox + 1, ey - oy + 1))
            }
            if (home != null) { val h = toScreen(home.first, home.second); drawCircle(Color(0xFF1A73E8), 9f, h); drawCircle(Color.White, 9f, h, style = Stroke(3f)) }
            drawMarks(cur, trails, selected, ::toScreen, onMap = true, text = Color(0xFF111418), halo = Color.White)
            overlay?.invoke(this, ::toScreen)
        }
        Text("© OpenStreetMap contributors", Modifier.align(Alignment.BottomEnd).background(Color.White.copy(alpha = 0.8f)).padding(horizontal = 4.dp, vertical = 1.dp),
            color = Color(0xFF222222), fontSize = TextUnit(9f, TextUnitType.Sp))
        Text("⌖", Modifier.align(Alignment.TopEnd).padding(6.dp).background(Color.White.copy(alpha = 0.85f)).clickable { recentre() }.padding(horizontal = 10.dp, vertical = 4.dp),
            color = Color(0xFF111418), fontSize = TextUnit(18f, TextUnitType.Sp))
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawMarks(
    list: List<Ac>, trails: Map<String, List<Pair<Double, Double>>>, selected: String?, toScreen: (Double, Double) -> Offset,
    onMap: Boolean, text: Color, halo: Color?) {
    val lab = android.graphics.Paint().apply { textSize = 24f; isAntiAlias = true }
    val hp = android.graphics.Paint().apply { textSize = 24f; isAntiAlias = true; style = android.graphics.Paint.Style.STROKE; strokeWidth = 5f; color = (halo ?: Color.Transparent).toArgb() }
    for (a in list) {
        val o = toScreen(a.lat, a.lon)
        val col = markColor(a)
        val tr = trails[a.icao]
        if (tr != null && tr.size > 1) {
            val path = Path()
            tr.forEachIndexed { i, q -> val s = toScreen(q.first, q.second); if (i == 0) path.moveTo(s.x, s.y) else path.lineTo(s.x, s.y) }
            drawPath(path, col.copy(alpha = if (onMap) 0.85f else 0.5f), style = Stroke(if (onMap) 3f else 2f))
        }
        val sel = a.icao == selected
        if (sel) drawCircle(if (onMap) Color(0xFF111418) else Color.White, 22f, o, style = Stroke(3f))
        if (onMap) { // dark outline so the colour reads on the map
            rotate(if (a.hdg.isNaN()) 0f else a.hdg.toFloat(), o) {
                val t = Path().apply { moveTo(o.x, o.y - 17f); lineTo(o.x + 12f, o.y + 13f); lineTo(o.x, o.y + 7f); lineTo(o.x - 12f, o.y + 13f); close() }
                if (a.hdg.isNaN()) drawCircle(Color(0xFF111418), 9f, o) else drawPath(t, Color(0xFF111418), style = Stroke(5f))
            }
        }
        rotate(if (a.hdg.isNaN()) 0f else a.hdg.toFloat(), o) {
            val t = Path().apply { moveTo(o.x, o.y - 14f); lineTo(o.x + 9f, o.y + 10f); lineTo(o.x, o.y + 5f); lineTo(o.x - 9f, o.y + 10f); close() }
            if (a.hdg.isNaN()) drawCircle(col, 7f, o) else drawPath(t, col)
        }
        val s = a.call.ifBlank { a.icao }
        if (halo != null) drawContext.canvas.nativeCanvas.drawText(s, o.x + 16f, o.y - 12f, hp)
        lab.color = (if (sel && !onMap) Color.White else text).toArgb()
        drawContext.canvas.nativeCanvas.drawText(s, o.x + 16f, o.y - 12f, lab)
    }
}

/** Offline plot: dark plate, grid, range rings, north up. Pinch to zoom, drag to pan, tap to select. */
@Composable
private fun OfflinePlot(list: List<Ac>, trails: Map<String, List<Pair<Double, Double>>>, home: Pair<Double, Double>?, selected: String?, onSelect: (String?) -> Unit, modifier: Modifier, note: String?) {
    val p = LocalPalette.current
    val centre = home ?: if (list.isEmpty()) null else (list.sumOf { it.lat } / list.size) to (list.sumOf { it.lon } / list.size)
    var zoom by remember { mutableFloatStateOf(1f) }
    var pan by remember { mutableStateOf(Offset.Zero) }
    // pixels per km at zoom 1: fit the farthest marker
    val maxKm = remember(list, centre) {
        if (centre == null) 100.0 else max(30.0, (list.maxOfOrNull { hypot((it.lat - centre.first) * 111.0, (it.lon - centre.second) * 111.0 * cos(Math.toRadians(centre.first))) } ?: 100.0) * 1.15)
    }
    var size by remember { mutableStateOf(IntSize.Zero) }
    fun scale() = (min(size.width, size.height) / 2f) / maxKm.toFloat() * zoom
    fun toScreen(lat: Double, lon: Double): Offset {
        val c = centre ?: return Offset.Zero
        val k = scale()
        val x = (lon - c.second) * 111.0 * cos(Math.toRadians(c.first)) * k
        val y = -(lat - c.first) * 111.0 * k
        return Offset(size.width / 2f + pan.x + x.toFloat(), size.height / 2f + pan.y + y.toFloat())
    }
    Box(modifier.background(p.plate).border(1.dp, p.border)) {
        Canvas(Modifier.fillMaxSize()
            .pointerInput(Unit) { detectTransformGestures { _, d, z, _ -> zoom = (zoom * z).coerceIn(0.3f, 40f); pan += d } }
            .pointerInput(list, centre, zoom, pan) {
                detectTapGestures { o ->
                    val hit = list.minByOrNull { (toScreen(it.lat, it.lon) - o).getDistance() }
                    onSelect(if (hit != null && (toScreen(hit.lat, hit.lon) - o).getDistance() < 48f) hit.icao else null)
                }
            }) {
            size = IntSize(this.size.width.toInt(), this.size.height.toInt())
            val cx = this.size.width / 2f + pan.x; val cy = this.size.height / 2f + pan.y
            val k = scale()
            var step = 50f
            while (step * k < 40f) step *= 2f
            val gs = step * k
            var x = cx - ceil(cx / gs) * gs
            while (x < this.size.width) { drawLine(p.grid, Offset(x, 0f), Offset(x, this.size.height), 1f); x += gs }
            var y = cy - ceil(cy / gs) * gs
            while (y < this.size.height) { drawLine(p.grid, Offset(0f, y), Offset(this.size.width, y), 1f); y += gs }
            val ringPaint = android.graphics.Paint().apply { color = p.muted.toArgb(); textSize = 26f; isAntiAlias = true }
            for (i in 1..8) {
                val r = i * step * k
                if (r > hypot(this.size.width, this.size.height) * 2) break
                drawCircle(p.muted.copy(alpha = 0.35f), r, Offset(cx, cy), style = Stroke(1.5f))
                drawContext.canvas.nativeCanvas.drawText("${(i * step).toInt()} km", cx + 6f, cy - r + 26f, ringPaint)
            }
            drawContext.canvas.nativeCanvas.drawText("N", this.size.width - 36f, 36f, ringPaint.apply { color = p.accent.toArgb() })
            if (home != null) { drawCircle(p.accent, 8f, Offset(cx, cy), style = Stroke(3f)); drawCircle(p.accent, 3f, Offset(cx, cy)) }
            drawMarks(list, trails, selected, ::toScreen, onMap = false, text = p.text, halo = null)
        }
        Text((note ?: (if (home == null) "position unknown - centred on markers" else "your position")) + "  ·  ${list.size} with position",
            Modifier.align(Alignment.BottomStart).padding(6.dp), color = p.muted, fontFamily = Mono, fontSize = TextUnit(10f, TextUnitType.Sp))
    }
}
