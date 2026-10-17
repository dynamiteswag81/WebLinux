// Network worker: the other end of the kernel's eth0 (arch/wasm/drivers/wasmnet.c).
//
// A tiny router with NAT, in the spirit of QEMU's slirp:
//   guest 10.0.2.15/24, gateway 10.0.2.2 (also "the host": the page server),
//   DNS 10.0.2.3.
// It answers ARP and pings to its own addresses, terminates the guest's TCP
// connections and UDP flows, and carries their payload over WebSockets to
// the page server's relay (host/netrelay.js), which opens the real sockets.

'use strict';

const SLOTS = 256, SLOT = 2048;
const MAC_OFF = 0, LINK = 8, TX = 16, RX = TX + 8 + SLOTS * SLOT;
const IRQ_NET = 4;

const GUEST_IP = ip('10.0.2.15'), GW_IP = ip('10.0.2.2'), DNS_IP = ip('10.0.2.3');
const NET_MASK = ip('255.255.255.0');
const ROUTER_MAC = [0x52, 0x54, 0x00, 0x12, 0x35, 0x02];
const GUEST_MAC = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
let guestMac = GUEST_MAC; // learned from the guest's frames
const MSS = 1460;

function ip(s) { return s.split('.').reduce((a, b) => ((a << 8) | Number(b)) >>> 0, 0); }
function ipStr(n) { return [n >>> 24, (n >>> 16) & 255, (n >>> 8) & 255, n & 255].join('.'); }

let i32, u8, dv, state, irq, relayUrl, log;

// ---------------------------------------------------------------- frames out
function sendFrame(frame) {
  const head = Atomics.load(i32, (state + RX) >> 2) >>> 0;
  const tail = Atomics.load(i32, (state + RX + 4) >> 2) >>> 0;
  if (((head - tail) >>> 0) >= SLOTS) return false; // ring full: drop
  const slot = state + RX + 8 + (head % SLOTS) * SLOT;
  dv.setUint32(slot, frame.length, true);
  u8.set(frame, slot + 4);
  Atomics.store(i32, (state + RX) >> 2, (head + 1) | 0);
  Atomics.or(i32, irq >> 2, 1 << IRQ_NET);
  Atomics.notify(i32, irq >> 2);
  return true;
}

function csum(buf, off, len, init = 0) {
  let s = init;
  for (let i = 0; i + 1 < len; i += 2) s += (buf[off + i] << 8) | buf[off + i + 1];
  if (len & 1) s += buf[off + len - 1] << 8;
  while (s > 0xffff) s = (s & 0xffff) + (s >>> 16);
  return s;
}

// Ethernet + IPv4 header around payload; proto 1/6/17
function ipFrame(src, dst, proto, payload, l4csum) {
  const f = new Uint8Array(14 + 20 + payload.length);
  f.set(guestMac, 0); f.set(ROUTER_MAC, 6); f[12] = 0x08; f[13] = 0x00;
  const v = new DataView(f.buffer);
  f[14] = 0x45; v.setUint16(16, 20 + payload.length); v.setUint16(18, (ipId = (ipId + 1) & 0xffff));
  v.setUint16(20, 0x4000); f[22] = 64; f[23] = proto;
  v.setUint32(26, src); v.setUint32(30, dst);
  v.setUint16(24, ~csum(f, 14, 20) & 0xffff);
  f.set(payload, 34);
  if (l4csum) {
    // pseudo header: src, dst, proto, length
    let s = csum(f, 26, 8) + proto + payload.length;
    s = csum(f, 34, payload.length, s);
    v.setUint16(34 + l4csum, (~s & 0xffff) || 0xffff);
  }
  return f;
}
let ipId = 0;

// ---------------------------------------------------------------- frames in
function onFrame(f) {
  if (f.length < 14) return;
  if (!(f[6] & 1)) guestMac = f.slice(6, 12);
  const v = new DataView(f.buffer, f.byteOffset, f.length);
  const type = v.getUint16(12);
  if (type === 0x0806) return arp(f, v);
  if (type !== 0x0800 || f.length < 34) return;
  const ihl = (f[14] & 15) * 4, total = v.getUint16(16);
  if ((v.getUint16(20) & 0x3fff) !== 0) return; // fragments: not supported
  const proto = f[23], src = v.getUint32(26), dst = v.getUint32(30);
  const p = f.subarray(14 + ihl, 14 + total);
  if (proto === 1) icmp(src, dst, p);
  else if (proto === 6) tcpIn(src, dst, p);
  else if (proto === 17) udpIn(src, dst, p);
}

