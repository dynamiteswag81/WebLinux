// Headless boot test: serves public/, boots WebLinux in Chromium and drives
// the console.  Needs the playwright package (NODE_PATH or a local install).
//
//   node test/boot-test.mjs [steps.json] [--log out.txt] [--shot out.png]
//
// steps.json is a list of [input, expected] pairs: send `input` + "\n"
// (null sends nothing), then wait until the console output contains
// `expected` (a string, searched after the previous match).  An input of
// {"kbd": "text"} types text + Enter on the canvas instead (keyboard
// device, frame buffer console); {"sleep": ms} just waits; {"reload": 1}
// reloads the page (a new boot, same browser storage); {"click": [x, y]}
// clicks on the canvas at frame buffer coordinates ("double": true for a
// double click; "wheel": dy scrolls there instead); {"button": "id"} clicks a page button (dialogs are
// accepted; a reload it causes restarts the output); {"press": "Control+c"}
// (or a list) presses keys on the canvas.  --shot saves a
// screenshot of the canvas at the end.  @PORT@ in an input becomes the
// page server's port (reachable from the guest as 10.0.2.2).  The default checks that
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
const si = args.indexOf('--shot');
const shotFile = si >= 0 ? args.splice(si, 2)[1] : null;
const steps = args[0] ? JSON.parse(fs.readFileSync(args[0], 'utf8')) : [
  [null, 'weblinux:/# '],
  ['echo $((6*7)) | { read x; echo got$x; }', 'got42'],
];
const TIMEOUT = Number(process.env.STEP_TIMEOUT || 60) * 1000;
const PORT = 8000 + Math.floor(Math.random() * 900);

const server = spawn(process.execPath, [path.join(ROOT, 'server.js'), String(PORT)],
  { stdio: 'ignore' });
let out = '';
let ok = false;
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
let page;
try {
  page = await browser.newPage({ viewport: { width: 1100, height: 850 } });
  page.on('pageerror', (e) => { out += `\n[pageerror] ${e.message}\n`; });
  await new Promise((r) => setTimeout(r, 300));
  page.on('dialog', (d) => d.accept());
  // collect the console output on every (re)load of the page
  await page.addInitScript(() => {
    self.__out = '';
    const hook = () => {
      if (!self.machine) return setTimeout(hook, 5);
      for (const t of ['console', 'log']) self.machine.addEventListener(t, (e) => { self.__out += e.detail; });
    };
    hook();
  });
  await page.goto(`http://127.0.0.1:${PORT}/`);
  let from = 0;
  for (const [input, expect] of steps) {
    if (input && input.kbd !== undefined) {
      await page.focus('#screen');
      await page.keyboard.type(input.kbd.replaceAll('@PORT@', String(PORT)), { delay: 5 });
      await page.keyboard.press('Enter');
    } else if (input && input.click) {
      // canvas pixel coordinates; the canvas is scaled to fit (object-fit)
      const [cx, cy] = input.click;
      const geo = await page.evaluate(() => {
        const c = document.getElementById('screen'), r = c.getBoundingClientRect();
        return { l: r.left, t: r.top, w: r.width, h: r.height, cw: c.width, ch: c.height };
      });
      const sc = Math.min(geo.w / geo.cw, geo.h / geo.ch);
      const x = geo.l + (geo.w - geo.cw * sc) / 2 + cx * sc, y = geo.t + (geo.h - geo.ch * sc) / 2 + cy * sc;
      await page.mouse.move(x, y, { steps: 4 });
      if (input.wheel) await page.mouse.wheel(0, input.wheel);
      else await page.mouse.click(x, y, { clickCount: input.double ? 2 : 1 });
    } else if (input && input.press) {
      // key combinations on the canvas, e.g. "Control+c"
      await page.focus('#screen');
      for (const k of [].concat(input.press)) await page.keyboard.press(k);
    } else if (input && input.paste !== undefined) {
      // what a Ctrl+Shift+V paste on the canvas does
      await page.evaluate((t) => self.wlscreen.typeText(t), input.paste.replaceAll('@PORT@', String(PORT)));
    } else if (input && input.drag) {
      // press at the first point, move to the second, release
      const geo = await page.evaluate(() => {
        const c = document.getElementById('screen'), r = c.getBoundingClientRect();
        return { l: r.left, t: r.top, w: r.width, h: r.height, cw: c.width, ch: c.height };
      });
      const sc = Math.min(geo.w / geo.cw, geo.h / geo.ch);
      const P = ([x, y]) => [geo.l + (geo.w - geo.cw * sc) / 2 + x * sc, geo.t + (geo.h - geo.ch * sc) / 2 + y * sc];
      const [a, b] = input.drag.map(P);
      await page.mouse.move(...a);
      await page.mouse.down();
      await page.mouse.move(...b, { steps: 8 });
      await page.mouse.up();
    } else if (input && input.copied !== undefined) {
      // wait until the guest copied this text to the host clipboard
      const t0 = Date.now();
      while ((await page.evaluate(() => self.lastCopied || '')) !== input.copied) {
        if (Date.now() - t0 > TIMEOUT) throw new Error('clipboard: ' + JSON.stringify(await page.evaluate(() => self.lastCopied)));
        await new Promise((r) => setTimeout(r, 100));
      }
    } else if (input && input.button) {
      // a button of the page; it may reload the page
      const nav = page.waitForNavigation({ timeout: TIMEOUT }).catch(() => null);
      await page.click('#' + input.button);
      if (input.reloads !== false) await nav;
      from = 0;
    } else if (input && input.reload) {
      await page.reload();
      from = 0;
    } else if (input && input.sleep) {
      await new Promise((r) => setTimeout(r, input.sleep));
    } else if (input !== null) {
      const text = input.replaceAll('@PORT@', String(PORT));
      await page.evaluate((s) => self.machine.consoleInput(s + '\n'), text);
    }
    const t0 = Date.now();
    for (;;) {
      out = await page.evaluate(() => self.__out || '').catch(() => out);
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
  if (shotFile && page) await page.locator('#screen').screenshot({ path: shotFile }).catch(() => {});
  if (logFile) fs.writeFileSync(logFile, out);
  await browser.close();
  server.kill();
}
process.exit(ok ? 0 : 1);
