#!/usr/bin/env node
// Build a newc cpio archive (an initramfs) from a staging directory plus a
// manifest of things Windows file systems can't hold (device nodes,
// symlinks, ownership, modes).
//
//   node build/mkinitramfs.js <rootdir> <manifest> <out.cpio>
//
// Manifest lines (gen_init_cpio syntax subset):
//   dir   <path> <mode> <uid> <gid>
//   nod   <path> <mode> <uid> <gid> <c|b> <maj> <min>
//   slink <path> <target> <mode> <uid> <gid>
//   mode  <path> <mode>            (override a staged file's mode)
// Staged files default to 0644, or 0755 for executables (wasm modules,
// scripts starting with #!, anything under a bin/ or sbin/ directory).

'use strict';

const fs = require('fs');
const path = require('path');

const [, , rootDir, manifestFile, outFile] = process.argv;
if (!rootDir || !outFile) {
  console.error('usage: mkinitramfs.js <rootdir> <manifest> <out.cpio>');
  process.exit(1);
}

const entries = new Map(); // path -> entry
let ino = 300000;

function add(e) {
  e.path = e.path.replace(/^\/+/, '');
  entries.set(e.path, e);
}

function isExecutable(rel, buf) {
  if (/(^|\/)s?bin\//.test(rel)) return true;
  if (buf.length >= 4 && buf[0] === 0 && buf[1] === 0x61 && buf[2] === 0x73 && buf[3] === 0x6d) return true;
  if (buf.length >= 2 && buf[0] === 0x23 && buf[1] === 0x21) return true;
  return false;
}

function walk(dir, rel) {
  for (const name of fs.readdirSync(dir).sort()) {
    const full = path.join(dir, name);
    const r = rel ? rel + '/' + name : name;
    const st = fs.statSync(full);
    if (st.isDirectory()) {
      add({ path: r, type: 'dir', mode: 0o755, uid: 0, gid: 0 });
      walk(full, r);
    } else if (st.isFile()) {
      const data = fs.readFileSync(full);
      add({ path: r, type: 'file', mode: isExecutable(r, data) ? 0o755 : 0o644, uid: 0, gid: 0, data });
    }
  }
}

if (fs.existsSync(rootDir)) walk(rootDir, '');

if (manifestFile && fs.existsSync(manifestFile)) {
  const lines = fs.readFileSync(manifestFile, 'utf8').split(/\r?\n/);
  for (const raw of lines) {
    const line = raw.replace(/#.*/, '').trim();
    if (!line) continue;
    const f = line.split(/\s+/);
    switch (f[0]) {
      case 'dir':
        add({ path: f[1], type: 'dir', mode: parseInt(f[2], 8), uid: +f[3], gid: +f[4] });
        break;
      case 'nod':
        add({ path: f[1], type: 'nod', mode: parseInt(f[2], 8), uid: +f[3], gid: +f[4],
              kind: f[5], maj: +f[6], min: +f[7] });
        break;
      case 'slink':
        add({ path: f[1], type: 'slink', target: f[2], mode: parseInt(f[3], 8), uid: +f[4], gid: +f[5] });
        break;
      case 'mode': {
        const e = entries.get(f[1].replace(/^\/+/, ''));
        if (!e) throw new Error('mode: no such entry ' + f[1]);
        e.mode = parseInt(f[2], 8);
        break;
      }
      default:
        throw new Error('bad manifest line: ' + line);
    }
  }
}

// Parents before children: create missing parent dirs and sort by path.
for (const p of [...entries.keys()]) {
  const parts = p.split('/');
  for (let i = 1; i < parts.length; i++) {
    const d = parts.slice(0, i).join('/');
    if (!entries.has(d)) add({ path: d, type: 'dir', mode: 0o755, uid: 0, gid: 0 });
  }
}
const sorted = [...entries.values()].sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));

const chunks = [];
let offset = 0;
const hex = (n) => (n >>> 0).toString(16).padStart(8, '0');
const mtime = Math.floor(Date.now() / 1000);

function push(buf) {
  chunks.push(buf);
  offset += buf.length;
}
function pad4() {
  const n = (4 - (offset % 4)) % 4;
  if (n) push(Buffer.alloc(n));
}

function record(name, mode, uid, gid, nlink, data, rdevmaj, rdevmin) {
  const nameBuf = Buffer.from(name + '\0', 'utf8');
  const hdr = '070701' + hex(ino++) + hex(mode) + hex(uid) + hex(gid) + hex(nlink) +
    hex(mtime) + hex(data.length) + hex(0) + hex(0) + hex(rdevmaj) + hex(rdevmin) +
    hex(nameBuf.length) + hex(0);
  push(Buffer.from(hdr, 'ascii'));
  push(nameBuf);
  pad4();
  push(data);
  pad4();
}

const S_IFDIR = 0o040000, S_IFREG = 0o100000, S_IFLNK = 0o120000, S_IFCHR = 0o020000, S_IFBLK = 0o060000;
for (const e of sorted) {
  switch (e.type) {
    case 'dir': record(e.path, S_IFDIR | e.mode, e.uid, e.gid, 2, Buffer.alloc(0), 0, 0); break;
    case 'file': record(e.path, S_IFREG | e.mode, e.uid, e.gid, 1, e.data, 0, 0); break;
    case 'slink': record(e.path, S_IFLNK | e.mode, e.uid, e.gid, 1, Buffer.from(e.target), 0, 0); break;
    case 'nod':
      record(e.path, (e.kind === 'b' ? S_IFBLK : S_IFCHR) | e.mode, e.uid, e.gid, 1, Buffer.alloc(0), e.maj, e.min);
      break;
  }
}
record('TRAILER!!!', 0, 0, 0, 1, Buffer.alloc(0), 0, 0);

fs.mkdirSync(path.dirname(outFile), { recursive: true });
fs.writeFileSync(outFile, Buffer.concat(chunks));
console.log(`${outFile}: ${sorted.length} entries, ${(offset / 1024).toFixed(0)} KiB`);
