#!/usr/bin/env node
// WebLinux host server.
//
// Serves public/ with the headers that make the page cross-origin isolated
// (SharedArrayBuffer and wasm threads need them), and relays the guest's
// TCP/UDP connections to the real network over a WebSocket (see netrelay).
//
// Zero dependencies: node server.js [port]

'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const PORT = Number(process.argv[2] || process.env.PORT || 8080);
const HOST = process.env.HOST || '127.0.0.1';
const ROOT = path.join(__dirname, 'public');

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
};

function isolationHeaders(res) {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
}

const COMPRESS = new Set(['.wasm', '.cpio', '.js', '.html', '.css']);
const gzCache = new Map(); // file -> { mtime, size, gz }
function gzipped(file, st, cb) {
  const c = gzCache.get(file);
  if (c && c.mtime === st.mtimeMs && c.size === st.size) return cb(c.gz);
  fs.readFile(file, (err, data) => {
    if (err) return cb(null);
    zlib.gzip(data, { level: 6 }, (err2, gz) => {
      if (err2) return cb(null);
      gzCache.set(file, { mtime: st.mtimeMs, size: st.size, gz });
      cb(gz);
    });
  });
}

const server = http.createServer((req, res) => {
  isolationHeaders(res);
  let url;
  try {
    url = new URL(req.url, 'http://localhost');
  } catch {
    res.writeHead(400).end();
    return;
  }
  let rel = decodeURIComponent(url.pathname);
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.normalize(path.join(ROOT, rel));
  if (!file.startsWith(ROOT)) {
    res.writeHead(403).end();
    return;
  }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) {
      res.writeHead(404, { 'Content-Type': 'text/plain' }).end('not found');
      return;
    }
    // the kernel and the root file system are big: send them gzipped
    if (COMPRESS.has(path.extname(file)) && /\bgzip\b/.test(req.headers['accept-encoding'] || '')) {
      gzipped(file, st, (gz) => {
        if (!gz) return res.writeHead(500).end();
        res.writeHead(200, {
          'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream',
          'Content-Encoding': 'gzip',
          'Content-Length': gz.length,
          'Cache-Control': 'no-cache',
          'Vary': 'Accept-Encoding',
        });
        res.end(req.method === 'HEAD' ? undefined : gz);
      });
      return;
    }
    res.writeHead(200, {
      'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream',
      'Content-Length': st.size,
      'Cache-Control': 'no-cache',
    });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file).pipe(res);
  });
});

let relay = null;
try {
  relay = require('./host/netrelay.js');
} catch (e) {
  if (e.code !== 'MODULE_NOT_FOUND') throw e;
}
if (relay) relay.attach(server);

server.listen(PORT, HOST, () => {
  console.log(`WebLinux running at http://localhost:${PORT}/`);
  if (relay) console.log('network relay: ws://localhost:' + PORT + '/net');
});
