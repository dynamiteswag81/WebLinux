#!/bin/sh
# Start WebLinux: serve the page and open it in the default browser.
# Needs Node.js.  Ctrl+C stops it.
cd "$(dirname "$0")"
command -v node >/dev/null || { echo "Node.js is not installed: get it from https://nodejs.org"; exit 1; }
URL=http://127.0.0.1:${PORT:-8080}/
( sleep 1; xdg-open "$URL" 2>/dev/null || open "$URL" 2>/dev/null ) &
exec node server.js "${PORT:-8080}"
