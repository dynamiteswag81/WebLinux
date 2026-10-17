// WebLinux machine: owns the shared memory and the workers, and routes
// device traffic between the kernel and the page.
//
//   const m = new Machine({ memoryMiB, cmdline, onConsole, onHalt, ... });
//   await m.boot();

'use strict';

export class Machine extends EventTarget {
  constructor(opts = {}) {
    super();
    this.memoryMiB = opts.memoryMiB || 512;
    this.cmdline = opts.cmdline || 'console=ttyW0 rdinit=/init';
    this.kernelUrl = opts.kernelUrl || 'vmlinux.wasm';
    this.initrdUrl = opts.initrdUrl || 'initramfs.cpio';
    this.disk = { backend: opts.disk || 'opfs', name: opts.diskName || 'disk0.img',
      sizeMiB: opts.diskSizeMiB || 256 };
    this.workers = new Map();     // wctl address -> Worker
    this.dev = {};                // device name -> address in memory
    this.timer = null;
    this.halted = false;
    this.stats = { spawned: 0, exited: 0 };
  }

  async boot() {
    if (!self.crossOriginIsolated)
      throw new Error('This page must be cross-origin isolated (run it through server.js).');

    const pages = (this.memoryMiB * 1024 * 1024) / 65536;
    this.memory = new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
    this.u8 = new Uint8Array(this.memory.buffer);
    this.i32 = new Int32Array(this.memory.buffer);
    this.clock = new SharedArrayBuffer(8);
    this.base = performance.timeOrigin + performance.now();

    const [kmodule, initrd] = await Promise.all([
      WebAssembly.compileStreaming(fetch(this.kernelUrl)),
      fetch(this.initrdUrl).then((r) => (r.ok ? r.arrayBuffer() : null)).catch(() => null),
    ]);
    this.kmodule = kmodule;

    this._spawn({ role: 'boot', cmdline: this.cmdline, initrd }, 'boot', initrd ? [initrd] : []);
  }

  _spawn(msg, key, transfer = []) {
    const w = new Worker(new URL('./kworker.js', import.meta.url));
    w.onmessage = (ev) => this._onMessage(w, key, ev.data);
    w.onerror = (ev) => this._emit('log', `worker error: ${ev.message}\n`);
    w.postMessage({
      ...msg,
      module: this.kmodule,
      memory: this.memory,
      clock: this.clock,
      base: this.base,
    }, transfer);
    this.workers.set(key, w);
    this.stats.spawned++;
    return w;
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  _onMessage(w, key, m) {
    switch (m.t) {
      case 'console':
        this._emit('console', m.s);
        break;
      case 'spawn':
        this._spawn({ role: 'task', task: m.task, wctl: m.wctl, ksp: m.ksp, tls: m.tls }, m.wctl);
        break;
      case 'kill': {
        const victim = this.workers.get(m.wctl);
        if (victim) victim.terminate();
        this.workers.delete(m.wctl);
        break;
      }
      case 'exited':
        this.workers.delete(key);
        this.stats.exited++;
        break;
      case 'dev':
        this.dev[m.name] = m.addr;
        this._emit('device', m);
        if (m.name === 'blk') this._startDisk(m.addr);
        if (m.name === 'net') this._startNet(m.addr);
        if (this.dev.irq && this.dev.timer_deadline && this.dev.timer_seq && !this.timer)
          this._startTimer();
        break;
      case 'halt':
        this.halt(m.code);
        break;
      case 'oops':
        this._emit('console', `\n*** wasm kernel fault: ${m.msg}\n${m.stack || ''}\n`);
        this.halt(2);
        break;
    }
  }

  _startTimer() {
    this.timer = new Worker(new URL('./timer.js', import.meta.url));
    this.timer.postMessage({
      memory: this.memory, clock: this.clock, base: this.base,
      irq: this.dev.irq, deadline: this.dev.timer_deadline, seq: this.dev.timer_seq,
    });
  }

  _startDisk(state) {
    const w = new Worker(new URL('./disk.js', import.meta.url));
    w.onmessage = ({ data }) => {
      if (data.t === 'log') this._emit('log', data.s);
      if (data.t === 'ready') this._emit('disk', data);
    };
    w.postMessage({ memory: this.memory, state, irq: this.dev.irq, ...this.disk });
    this.diskWorker = w;
    // release the disk file promptly so a reloaded page can open it
    addEventListener('pagehide', () => w.terminate());
  }

  _startNet(state) {
    const w = new Worker(new URL('./net.js', import.meta.url));
    w.onmessage = ({ data }) => { if (data.t === 'log') this._emit('log', data.s); };
    const relayUrl = this.relayUrl
      || `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/net`;
    w.postMessage({ memory: this.memory, state, irq: this.dev.irq, relayUrl });
    this.netWorker = w;
  }

  // Feed bytes to ttyW0 through the console input ring.
  consoleInput(text) {
    const base = this.dev.console;
    if (!base) return;
    const RING = 4096;
    const bytes = new TextEncoder().encode(text);
    const hIdx = base >> 2;
    let head = Atomics.load(this.i32, hIdx) >>> 0;
    const tail = Atomics.load(this.i32, hIdx + 1) >>> 0;
    for (const b of bytes) {
      if (((head - tail) >>> 0) >= RING) break;
      this.u8[base + 8 + (head % RING)] = b;
      head = (head + 1) >>> 0;
    }
    Atomics.store(this.i32, hIdx, head | 0);
    this.raiseIrq(1);
  }

  // Queue input events [dev, type, code, value] (see arch/wasm/drivers/wasminput.c).
  inputEvents(evs) {
    const base = this.dev.input;
    if (!base) return;
    const RING = 1024;
    const hIdx = base >> 2;
    let head = Atomics.load(this.i32, hIdx) >>> 0;
    const tail = Atomics.load(this.i32, hIdx + 1) >>> 0;
    const dv = this.dv || (this.dv = new DataView(this.memory.buffer));
    for (const [d, type, code, value] of evs) {
      if (((head - tail) >>> 0) >= RING) break;
      const p = base + 8 + (head % RING) * 12;
      dv.setUint16(p, d, true);
      dv.setUint16(p + 2, type, true);
      dv.setUint16(p + 4, code, true);
      dv.setInt32(p + 8, value, true);
      head = (head + 1) >>> 0;
    }
    Atomics.store(this.i32, hIdx, head | 0);
    this.raiseIrq(2);
  }

  raiseIrq(n) {
    const idx = this.dev.irq >> 2;
    Atomics.or(this.i32, idx, 1 << n);
    Atomics.notify(this.i32, idx);
  }

  halt(code) {
    if (this.halted) return;
    this.halted = true;
    for (const w of this.workers.values()) w.terminate();
    this.workers.clear();
    if (this.timer) this.timer.terminate();
    if (this.diskWorker) this.diskWorker.terminate();
    if (this.netWorker) this.netWorker.terminate();
    this._emit('halt', code);
  }
}
