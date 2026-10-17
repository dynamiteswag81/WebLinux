// WebLinux task worker.
//
// Every Linux task runs in a worker like this one.  The worker instantiates
// the kernel module (vmlinux.wasm) against the machine's shared memory and
// then either becomes the boot CPU's idle task or waits to run one task.
// When that task execs a program, the program's wasm module is instantiated
// here too and calls straight into the kernel's "syscall" export.
//
// Control transfers that unwind the wasm stack (starting a new program,
// resuming a vfork parent, terminating the worker) are JS exceptions thrown
// from kernel imports and caught by run() below.

'use strict';

let memory, u8, dv, kx, clock, BASE, CMDLINE, initrd, kmodule;
let kernelSP = 0;        // shadow stack top of this worker's kernel instance
let user = null;         // the running user program, if any
// compiled user programs; shared through the machine with every new worker
let moduleCache = new Map();

const td = new TextDecoder();
const te = new TextEncoder();

class StartUser { constructor(info) { this.info = info; } }
class ExitWorker {}
class ForkResume { constructor(key, pid) { this.key = key; this.pid = pid; } }

// fork(): Asyncify-saved stacks of parents waiting for their child, by key
const forkStates = new Map();
let forkKey = 0x40000000;
let forkReturn = 0;
const SYS_wasm_vfork = 244, ENOSYS = 38;

const post = (m) => self.postMessage(m);

function cstr(p) {
  let e = p;
  while (u8[e]) e++;
  return td.decode(u8.slice(p, e));
}

// Monotonic nanoseconds, consistent across workers: every worker's
// timeOrigin+now() is on the same clock; clamp to the largest value handed
// out so far so the kernel never sees time go backwards.
function nowNs() {
  const t = BigInt(Math.floor((performance.timeOrigin + performance.now() - BASE) * 1e6));
  let last = Atomics.load(clock, 0);
  while (t > last) {
    const prev = Atomics.compareExchange(clock, 0, last, t);
    if (prev === last) return t;
    last = prev;
  }
  return last;
}

function readStart(p) {
  const w = (i) => dv.getUint32(p + 4 * i, true);
  return {
    image: w(0), image_len: w(1), mem_base: w(2), table_size: w(3),
    stack_top: w(4), args: w(5), tls_base: w(6), pid: w(7),
  };
}

const host = {
  console_write(p, n) {
    post({ t: 'console', s: td.decode(u8.slice(p, p + n)) });
  },
  now_ns: nowNs,
  wall_ns: () => BigInt(Date.now()) * 1000000n,
  get_cmdline(buf, size) {
    const b = te.encode(CMDLINE);
    const n = Math.min(b.length, size - 1);
    u8.set(b.subarray(0, n), buf);
    u8[buf + n] = 0;
    return n;
  },
  initrd_size: () => (initrd ? initrd.byteLength : 0),
  initrd_load(dst) {
    u8.set(new Uint8Array(initrd), dst);
    initrd = null;
  },
  spawn(task, wctl, ksp, tls) {
    post({ t: 'spawn', task, wctl, ksp, tls });
  },
  kill_worker(wctl) {
    post({ t: 'kill', wctl });
  },
  exit_worker() {
    throw new ExitWorker();
  },
  start_user(p) {
    throw new StartUser(readStart(p));
  },
  vfork_resume(jmpbuf, val) {
    // longjmp(jmpbuf, val) inside the user program: throw its C longjmp
    // exception.  Nothing of the kernel's stack survives the unwind.
    kx.__stack_pointer.value = kernelSP;
    if (forkStates.has(jmpbuf)) throw new ForkResume(jmpbuf, val);
    if (!user || !user.longjmpTag) throw new Error('vfork_resume without a user program');
    dv.setUint32(jmpbuf + user.ljArgOff, jmpbuf, true);
    dv.setInt32(jmpbuf + user.ljArgOff + 4, val, true);
    throw new WebAssembly.Exception(user.longjmpTag, [jmpbuf + user.ljArgOff]);
  },
  halt(code) {
    post({ t: 'halt', code });
    // park this worker for good
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0);
  },
  get_random(buf, len) {
    // getRandomValues refuses views of shared memory: go through a copy
    const t = new Uint8Array(len);
    for (let o = 0; o < len; o += 65536) crypto.getRandomValues(t.subarray(o, Math.min(len, o + 65536)));
    u8.set(t, buf);
  },
  dev_register(name, addr) {
    post({ t: 'dev', name: cstr(name), addr });
  },
};

