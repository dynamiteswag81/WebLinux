import fs from 'fs';
const mod = new WebAssembly.Module(fs.readFileSync(process.argv[2]));
const memory = new WebAssembly.Memory({ initial: 512, maximum: 512, shared: true });
const u8 = new Uint8Array(memory.buffer);
let out = '';
const imports = { env: { memory, __indirect_function_table: new WebAssembly.Table({ element: 'anyfunc', initial: 100 }),
  __memory_base: new WebAssembly.Global({ value: 'i32' }, 1 << 20), __table_base: new WebAssembly.Global({ value: 'i32' }, 1),
  __stack_pointer: new WebAssembly.Global({ value: 'i32', mutable: true }, 4 << 20) },
  linux: { syscall: (n, a, b, c) => { if (n === 64) out += new TextDecoder().decode(u8.slice(b, b + c)); if (n === 94) throw 'exit'; return 0; } },
  host: { fork: () => 7 } };
const inst = new WebAssembly.Instance(mod, imports);
try { inst.exports._start(0); } catch (e) { if (e !== 'exit') throw e; }
console.log(JSON.stringify(out));
