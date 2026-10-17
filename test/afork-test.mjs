import fs from 'fs';
const mod = new WebAssembly.Module(fs.readFileSync(process.argv[2]));
const memory = new WebAssembly.Memory({ initial: 512, maximum: 512, shared: true });
const u8 = new Uint8Array(memory.buffer), dv = new DataView(memory.buffer);
const BASE = 1 << 20, STACK = 4 << 20, BUF = 6 << 20;
const SPG = new WebAssembly.Global({ value: "i32", mutable: true }, 4 << 20);
let inst, phase = 0, saved = null, out = '';
class Exit extends Error {}
let trace = []; const sys = (n, a, b, c) => { if (trace.length < 40) trace.push(n); else if (trace.length === 40) { trace.push("..."); console.log("syscalls", trace.join(",")); }
  if (n === 64) { out += new TextDecoder().decode(u8.slice(b, b + c)); return c; }
  if (n === 66) { let t = 0; for (let i = 0; i < c; i++) { const p = dv.getUint32(b + 8 * i, true), l = dv.getUint32(b + 8 * i + 4, true); out += new TextDecoder().decode(u8.slice(p, p + l)); t += l; } return t; }
  if (n === 96) return 100;
  if (n === 94 || n === 93) throw new Exit();
  return 0;
};
const table = new WebAssembly.Table({ element: 'anyfunc', initial: 2000 });
const imports = { env: { memory, __indirect_function_table: table,
  __memory_base: new WebAssembly.Global({ value: 'i32' }, BASE),
  __table_base: new WebAssembly.Global({ value: 'i32' }, 1),
  __stack_pointer: SPG },
  linux: { syscall: sys },
  host: { fork() {
    const x = inst.exports;
    if (x.asyncify_get_state() === 2) { x.asyncify_stop_rewind(); return phase === 1 ? 0 : 42; }
    dv.setUint32(BUF, BUF + 8, true); dv.setUint32(BUF + 4, BUF + 65536, true);
    x.asyncify_start_unwind(BUF); return 0;
  } } };
inst = new WebAssembly.Instance(mod, imports);
const x = inst.exports;
if (x.__wasm_apply_data_relocs) x.__wasm_apply_data_relocs();
console.log(Object.keys(x).join(" "));
const args = STACK + 1024; dv.setUint32(args, 0, true);
const run = () => { SPG.value = STACK; try { x._start(args); } catch (e) { if (!(e instanceof Exit)) throw e; } };
console.log("run1"); run(); console.log("unwound", x.asyncify_get_state());
x.asyncify_stop_unwind();
saved = u8.slice(BUF, BUF + 65536); const memsnap = u8.slice(BASE, STACK + 65536);
console.log("child"); phase = 1; x.asyncify_start_rewind(BUF); run(); console.log("child done", JSON.stringify(out));
u8.set(memsnap, BASE); u8.set(saved, BUF); phase = 2; x.asyncify_start_rewind(BUF); run();   // parent
console.log(JSON.stringify(out));
