// Screen: shows the kernel's frame buffer (wasmfb) on a canvas and feeds the
// canvas' keyboard and mouse events to the kernel's input devices.

import * as K from './keymap.js';

export class Screen {
  constructor(machine, canvas) {
    this.m = machine;
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.fb = null;
    this.held = new Set();
    machine.addEventListener('device', (e) => {
      if (e.detail.name === 'fb') this._attach(e.detail.addr);
    });
    this._wireInput();
  }

  _attach(addr) {
    const i32 = this.m.i32;
    const w = i32[(addr >> 2) + 1], h = i32[(addr >> 2) + 2];
    this.fb = { state: addr, w, h };
    this.canvas.width = w;
    this.canvas.height = h;
    this.image = this.ctx.createImageData(w, h);
    this.pixels = new Uint32Array(this.image.data.buffer);
    this.canvas.dispatchEvent(new CustomEvent('fbattach', { detail: { w, h } }));
    const frame = () => {
      if (this.m.halted) return;
      this._blit();
      requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);
  }

  // XRGB8888 (bytes B, G, R, X) -> canvas RGBA (bytes R, G, B, A)
  _blit() {
    const { state, w, h } = this.fb;
    const i32 = this.m.i32;
    const addr = i32[state >> 2] >>> 0, stride = i32[(state >> 2) + 3] >>> 0;
    if (!addr) return;
    if (i32[(state >> 2) + 5]) { this.pixels.fill(0xff000000); }
    else {
      const src = new Uint32Array(this.m.memory.buffer, addr, (stride >> 2) * h);
      const dst = this.pixels, line = stride >> 2;
      for (let y = 0, o = 0; y < h; y++) {
        let s = y * line;
        for (let x = 0; x < w; x++, o++, s++) {
          const p = src[s];
          dst[o] = 0xff000000 | (p & 0xff00) | ((p >>> 16) & 0xff) | ((p & 0xff) << 16);
        }
      }
    }
    this.ctx.putImageData(this.image, 0, 0);
  }

  _send(evs) { this.m.inputEvents(evs); }

  // A text field that on-screen keyboards type into; forward what arrives.
  attachTextInput(ta) {
    const press = (code) => this._send([[0, K.EV_KEY, code, 1], [0, K.EV_KEY, code, 0], [0, K.EV_SYN, 0, 0]]);
    ta.addEventListener('beforeinput', (e) => {
      e.preventDefault();
      switch (e.inputType) {
        case 'insertText': case 'insertReplacementText': case 'insertFromPaste':
          if (e.data) this.typeText(e.data);
          break;
        case 'insertLineBreak': case 'insertParagraph': press(K.KEYCODES.Enter); break;
        case 'deleteContentBackward': press(K.KEYCODES.Backspace); break;
        case 'deleteContentForward': press(K.KEYCODES.Delete); break;
      }
    });
    ta.addEventListener('keydown', (e) => {
      // keys that produce no input events
      const code = { ArrowUp: 1, ArrowDown: 1, ArrowLeft: 1, ArrowRight: 1, Escape: 1, Tab: 1 }[e.key]
        ? K.KEYCODES[e.key === 'Escape' ? 'Escape' : e.key === 'Tab' ? 'Tab' : e.key] : undefined;
      if (code) { e.preventDefault(); press(code); }
    });
  }

  // Type text as key presses, in chunks so the guest's queues keep up.
  typeText(text) {
    const keys = [];
    for (const ch of text) {
      const k = K.charKey(ch);
      if (!k) continue;
      const code = K.KEYCODES[k[0]];
      if (k[1]) keys.push([0, K.EV_KEY, 42, 1]);
      keys.push([0, K.EV_KEY, code, 1], [0, K.EV_KEY, code, 0]);
      if (k[1]) keys.push([0, K.EV_KEY, 42, 0]);
      keys.push([0, K.EV_SYN, 0, 0]);
    }
    const step = () => {
      if (!keys.length) return;
      this._send(keys.splice(0, 200));
      setTimeout(step, 30);
    };
    step();
  }