function arp(f, v) {
  if (v.getUint16(20) !== 1) return; // requests only
  const target = v.getUint32(38);
  if ((target & NET_MASK) !== (GUEST_IP & NET_MASK) || target === GUEST_IP) return;
  const r = new Uint8Array(42);
  r.set(f.subarray(6, 12), 0); r.set(ROUTER_MAC, 6); r[12] = 8; r[13] = 6;
  r.set([0, 1, 8, 0, 6, 4, 0, 2], 14);
  r.set(ROUTER_MAC, 22); r.set(f.subarray(38, 42), 28);
  r.set(f.subarray(22, 32), 32);
  sendFrame(r);
}

function icmp(src, dst, p) {
  if (p[0] !== 8 || (dst !== GW_IP && dst !== DNS_IP)) return; // echo to us only
  const r = new Uint8Array(p);
  r[0] = 0; r[2] = r[3] = 0;
  const s = ~csum(r, 0, r.length) & 0xffff;
  r[2] = s >> 8; r[3] = s & 255;
  sendFrame(ipFrame(dst, src, 1, r));
}

// the real address the relay should use for a guest destination
function target(dst) {
  if (dst === GW_IP) return '127.0.0.1';
  return ipStr(dst);
}

function openSocket(params) {
  const ws = new WebSocket(`${relayUrl}?${new URLSearchParams(params)}`);
  ws.binaryType = 'arraybuffer';
  return ws;
}

// ---------------------------------------------------------------- UDP
const udpFlows = new Map(); // "sport|dst|dport" -> { ws, queue, idle }

function udpIn(src, dst, p) {
  if (p.length < 8) return;
  const v = new DataView(p.buffer, p.byteOffset, p.length);
  const sport = v.getUint16(0), dport = v.getUint16(2);
  const data = p.slice(8, v.getUint16(4));
  const key = `${sport}|${dst}|${dport}`;
  let fl = udpFlows.get(key);
  if (!fl) {
    const params = dst === DNS_IP && dport === 53
      ? { proto: 'udp', host: 'dns', port: 53 }
      : { proto: 'udp', host: target(dst), port: dport };
    fl = { ws: openSocket(params), queue: [], last: Date.now() };
    fl.ws.onopen = () => { for (const d of fl.queue) fl.ws.send(d); fl.queue = null; };
    fl.ws.onmessage = (e) => {
      fl.last = Date.now();
      const d = new Uint8Array(e.data);
      const u = new Uint8Array(8 + d.length);
      const uv = new DataView(u.buffer);
      uv.setUint16(0, dport); uv.setUint16(2, sport); uv.setUint16(4, u.length);
      u.set(d, 8);
      sendFrame(ipFrame(dst, src, 17, u, 6));
    };
    fl.ws.onclose = fl.ws.onerror = () => udpFlows.delete(key);
    udpFlows.set(key, fl);
  }
  fl.last = Date.now();
  if (fl.queue) fl.queue.push(data); else fl.ws.send(data);
}

setInterval(() => {
  const now = Date.now();
  for (const [k, fl] of udpFlows) if (now - fl.last > 60000) { fl.ws.close(); udpFlows.delete(k); }
}, 10000);

// ---------------------------------------------------------------- TCP
// We are the server side of each guest connection; the relay's socket is
// the far end.  No window scaling, no SACK: the guest offers at most 64K.
const FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16;
const conns = new Map(); // "sport|dst|dport" -> conn

