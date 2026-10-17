// AudioWorklet that plays the kernel's PCM buffer (arch/wasm/drivers/wasmsnd.c)
// straight out of shared memory, resampling linearly to the context's rate.

'use strict';

// struct wasm_snd_state, in 32-bit words
const RUNNING = 0, BUF = 1, BUF_BYTES = 2, PERIOD = 3, RATE = 4, CHANNELS = 5, HW_PTR = 6, SEQ = 7;
const IRQ_SOUND = 6;

class PcmPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.s = null;
    this.port.onmessage = ({ data }) => {
      this.i32 = new Int32Array(data.memory.buffer);
      this.i16 = new Int16Array(data.memory.buffer);
      this.s = data.state >> 2;
      this.irq = data.irq >> 2;
      this.seq = -1;
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    const i32 = this.i32, s = this.s;
    if (s === null || !Atomics.load(i32, s + RUNNING)) {
      L.fill(0); if (R !== L) R.fill(0);
      return true;
    }
    const seq = i32[s + SEQ];
    if (seq !== this.seq) { this.seq = seq; this.frac = 0; this.last = [0, 0]; }
    const buf = i32[s + BUF] >>> 0, bytes = i32[s + BUF_BYTES] >>> 0;
    const period = i32[s + PERIOD] >>> 0, ch = i32[s + CHANNELS], rate = i32[s + RATE];
    const fbytes = 2 * ch, nframes = bytes / fbytes;
    let pos = (Atomics.load(i32, s + HW_PTR) >>> 0) / fbytes; // frame index
    const step = rate / sampleRate;
    let frac = this.frac, consumed = 0;
    for (let i = 0; i < L.length; i++) {
      const a = (buf >> 1) + ((pos % nframes) * ch);
      const b = (buf >> 1) + (((pos + 1) % nframes) * ch);
      const l0 = this.i16[a], l1 = this.i16[b];
      const r0 = ch > 1 ? this.i16[a + 1] : l0, r1 = ch > 1 ? this.i16[b + 1] : l1;
      L[i] = (l0 + (l1 - l0) * frac) / 32768;
      if (R !== L) R[i] = (r0 + (r1 - r0) * frac) / 32768;
      frac += step;
      while (frac >= 1) { frac -= 1; pos = (pos + 1) % nframes; consumed++; }
    }
    this.frac = frac;
    if (consumed) advance(i32, s, this.irq, consumed * fbytes, bytes, period);
    return true;
  }
}

function advance(i32, s, irq, n, bytes, period) {
  const old = Atomics.load(i32, s + HW_PTR) >>> 0;
  const moved = old + n;
  Atomics.store(i32, s + HW_PTR, moved % bytes);
  if (Math.floor(moved / period) !== Math.floor(old / period)) {
    Atomics.or(i32, irq, 1 << IRQ_SOUND);
    Atomics.notify(i32, irq);
  }
}

registerProcessor('weblinux-pcm', PcmPlayer);
