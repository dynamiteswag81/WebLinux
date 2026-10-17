// Disk worker: the backing store of the kernel's wasmblk device (/dev/wda).
//
// The store is a file in the Origin Private File System, written through a
// FileSystemSyncAccessHandle, so it survives closing the tab.  Without OPFS
// (or when another tab holds the file) it falls back to a sparse in-memory
// disk.  Protocol: arch/wasm/drivers/wasmblk.c.

'use strict';

// struct wasm_blk_state offsets
const READY = 0, RO = 4, SECTORS = 8, DOORBELL = 16, DONE = 20, OP = 24, STATUS = 28,
  SECTOR = 32, NSEG = 40, SEG = 48;
const OP_READ = 0, OP_WRITE = 1, OP_FLUSH = 2, OP_DISCARD = 3;
const IRQ_BLOCK = 3;
const EIO = 5;

class MemStore {
  constructor(size) { this.size = size; this.chunks = new Map(); }
  _chunk(i, create) {
    let c = this.chunks.get(i);
    if (!c && create) { c = new Uint8Array(65536); this.chunks.set(i, c); }
    return c;
  }
  read(dst, at) {
    for (let o = 0; o < dst.length;) {
      const i = Math.floor((at + o) / 65536), off = (at + o) % 65536;
      const n = Math.min(65536 - off, dst.length - o);
      const c = this._chunk(i, false);
      if (c) dst.set(c.subarray(off, off + n), o); else dst.fill(0, o, o + n);
      o += n;
    }
  }
  write(src, at) {
    for (let o = 0; o < src.length;) {
      const i = Math.floor((at + o) / 65536), off = (at + o) % 65536;
      const n = Math.min(65536 - off, src.length - o);
      this._chunk(i, true).set(src.subarray(o, o + n), off);
      o += n;
    }
  }
  discard(at, len) {
    // drop whole chunks inside the range, zero the partial ends
    const z = (a, n) => { if (n > 0) this.write(new Uint8Array(n), a); };
    const first = Math.ceil(at / 65536), last = Math.floor((at + len) / 65536);
    if (first >= last) return z(at, len);
    z(at, first * 65536 - at);
    for (let i = first; i < last; i++) this.chunks.delete(i);
    z(last * 65536, at + len - last * 65536);
  }
  flush() {}
}

class OpfsStore {
  constructor(handle) { this.h = handle; this.size = handle.getSize(); }
  read(dst, at) {
    const n = this.h.read(dst, { at });
    if (n < dst.length) dst.fill(0, n);
  }
  write(src, at) { this.h.write(src, { at }); }
  discard(at, len) {
    // no hole punching in OPFS: write zeros so reads match
    const z = new Uint8Array(Math.min(len, 1 << 20));
    for (let o = 0; o < len; o += z.length)
      this.h.write(z.subarray(0, Math.min(z.length, len - o)), { at: at + o });
  }
  flush() { this.h.flush(); }
}

async function openStore(backend, name, sizeMiB) {
  const size = sizeMiB * 1024 * 1024;
  if (backend !== 'mem' && self.navigator && navigator.storage && navigator.storage.getDirectory) {
    try {
      const root = await navigator.storage.getDirectory();
      const fh = await root.getFileHandle(name, { create: true });
      // after a reload the previous page's worker may still hold the file
      let h;
      for (let tries = 0; ; tries++) {
        try { h = await fh.createSyncAccessHandle(); break; } catch (e) {
          if (e.name !== 'NoModificationAllowedError' || tries >= 150) throw e;
          await new Promise((r) => setTimeout(r, 100));
        }
      }
      if (h.getSize() === 0) h.truncate(size);
      return { store: new OpfsStore(h), kind: 'opfs' };
    } catch (e) {
      self.postMessage({ t: 'log', s: `disk: browser storage unavailable (${e.message}); using a memory disk\n` });
    }
  }
  return { store: new MemStore(size), kind: 'mem' };
}

self.onmessage = async ({ data }) => {
  const { memory, state, irq, backend, name, sizeMiB } = data;
  const i32 = new Int32Array(memory.buffer);
  const u8 = new Uint8Array(memory.buffer);
  const dv = new DataView(memory.buffer);
  const S = (o) => (state + o) >> 2;

  if (backend === 'none') {
    Atomics.store(i32, S(READY), 2);
    Atomics.notify(i32, S(READY));
    return;
  }
  const { store, kind } = await openStore(backend, name || 'disk0.img', sizeMiB || 256);
  dv.setBigUint64(state + SECTORS, BigInt(Math.floor(store.size / 512)), true);
  Atomics.store(i32, S(RO), 0);
  Atomics.store(i32, S(READY), 1);
  Atomics.notify(i32, S(READY));
  self.postMessage({ t: 'ready', kind, size: store.size });

  let bounce = new Uint8Array(1 << 20);
  let dirty = false;
  let done = Atomics.load(i32, S(DONE));
  for (;;) {
    // wake at least every second to flush written data
    if (Atomics.load(i32, S(DOORBELL)) === done) {
      if (Atomics.wait(i32, S(DOORBELL), done, 1000) === 'timed-out' && dirty) {
        try { store.flush(); } catch (e) { /* retried next time */ }
        dirty = false;
      }
      continue;
    }
    const seq = Atomics.load(i32, S(DOORBELL));
    const op = dv.getUint32(state + OP, true);
    let at = Number(dv.getBigUint64(state + SECTOR, true)) * 512;
    const nseg = dv.getUint32(state + NSEG, true);
    let status = 0;
    try {
      if (op === OP_READ || op === OP_WRITE) {
        for (let i = 0; i < nseg; i++) {
          const addr = dv.getUint32(state + SEG + i * 8, true);
          const len = dv.getUint32(state + SEG + i * 8 + 4, true);
          if (at + len > store.size) throw new Error('beyond end of disk');
          if (bounce.length < len) bounce = new Uint8Array(len);
          const b = bounce.subarray(0, len);
          if (op === OP_READ) { store.read(b, at); u8.set(b, addr); }
          else { b.set(u8.subarray(addr, addr + len)); store.write(b, at); dirty = true; }
          at += len;
        }
      } else if (op === OP_FLUSH) {
        store.flush();
        dirty = false;
      } else if (op === OP_DISCARD) {
        store.discard(at, dv.getUint32(state + SEG + 4, true));
        dirty = true;
      }
    } catch (e) {
      status = -EIO;
      self.postMessage({ t: 'log', s: `disk: ${e.message}\n` });
    }
    dv.setInt32(state + STATUS, status, true);
    done = seq;
    Atomics.store(i32, S(DONE), seq);
    Atomics.or(i32, irq >> 2, 1 << IRQ_BLOCK);
    Atomics.notify(i32, irq >> 2);
  }
};
