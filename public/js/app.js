// WebLinux page: boots the machine and wires up the console.

import { Machine } from './machine.js';
import { Screen } from './screen.js';
import { Audio } from './audio.js';

const $ = (id) => document.getElementById(id);
const log = $('log');
const status = $('status');

const params = new URLSearchParams(location.search);

// The browser's current UTC offset as a POSIX TZ value (no DST rules: the
// guest has no zoneinfo).  The kernel hands TZ=... to init's environment.
function posixTZ() {
  const off = new Date().getTimezoneOffset(); // minutes west of UTC
  const a = Math.abs(off);
  return `LOC${off < 0 ? '-' : ''}${Math.floor(a / 60)}${a % 60 ? ':' + String(a % 60).padStart(2, '0') : ''}`;
}
const machine = new Machine({
  memoryMiB: Number(params.get('mem')) || 512,
  disk: params.get('disk') || 'opfs',          // opfs | mem | none
  diskSizeMiB: Number(params.get('disksize')) || 256,
  cmdline: (params.get('cmdline') || 'console=ttyW0 rdinit=/init loglevel=7')
    + ` TZ=${posixTZ()}`
    + (params.get('mode') ? ` wasmfb.mode=${params.get('mode')}` : ''),
});
self.machine = machine; // handy from devtools
const screen = new Screen(machine, $('screen'));
self.wlscreen = screen; // handy from devtools and tests
const audio = new Audio(machine);
self.audio = audio;
// once the frame buffer is up, the canvas is the main console
$('screen').addEventListener('fbattach', () => {
  $('bootlog').classList.add('hidden');
  $('screen').focus();
});

const ANSI = /\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][0-9A-Za-z]|\r/g;
let pending = '';
let flushQueued = false;
function appendLog(s) {
  pending += s;
  if (flushQueued) return;
  flushQueued = true;
  setTimeout(() => {
    flushQueued = false;
    const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
    log.textContent += pending.replace(ANSI, '');
    pending = '';
    if (log.textContent.length > 400000) log.textContent = log.textContent.slice(-300000);
    if (atBottom) log.scrollTop = log.scrollHeight;
  }, 30);
}

// OSC 52 on the debug console (ESC ] 52 ; c ; base64 BEL) sets the host's
// clipboard: the desktop's terminal copies its selection this way.
let oscHold = '';
function consoleText(s) {
  s = oscHold + s;
  oscHold = '';
  for (;;) {
    const i = s.indexOf('\x1b]52;');
    if (i < 0) break;
    const m = /^\x1b\]52;[^;]*;([A-Za-z0-9+/=]*)(\x07|\x1b\\)/.exec(s.slice(i));
    if (!m) {
      if (s.length - i < 1 << 20) { oscHold = s.slice(i); s = s.slice(0, i); }
      break;
    }
    try {
      const bytes = Uint8Array.from(atob(m[1]), (c) => c.charCodeAt(0));
      const text = new TextDecoder().decode(bytes);
      self.lastCopied = text;
      navigator.clipboard?.writeText(text).catch(() => {});
    } catch (err) { /* bad base64: drop it */ }
    s = s.slice(0, i) + s.slice(i + m[0].length);
  }
  appendLog(s);
}
machine.addEventListener('console', (e) => consoleText(e.detail));
machine.addEventListener('log', (e) => appendLog(e.detail));
machine.addEventListener('halt', (e) => {
  status.textContent = ['powered off', 'rebooting', 'kernel panic'][e.detail] || 'halted';
  if (e.detail === 1) setTimeout(() => location.reload(), 500);
});

$('cons').addEventListener('submit', (e) => {
  e.preventDefault();
  const v = $('consin').value;
  $('consin').value = '';
  machine.consoleInput(v + '\n');
});
$('btn-log').addEventListener('click', () => $('bootlog').classList.toggle('hidden'));
$('btn-full').addEventListener('click', () => {
  document.documentElement.requestFullscreen?.().then(() => $('screen').focus(), () => {});
});
$('btn-reset').addEventListener('click', async () => {
  if (!confirm('Erase the saved disk (all your files) and restart?')) return;
  machine.halt(0);
  status.textContent = 'erasing…';
  try {
    const root = await navigator.storage.getDirectory();
    // the disk worker just stopped: its file handle closes asynchronously
    for (let i = 0; ; i++) {
      try { await root.removeEntry(machine.disk.name); break; } catch (e) {
        if (e.name === 'NotFoundError') break;
        if (i > 50) throw e;
        await new Promise((r) => setTimeout(r, 100));
      }
    }
  } catch (e) {
    appendLog(`reset: ${e.message}\n`);
  }
  location.reload();
});

// the tab may be closed soon after it is hidden: write the disk out
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden') machine.requestSync();
});
addEventListener('pagehide', () => machine.requestSync());

status.textContent = 'booting…';
machine.boot().then(
  () => { status.textContent = 'running'; },
  (err) => { status.textContent = 'failed'; appendLog(`boot failed: ${err.message}\n`); },
);