  _wireInput() {
    const c = this.canvas;
    c.addEventListener('keydown', (e) => {
      // Ctrl+Shift+V: let the browser paste (handled below)
      if (e.ctrlKey && e.shiftKey && e.code === 'KeyV') return;
      const code = K.KEYCODES[e.code];
      if (code === undefined) return;
      e.preventDefault();
      this.held.add(code);
      const ev = [[0, K.EV_KEY, code, e.repeat ? 2 : 1]];
      // a shifted key without a Shift key down (synthetic events, some
      // input methods): press Shift around it
      if ((e.shiftKey || K.isShifted(e.code, e.key)) && !this.held.has(42) && !this.held.has(54) && code !== 42 && code !== 54)
        ev.unshift([0, K.EV_KEY, 42, 1]), ev.push([0, K.EV_KEY, 42, 0]);
      this._send([...ev, [0, K.EV_SYN, 0, 0]]);
    });
    c.addEventListener('keyup', (e) => {
      const code = K.KEYCODES[e.code];
      if (code === undefined) return;
      e.preventDefault();
      this.held.delete(code);
      this._send([[0, K.EV_KEY, code, 0], [0, K.EV_SYN, 0, 0]]);
    });
    // release everything when focus goes away, so no key stays stuck
    c.addEventListener('blur', () => {
      if (!this.held.size) return;
      const evs = [...this.held].map((k) => [0, K.EV_KEY, k, 0]);
      this.held.clear();
      this._send([...evs, [0, K.EV_SYN, 0, 0]]);
    });

    const pos = (e) => {
      const r = c.getBoundingClientRect();
      // the canvas keeps its aspect ratio (object-fit: contain)
      const scale = Math.min(r.width / c.width, r.height / c.height);
      const ox = r.left + (r.width - c.width * scale) / 2;
      const oy = r.top + (r.height - c.height * scale) / 2;
      const x = Math.min(Math.max((e.clientX - ox) / scale / c.width, 0), 1);
      const y = Math.min(Math.max((e.clientY - oy) / scale / c.height, 0), 1);
      return [Math.round(x * K.ABS_MAX), Math.round(y * K.ABS_MAX)];
    };
    const BTN = [K.BTN_LEFT, K.BTN_MIDDLE, K.BTN_RIGHT];
    c.addEventListener('pointermove', (e) => {
      const [x, y] = pos(e);
      this._send([[1, K.EV_ABS, K.ABS_X, x], [1, K.EV_ABS, K.ABS_Y, y], [1, K.EV_SYN, 0, 0]]);
    });
    c.addEventListener('pointerdown', (e) => {
      c.focus();
      c.setPointerCapture(e.pointerId);
      const [x, y] = pos(e);
      const b = BTN[e.button];
      if (b === undefined) return;
      this._send([[1, K.EV_ABS, K.ABS_X, x], [1, K.EV_ABS, K.ABS_Y, y],
        [1, K.EV_KEY, b, 1], [1, K.EV_SYN, 0, 0]]);
    });
    c.addEventListener('pointerup', (e) => {
      const b = BTN[e.button];
      if (b === undefined) return;
      this._send([[1, K.EV_KEY, b, 0], [1, K.EV_SYN, 0, 0]]);
    });
    c.addEventListener('contextmenu', (e) => e.preventDefault());
    // paste: type the text (ASCII, US layout) into the guest
    addEventListener('paste', (e) => {
      if (document.activeElement !== c) return;
      e.preventDefault();
      this.typeText(e.clipboardData.getData('text/plain'));
    });
    c.addEventListener('wheel', (e) => {
      e.preventDefault();
      const evs = [];
      if (e.deltaY) evs.push([1, K.EV_REL, K.REL_WHEEL, e.deltaY > 0 ? -1 : 1]);
      if (e.deltaX) evs.push([1, K.EV_REL, K.REL_HWHEEL, e.deltaX > 0 ? 1 : -1]);
      if (evs.length) this._send([...evs, [1, K.EV_SYN, 0, 0]]);
    }, { passive: false });
  }
}