function instantiateKernel() {
  const inst = new WebAssembly.Instance(kmodule, { env: { memory }, host });
  kx = inst.exports;
  if (kx.__tls_size.value > 64) throw new Error('kernel TLS block too big');
}

// ---------------------------------------------------------------- user code

function userImports(mod, info, table) {
  const imports = {};
  const need = (m) => (imports[m] ||= {});
  for (const imp of WebAssembly.Module.imports(mod)) {
    const ns = need(imp.module);
    const key = imp.module + '.' + imp.name;
    switch (key) {
      case 'env.memory': ns[imp.name] = memory; break;
      case 'env.__indirect_function_table': ns[imp.name] = table; break;
      case 'env.__memory_base':
        ns[imp.name] = new WebAssembly.Global({ value: 'i32', mutable: false }, info.mem_base); break;
      case 'env.__table_base':
        ns[imp.name] = new WebAssembly.Global({ value: 'i32', mutable: false }, 1); break;
      case 'env.__stack_pointer':
        ns[imp.name] = info.sp = new WebAssembly.Global({ value: 'i32', mutable: true }, info.stack_top); break;
      case 'env.__tls_base':
        ns[imp.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, info.tls_base); break;
      case 'linux.syscall': ns[imp.name] = kx.syscall; break;
      case 'linux.fork': ns[imp.name] = forkImport; break;
      default:
        // GOT entries of undefined weak symbols: null
        if ((imp.module === 'GOT.func' || imp.module === 'GOT.mem') && imp.kind === 'global') {
          ns[imp.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, 0);
          break;
        }
        // undefined weak functions: only reached through a null check
        if (imp.module === 'env' && imp.kind === 'function') {
          ns[imp.name] = () => { throw new Error(`call to undefined function ${imp.name}`); };
          break;
        }
        throw new Error(`unresolved import ${key} (${imp.kind})`);
    }
  }
  return imports;
}

function prepareUser(info) {
  const bytes = u8.slice(info.image, info.image + info.image_len);
  const key = info.image_len + ':' + hash(bytes);
  let mod = moduleCache.get(key);
  if (!mod) {
    mod = new WebAssembly.Module(bytes);
    moduleCache.set(key, mod);
    post({ t: 'module', key, mod });
  }
  const table = new WebAssembly.Table({ element: 'anyfunc', initial: 1 + info.table_size });
  const inst = new WebAssembly.Instance(mod, userImports(mod, info, table));
  const ux = inst.exports;
  user = {
    inst, table, info,
    longjmpTag: ux.__c_longjmp || null,
    ljArgOff: 8, // jmp_buf: id, label, then {env, val} (musl rt.c)
  };
  // no kernel frames survive into user mode
  kx.__stack_pointer.value = kernelSP;
  if (ux.__wasm_init_tls) ux.__wasm_init_tls(info.tls_base);
  if (ux.__wasm_apply_data_relocs) ux.__wasm_apply_data_relocs();
  if (ux.__wasm_apply_global_relocs) ux.__wasm_apply_global_relocs();
  return () => ux._start(info.args);
}

// The program called fork().  First time: save its wasm stack with Asyncify
// (it unwinds back out of _start into run()).  When rewound: return.
function forkImport(buf, size) {
  const ux = user.inst.exports;
  if (!ux.asyncify_start_unwind) return -ENOSYS;
  if (ux.asyncify_get_state() === 2) {
    ux.asyncify_stop_rewind();
    return forkReturn;
  }
  dv.setUint32(buf, buf + 8, true);
  dv.setUint32(buf + 4, buf + size, true);
  // Asyncify restores locals but not the shadow stack pointer global:
  // remember where it was so the replayed frames find their memory.
  user.pendingFork = { buf, sp: user.info.sp.value };
  user.unwinding = true;
  ux.asyncify_start_unwind(buf);
  return 0;
}