function tcpSend(c, flags, data = new Uint8Array(0), seq = c.sndNxt) {
  const opt = flags & SYN ? 4 : 0;
  const t = new Uint8Array(20 + opt + data.length);
  const v = new DataView(t.buffer);
  v.setUint16(0, c.dport); v.setUint16(2, c.sport);
  v.setUint32(4, seq); v.setUint32(8, flags & ACK ? c.rcvNxt : 0);
  t[12] = ((20 + opt) / 4) << 4; t[13] = flags;
  // our receive window: what the relay can absorb
  v.setUint16(14, c.ws && c.ws.bufferedAmount > 1 << 20 ? 0 : 65535);
  if (opt) { t[20] = 2; t[21] = 4; v.setUint16(22, MSS); }
  t.set(data, 20 + opt);
  sendFrame(ipFrame(c.dst, c.src, 6, t, 16));
}

function tcpReset(src, dst, p, v) {
  const flags = p[13];
  if (flags & RST) return;
  const c = { src, dst, sport: v.getUint16(0), dport: v.getUint16(2), rcvNxt: 0, sndNxt: 0 };
  const seq = v.getUint32(4), len = p.length - (p[12] >> 4) * 4;
  if (flags & ACK) tcpSend(c, RST, undefined, v.getUint32(8));
  else { c.rcvNxt = (seq + len + (flags & SYN ? 1 : 0) + (flags & FIN ? 1 : 0)) >>> 0; tcpSend(c, RST | ACK, undefined, 0); }
}

function tcpIn(src, dst, p) {
  if (p.length < 20) return;
  const v = new DataView(p.buffer, p.byteOffset, p.length);
  const sport = v.getUint16(0), dport = v.getUint16(2);
  const seq = v.getUint32(4), ack = v.getUint32(8);
  const off = (p[12] >> 4) * 4, flags = p[13];
  const data = p.subarray(off);
  const key = `${sport}|${dst}|${dport}`;
  let c = conns.get(key);

  if (!c) {
    if ((flags & (SYN | ACK)) !== SYN) return tcpReset(src, dst, p, v);
    c = {
      key, src, dst, sport, dport, state: 'syn', rcvNxt: (seq + 1) >>> 0,
      iss: (Math.random() * 0x7fffffff) >>> 0, sndUna: 0, sndNxt: 0,
      sndWnd: v.getUint16(14), out: [], outLen: 0, finQueued: false, finSent: false,
      lastProgress: Date.now(),
    };
    c.sndUna = c.sndNxt = c.iss;
    conns.set(key, c);
    c.ws = openSocket({ proto: 'tcp', host: target(dst), port: dport });
    c.ws.onopen = () => {
      c.state = 'est';
      tcpSend(c, SYN | ACK, undefined, c.iss);
      c.sndNxt = (c.iss + 1) >>> 0;
    };
    c.ws.onmessage = (e) => { queueOut(c, new Uint8Array(e.data)); };
    c.ws.onclose = () => {
      if (c.state === 'syn') { tcpSend(c, RST | ACK, undefined, 0); conns.delete(key); return; }
      c.finQueued = true;
      pump(c);
    };
    c.ws.onerror = () => {};
    return;
  }

  if (flags & RST) { closeConn(c); return; }
  if (flags & SYN) { // retransmitted SYN while connecting
    if (c.state !== 'syn') tcpSend(c, SYN | ACK, undefined, c.iss);
    return;
  }
  if (flags & ACK) {
    const acked = (ack - c.sndUna) | 0;
    if (acked > 0 && ((c.sndNxt - ack) | 0) >= 0) {
      let n = acked;
      if (c.sndUna === c.iss) n--; // our SYN
      if (c.finSent && ack === c.sndNxt) n--; // our FIN
      dropAcked(c, n);
      c.sndUna = ack;
      c.lastProgress = Date.now();
      if (c.finSent && ack === c.sndNxt && c.guestFin) return closeConn(c);
    }
    c.sndWnd = v.getUint16(14);
  }
  if (data.length || (flags & FIN)) {
    if (seq !== c.rcvNxt) { tcpSend(c, ACK); return; } // out of order: re-ack
    if (data.length) {
      if (c.ws.readyState === WebSocket.OPEN) c.ws.send(data.slice());
      c.rcvNxt = (c.rcvNxt + data.length) >>> 0;
    }
    if (flags & FIN) {
      c.rcvNxt = (c.rcvNxt + 1) >>> 0;
      c.guestFin = true;
      // half close: the relay ends its write side on an empty message
      if (c.ws.readyState === WebSocket.OPEN) c.ws.send(new Uint8Array(0));
    }
    tcpSend(c, ACK);
    if (c.guestFin && c.finSent && c.sndUna === c.sndNxt) closeConn(c);
  }
  pump(c);
}

