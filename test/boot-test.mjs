// Headless boot test: serves public/, boots WebLinux in Chromium and drives
// the console.  Needs the playwright package (NODE_PATH or a local install).
//
//   node test/boot-test.mjs [steps.json] [--log out.txt]
//
// steps.json is a list of [input, expected] pairs: send `input` + "\n"
// (null sends nothing), then wait until the console output contains
// `expected` (a string, searched after the previous match).  The default checks that
// bash comes up and runs a pipeline.

import { spawn } from 'child_process';
import fs from 'fs';
import path from 'path';
import { createRequire } from 'module';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const ROOT = path.join(path.dirname(new URL(import.meta.url).pathname), '..');
const args = process.argv.slice(2);
const li = args.indexOf('--log');
const logFile = li >= 0 ? args.splice(li, 2)[1] : null;
const steps = args[0] ? JSON.parse(fs.readFileSync(args[0], 'utf8')) : [
  [null, 'running as init'],
  [null, '$ '],
  ['echo $((6*7)) | { read x; echo got$x; }', 'got42'],
];
const TIMEOUT = Number(process.env.STEP_TIMEOUT || 60) * 1000;
const PORT = 8000 + Math.floor(Math.random() * 900);

const server = spawn(process.execPath, [path.join(ROOT, 'server.js'), String(PORT)],
  { stdio: 'ignore' });
let out = '';
let ok = false;
const browser = await chromium.launch();
try {
  const page = await browser.newPage();
  page.on('pageerror', (e) => { out += `\n[pageerror] ${e.message}\n`; });
  await new Promise((r) => setTimeout(r, 300));
  await page.goto(`http://127.0.0.1:${PORT}/`);
  await page.evaluate(() => {
    self.__out = '';
    const hook = () => {
      if (!self.machine) return setTimeout(hook, 5);
      for (const t of ['console', 'log']) self.machine.addEventListener(t, (e) => { self.__out += e.detail; });
    };
    hook();
  });
  let from = 0;
  for (const [input, expect] of steps) {
    if (input !== null) await page.evaluate((s) => self.machine.consoleInput(s + '\n'), input);
    const t0 = Date.now();
    for (;;) {
      out = await page.evaluate(() => self.__out);
      const i = out.indexOf(expect, from);
      if (i >= 0) { from = i + expect.length; break; }
      if (Date.now() - t0 > TIMEOUT) throw new Error(`timed out waiting for ${JSON.stringify(expect)}`);
      await new Promise((r) => setTimeout(r, 100));
    }
  }
  ok = true;
  console.log('PASS');
} catch (e) {
  console.log(`FAIL: ${e.message}`);
  if (!logFile) console.log(out.slice(-1500));
} finally {
  if (logFile) fs.writeFileSync(logFile, out);
  await browser.close();
  server.kill();
}
process.exit(ok ? 0 : 1);
