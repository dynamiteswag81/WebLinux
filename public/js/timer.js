// WebLinux timer worker: the machine's one-shot timer.
//
// The kernel writes an absolute deadline (host monotonic ns) and bumps a
// sequence number.  We sleep until the deadline or until the sequence
// changes, then raise IRQ 0 and wake the CPU if it is idle.

'use strict';

self.onmessage = (ev) => {
  const { memory, clock, base, irq, deadline, seq } = ev.data;
  const i32 = new Int32Array(memory.buffer);
  const i64 = new BigInt64Array(memory.buffer);
  const clk = new BigInt64Array(clock);
  const seqIdx = seq >> 2, dlIdx = deadline >> 3, irqIdx = irq >> 2;

  const now = () => {
    const t = BigInt(Math.floor((performance.timeOrigin + performance.now() - base) * 1e6));
    const last = Atomics.load(clk, 0);
    return t > last ? t : last;
  };

  for (;;) {
    const s = Atomics.load(i32, seqIdx);
    const dl = Atomics.load(i64, dlIdx);
    let timeout = Infinity;
    if (dl !== 0n) {
      const t = now();
      if (t >= dl) {
        // fire once per programmed deadline
        if (Atomics.compareExchange(i64, dlIdx, dl, 0n) === dl) {
          Atomics.or(i32, irqIdx, 1);
          Atomics.notify(i32, irqIdx);
        }
        continue;
      }
      timeout = Number(dl - t) / 1e6;
    }
    Atomics.wait(i32, seqIdx, s, timeout);
  }
};
