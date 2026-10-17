// Network relay for WebLinux: the page's network worker (public/js/net.js)
// opens one WebSocket per guest connection, and this turns it into a real
// socket on the server's machine.
//
//   ws://<server>/net?proto=tcp&host=1.2.3.4&port=80   stream, both ways;
//       an empty message from the page ends our write side (half close)
//   ws://<server>/net?proto=udp&host=1.2.3.4&port=53   one message per datagram
//       (host=dns: the server's first resolver from /etc/resolv.conf)
//
// Zero dependencies: a minimal RFC 6455 server (binary frames, ping/pong,
// close).  The relay reaches whatever the server can, so the server only
// listens on localhost unless HOST says otherwise.

'use strict';

const crypto = require('crypto');
const dgram = require('dgram');
const fs = require('fs');
const net = require('net');

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function dnsServer() {
  try {
    const m = fs.readFileSync('/etc/resolv.conf', 'utf8').match(/^\s*nameserver\s+([0-9.]+)/m);
    if (m) return m[1];
  } catch { /* not unix */ }
  return '8.8.8.8';
}

// Wrap an upgraded socket: onMessage(Buffer), onClose(); returns {send, close}
function websocket(sock, onMessage, onClose) {
  let buf = Buffer.alloc(0), frag = [], closed = false;
  const frame = (op, data) => {
    const n = data.length;
    const h = n < 126 ? Buffer.from([0x80 | op, n])
      : n < 65536 ? Buffer.from([0x80 | op, 126, n >> 8, n & 255])
        : Buffer.concat([Buffer.from([0x80 | op, 127, 0, 0, 0, 0]), (() => { const b = Buffer.alloc(4); b.writeUInt32BE(n); return b; })()]);
    return Buffer.concat([h, data]);
  };
  const ws = {
    send(data) { if (!closed && sock.writable) sock.write(frame(2, data)); },
    close() {
      if (closed) return;
      closed = true;
      if (sock.writable) sock.end(frame(8, Buffer.from([0x03, 0xe8])));
      onClose();
    },
    get bufferedAmount() { return sock.writableLength; },
    pause() { sock.pause(); },
    resume() { sock.resume(); },
  };
  sock.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      if (buf.length < 2) return;
      const fin = buf[0] & 0x80, op = buf[0] & 15, masked = buf[1] & 0x80;
      let len = buf[1] & 127, p = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); p = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); p = 10; }
      const mp = p;
      if (masked) p += 4;
      if (buf.length < p + len) return;
      const data = Buffer.from(buf.subarray(p, p + len));
      if (masked) for (let i = 0; i < len; i++) data[i] ^= buf[mp + (i & 3)];
      buf = buf.subarray(p + len);
      if (op === 8) { ws.close(); return; }
      if (op === 9) { if (sock.writable) sock.write(frame(10, data)); continue; }
      if (op === 10) continue;
      frag.push(data);
      if (fin) { const m = Buffer.concat(frag); frag = []; onMessage(m); }
    }
  });
  sock.on('close', () => { if (!closed) { closed = true; onClose(); } });
  sock.on('error', () => {});
  return ws;
}

function attach(server) {
  server.on('upgrade', (req, sock) => {
    let url;
    try { url = new URL(req.url, 'http://localhost'); } catch { sock.destroy(); return; }
    const key = req.headers['sec-websocket-key'];
    if (url.pathname !== '/net' || !key) { sock.destroy(); return; }
    const proto = url.searchParams.get('proto');
    let host = url.searchParams.get('host') || '';
    const port = Number(url.searchParams.get('port'));
    if (!(port > 0 && port < 65536) || (proto !== 'tcp' && proto !== 'udp')) { sock.destroy(); return; }
    if (host === 'dns') host = dnsServer();

    const accept = () => {
      sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
        + `Sec-WebSocket-Accept: ${crypto.createHash('sha1').update(key + GUID).digest('base64')}\r\n\r\n`);
      sock.setNoDelay(true);
    };

    if (proto === 'tcp') {
      // the page treats "open" as "connected": answer the handshake only
      // once the TCP connection is up, refuse it if the connect fails
      const out = net.connect({ host, port });
      out.on('error', () => { if (!out.ws) sock.end('HTTP/1.1 502 Bad Gateway\r\n\r\n'); });
      out.on('connect', () => {
        accept();
        const ws = out.ws = websocket(sock, (m) => {
          if (m.length === 0) { out.end(); return; }
          if (!out.write(m)) ws.pause();
        }, () => out.destroy());
        out.on('drain', () => ws.resume());
        out.on('data', (d) => {
          ws.send(d);
          if (ws.bufferedAmount > 1 << 20) { out.pause(); sock.once('drain', () => out.resume()); }
        });
        out.on('end', () => ws.close());
        out.on('close', () => ws.close());
      });
    } else {
      accept();
      const u = dgram.createSocket(net.isIPv6(host) ? 'udp6' : 'udp4');
      let timer;
      const idle = () => { clearTimeout(timer); timer = setTimeout(() => ws.close(), 120000); };
      const ws = websocket(sock, (m) => { idle(); u.send(m, port, host); }, () => { clearTimeout(timer); u.close(); });
      u.on('message', (m) => { idle(); ws.send(m); });
      u.on('error', () => ws.close());
      idle();
    }
  });
}

module.exports = { attach };
