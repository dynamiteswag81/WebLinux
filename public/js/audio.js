// Sound: plays the kernel's PCM stream (wasmsnd) through Web Audio.
//
// Browsers only start audio after a user gesture, so until then (or without
// Web Audio) the stream is consumed silently at its real-time rate, which
// keeps players from stalling.

const RUNNING = 0, BUF_BYTES = 2, PERIOD = 3, RATE = 4, CHANNELS = 5, HW_PTR = 6;
const IRQ_SOUND = 6;

export class Audio {
  constructor(machine) {
    this.m = machine;
    this.state = 0;
    this.node = null;
    machine.addEventListener('device', (e) => {
      if (e.detail.name !== 'snd') return;
      this.state = e.detail.addr;
      this._fallback();
    });
    const start = () => this.enable();
    for (const t of ['pointerdown', 'keydown'])
      addEventListener(t, start, { once: true, capture: true });
  }

  async enable() {
    if (this.ctx || !self.AudioContext || !this.m.memory) return;
    try {
      this.ctx = new AudioContext({ latencyHint: 'interactive' });
      await this.ctx.audioWorklet.addModule(new URL('./audio-worklet.js', import.meta.url));
      this.node = new AudioWorkletNode(this.ctx, 'weblinux-pcm',
        { numberOfInputs: 0, outputChannelCount: [2] });
      this.node.port.postMessage({ memory: this.m.memory, state: this.state, irq: this.m.dev.irq });
      this.node.connect(this.ctx.destination);
      if (this.ctx.state !== 'running') await this.ctx.resume();
      this.m.dispatchEvent(new CustomEvent('log',
        { detail: `audio: Web Audio output at ${this.ctx.sampleRate} Hz (${this.ctx.state})\n` }));
    } catch (err) {
      this.m.dispatchEvent(new CustomEvent('log', { detail: `audio: ${err.message}\n` }));
      this.node = null;
    }
  }

  get live() { return !!(this.node && this.ctx.state === 'running'); }

  // consume silently in real time while there is no audio output
  _fallback() {
    let last = performance.now(), carry = 0;
    const tick = () => {
      if (this.m.halted) return;
      const now = performance.now(), dt = (now - last) / 1000;
      last = now;
      const i32 = this.m.i32, s = this.state >> 2;
      if (!this.live && s && Atomics.load(i32, s + RUNNING)) {
        const fbytes = 2 * i32[s + CHANNELS], bytes = i32[s + BUF_BYTES] >>> 0;
        const period = i32[s + PERIOD] >>> 0;
        carry += dt * i32[s + RATE];
        const frames = Math.floor(carry);
        carry -= frames;
        const old = Atomics.load(i32, s + HW_PTR) >>> 0, moved = old + frames * fbytes;
        if (bytes && period && frames) {
          Atomics.store(i32, s + HW_PTR, moved % bytes);
          if (Math.floor(moved / period) !== Math.floor(old / period)) {
            Atomics.or(i32, this.m.dev.irq >> 2, 1 << IRQ_SOUND);
            Atomics.notify(i32, this.m.dev.irq >> 2);
          }
        }
      } else carry = 0;
      setTimeout(tick, 10);
    };
    tick();
  }
}