function queueOut(c, d) {
  c.out.push(d);
  c.outLen += d.length;
  pump(c);
}

// unacked + unsent bytes live in c.out from sndUna on
function dropAcked(c, n) {
  c.outLen -= n;
  while (n > 0 && c.out.length) {
    if (c.out[0].length <= n) { n -= c.out.shift().length; }
    else { c.out[0] = c.out[0].subarray(n); n = 0; }
  }
}

function pump(c) {
  if (c.state !== 'est') return;
  const inFlight = (c.sndNxt - c.sndUna) >>> 0;
  let sent = inFlight - (c.finSent ? 1 : 0);
  const wnd = Math.min(c.sndWnd, 65535);
  while (sent < c.outLen && sent < wnd) {
    const n = Math.min(MSS, c.outLen - sent, wnd - sent);
    const seg = gather(c, sent, n);
    tcpSend(c, ACK | PSH, seg, (c.sndUna + sent) >>> 0);
    sent += n;
    c.sndNxt = (c.sndUna + sent) >>> 0;
  }
  if (c.finQueued && !c.finSent && sent === c.outLen) {
    tcpSend(c, FIN | ACK);
    c.sndNxt = (c.sndNxt + 1) >>> 0;
    c.finSent = true;
  }
}

function gather(c, from, n) {
  const r = new Uint8Array(n);
  let o = 0;
  for (const b of c.out) {
    if (from >= b.length) { from -= b.length; continue; }
    const k = Math.min(b.length - from, n - o);
    r.set(b.subarray(from, from + k), o);
    o += k; from = 0;
    if (o === n) break;
  }
  return r;
}

function closeConn(c) {
  conns.delete(c.key);
  if (c.ws.readyState <= WebSocket.OPEN) c.ws.close();
}

// retransmit: go back to the first unacked byte when nothing moved
setInterval(() => {
  const now = Date.now();
  for (const c of conns.values()) {
    if (c.state !== 'est') continue;
    if (c.sndNxt !== c.sndUna && now - c.lastProgress > 400) {
      c.sndNxt = c.sndUna;
      if (c.finSent) c.finSent = false;
      c.lastProgress = now;
      pump(c);
    } else if (c.sndWnd === 0 && c.outLen) {
      pump(c); // window probe-ish: resend when the guest reopens
    }
  }
}, 200);

// ---------------------------------------------------------------- main loop
async function txLoop() {
  const H = (state + TX) >> 2, T = H + 1;
  for (;;) {
    let tail = Atomics.load(i32, T) >>> 0;
    const head = Atomics.load(i32, H) >>> 0;
    if (head === tail) {
      const w = Atomics.waitAsync ? Atomics.waitAsync(i32, H, head | 0, 1000) : null;
      if (w && w.async) await w.value;
      else if (!w) await new Promise((r) => setTimeout(r, 4));
      continue;
    }
    while (tail !== head) {
      const slot = state + TX + 8 + (tail % SLOTS) * SLOT;
      const len = dv.getUint32(slot, true);
      if (len <= SLOT - 4) {
        try { onFrame(u8.slice(slot + 4, slot + 4 + len)); } catch (e) { log(`net: ${e.stack}\n`); }
      }
      tail = (tail + 1) >>> 0;
    }
    Atomics.store(i32, T, tail | 0);
  }
}

self.onmessage = ({ data }) => {
  ({ state, irq, relayUrl } = data);
  i32 = new Int32Array(data.memory.buffer);
  u8 = new Uint8Array(data.memory.buffer);
  dv = new DataView(data.memory.buffer);
  log = (s) => self.postMessage({ t: 'log', s });
  u8.set(GUEST_MAC, state + MAC_OFF);
  Atomics.store(i32, (state + LINK) >> 2, 1);
  txLoop();
};
