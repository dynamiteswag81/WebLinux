// WebLinux page: boots the machine and wires up the console.

import { Machine } from './machine.js';

const $ = (id) => document.getElementById(id);
const log = $('log');
const status = $('status');

const params = new URLSearchParams(location.search);
const machine = new Machine({
  memoryMiB: Number(params.get('mem')) || 512,
  cmdline: params.get('cmdline') || 'console=ttyW0 rdinit=/init loglevel=7',
});
self.machine = machine; // handy from devtools

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

machine.addEventListener('console', (e) => appendLog(e.detail));
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

status.textContent = 'booting…';
machine.boot().then(
  () => { status.textContent = 'running'; },
  (err) => { status.textContent = 'failed'; appendLog(`boot failed: ${err.message}\n`); },
);
