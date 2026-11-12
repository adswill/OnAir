// A zip reader with the same rules as OnAir's own (core/src/plugin/zip.cpp): at most 64 entries, no zip64, no encryption, stored or deflate
// only, safe names (no absolute paths, no "..", no backslash or colon, at most 4 levels), size limits, and the CRC of every entry checked.

export const MAX_ZIP_ENTRIES = 64;

export interface ZipEntry { name: string; method: number; crc: number; csize: number; usize: number; localOffset: number; dir: boolean }

const rd16 = (d: Uint8Array, p: number) => d[p] | (d[p + 1] << 8);
const rd32 = (d: Uint8Array, p: number) => (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (d[p + 3] << 24)) >>> 0;

// the same test as plugin::safeZipName
export function safeZipName(n: string): boolean {
  if (!n || n.length > 200 || n[0] === "/" || n.includes("\\") || n.includes(":")) return false;
  const parts = n.split("/");
  if (parts.length && parts[parts.length - 1] === "") parts.pop();   // a trailing slash: a directory entry
  if (parts.length > 4) return false;
  for (const p of parts) if (p === "" || p === "." || p === "..") return false;
  for (let i = 0; i < n.length; i++) if (n.charCodeAt(i) < 32) return false;
  return true;
}

let crcTable: Uint32Array | null = null;
export function crc32(d: Uint8Array): number {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[i] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < d.length; i++) c = crcTable[(c ^ d[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export class ZipError extends Error {}

export class Zip {
  readonly entries: ZipEntry[] = [];
  constructor(private d: Uint8Array, maxEntryBytes: number, maxTotalBytes: number) {
    if (d.length < 22) throw new ZipError("not a zip file");
    let eocd = -1;
    const lo = Math.max(0, d.length - 22 - 65535);
    for (let i = d.length - 22; i >= lo; i--) if (rd32(d, i) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new ZipError("not a zip file (no directory)");
    const count = rd16(d, eocd + 10), cdSize = rd32(d, eocd + 12), cdOff = rd32(d, eocd + 16);
    if (count === 0xffff || cdOff === 0xffffffff) throw new ZipError("zip64 files are not supported");
    if (count > MAX_ZIP_ENTRIES) throw new ZipError("the package has more than " + MAX_ZIP_ENTRIES + " entries");
    if (cdOff + cdSize > d.length) throw new ZipError("damaged zip directory");
    let p = cdOff, total = 0;
    const td = new TextDecoder();
    for (let i = 0; i < count; i++) {
      if (p + 46 > d.length || rd32(d, p) !== 0x02014b50) throw new ZipError("damaged zip directory");
      const flags = rd16(d, p + 8);
      const e: ZipEntry = { name: "", method: rd16(d, p + 10), crc: rd32(d, p + 16), csize: rd32(d, p + 20), usize: rd32(d, p + 24), localOffset: rd32(d, p + 42), dir: false };
      const nl = rd16(d, p + 28), xl = rd16(d, p + 30), cl = rd16(d, p + 32);
      if (p + 46 + nl + xl + cl > d.length) throw new ZipError("damaged zip directory");
      e.name = td.decode(d.subarray(p + 46, p + 46 + nl));
      p += 46 + nl + xl + cl;
      if (flags & 1) throw new ZipError("encrypted zip entries are not supported");
      if (!safeZipName(e.name)) throw new ZipError("unsafe file name in the package: " + e.name.slice(0, 60));
      e.dir = e.name.endsWith("/");
      total += e.usize;
      if (e.usize > maxEntryBytes || total > maxTotalBytes) throw new ZipError("the package unpacks to more than the size limit");
      if (!e.dir && e.method !== 0 && e.method !== 8) throw new ZipError("unsupported compression in " + e.name);
      this.entries.push(e);
    }
  }

  async read(e: ZipEntry): Promise<Uint8Array> {
    const d = this.d, lo = e.localOffset;
    if (lo + 30 > d.length || rd32(d, lo) !== 0x04034b50) throw new ZipError("damaged zip entry " + e.name);
    const start = lo + 30 + rd16(d, lo + 26) + rd16(d, lo + 28);
    if (start + e.csize > d.length) throw new ZipError("damaged zip entry " + e.name);
    const src = d.subarray(start, start + e.csize);
    let out: Uint8Array;
    if (e.method === 0) {
      if (e.csize !== e.usize) throw new ZipError("damaged zip entry " + e.name);
      out = src;
    } else {
      out = await inflateRaw(src, e.usize);
      if (out.length !== e.usize) throw new ZipError("cannot unpack " + e.name);
    }
    if (crc32(out) !== e.crc) throw new ZipError("checksum error in " + e.name);
    return out;
  }
}

// raw deflate with a hard cap on the output (a zip bomb stops at its declared size + 1 byte)
async function inflateRaw(src: Uint8Array, expect: number): Promise<Uint8Array> {
  const ds = new DecompressionStream("deflate-raw");
  const w = ds.writable.getWriter();
  w.write(src).catch(() => {});
  w.close().catch(() => {});
  const r = ds.readable.getReader();
  const out = new Uint8Array(expect);
  let n = 0;
  try {
    for (;;) {
      const { done, value } = await r.read();
      if (done) break;
      if (n + value.length > expect) { r.cancel().catch(() => {}); throw new ZipError("an entry unpacks to more than it says"); }
      out.set(value, n);
      n += value.length;
    }
  } catch (x) {
    if (x instanceof ZipError) throw x;
    throw new ZipError("cannot unpack a damaged entry");
  }
  return out.subarray(0, n);
}