// Replay a saved stack so that fork() returns @value.
function rewind(st, value) {
  const u = st.user;
  u8.set(st.bytes, st.buf);
  forkReturn = value;
  user = u;
  u.info.sp.value = st.sp;
  kx.__stack_pointer.value = kernelSP;
  u.inst.exports.asyncify_start_rewind(st.buf);
  return () => u.inst.exports._start(u.info.args);
}

function afterUnwind() {
  const u = user;
  u.unwinding = false;
  u.inst.exports.asyncify_stop_unwind();
  const { buf, sp } = u.pendingFork;
  const end = dv.getUint32(buf, true);
  const st = { user: u, buf, sp, bytes: u8.slice(buf, end) };
  const key = forkKey++;
  forkStates.set(key, st);
  kx.__stack_pointer.value = kernelSP;
  // the kernel snapshots our memory and hands this worker to the child
  const r = kx.syscall(SYS_wasm_vfork, key, 1, 0, 0, 0, 0);
  if (r < 0) {
    forkStates.delete(key);
    return rewind(st, r);
  }
  return rewind(st, 0);
}

function hash(b) {
  let h = 2166136261;
  for (let i = 0; i < b.length; i += 97) h = Math.imul(h ^ b[i], 16777619);
  return (h >>> 0).toString(16);
}

function trapKind(e) {
  const m = String(e && e.message);
  if (/unreachable/.test(m)) return 1;
  if (/out of bounds/.test(m)) return 2;
  if (/call stack|recursion/i.test(m)) return 3;
  if (/signature|indirect/.test(m)) return 4;
  if (/divide|integer/.test(m)) return 5;
  return 9;
}

// ---------------------------------------------------------------- main loop

function run(fn) {
  for (;;) {
    try {
      fn();
      if (user && user.unwinding) {
        fn = afterUnwind();
        continue;
      }
      post({ t: 'oops', msg: 'kernel entry point returned' });
      return;
    } catch (e) {
      if (e instanceof StartUser) {
        try {
          fn = prepareUser(e.info);
        } catch (err) {
          post({ t: 'console', s: `wasm: can't start program: ${err.message}\n` });
          kx.__stack_pointer.value = kernelSP;
          fn = () => kx.user_fault(8);
        }
        continue;
      }
      if (e instanceof ForkResume) {
        const st = forkStates.get(e.key);
        forkStates.delete(e.key);
        fn = rewind(st, e.pid);
        continue;
      }
      if (e instanceof ExitWorker) {
        post({ t: 'exited' });
        self.close();
        return;
      }
      let inUser = 0;
      try { inUser = kx.in_user(); } catch (_) { /* kernel is wedged */ }
      if (inUser) {
        post({ t: 'console', s: `wasm: user trap: ${e && e.message}\n` });
        kx.__stack_pointer.value = kernelSP;
        fn = () => kx.user_fault(trapKind(e));
        continue;
      }
      post({ t: 'oops', msg: String(e && e.message || e), stack: String(e && e.stack || '') });
      return;
    }
  }
}

self.onmessage = (ev) => {
  const d = ev.data;
  memory = d.memory;
  kmodule = d.module;
  if (d.modules) moduleCache = d.modules;
  clock = new BigInt64Array(d.clock);
  BASE = d.base;
  u8 = new Uint8Array(memory.buffer);
  dv = new DataView(memory.buffer);
  try {
    instantiateKernel();
  } catch (e) {
    post({ t: 'oops', msg: 'instantiate: ' + e.message, stack: String(e.stack) });
    return;
  }

  if (d.role === 'boot') {
    CMDLINE = d.cmdline;
    initrd = d.initrd;
    kx.__wasm_init_tls(kx.boot_tls());
    kernelSP = kx.boot_stack_top();
    kx.__stack_pointer.value = kernelSP;
    run(() => kx.boot());
  } else {
    kx.__wasm_init_tls(d.tls);
    kernelSP = d.ksp;
    kx.__stack_pointer.value = kernelSP;
    run(() => kx.task_entry(d.task));
  }
};
