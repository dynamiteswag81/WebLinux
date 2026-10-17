#!/bin/sh
# Install the WebLinux toolchain on Ubuntu/Debian (the cloud sessions) and
# write $T/env, which sets PATH, W and SYSROOT.  Then:  . /opt/wltools/env
#   clang/lld/llvm 20 (apt), binaryen 132 and node 24 (npm registry),
#   /wl -> this checkout.
set -e
T=${WLTOOLS:-/opt/wltools}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
{ command -v clang-20 && command -v flex && command -v bison; } >/dev/null ||
	{ apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q clang-20 lld-20 llvm-20 flex bison bc make gcc g++ rsync cpio; }
mkdir -p "$T/sysroot/lib" "$T/sysroot/include"
[ -d "$T/node_modules/playwright" ] || npm install -s --prefix "$T" node@24 binaryen@132 playwright@1.56.1
[ -e /wl ] || ln -s "$ROOT" /wl
cat > "$T/env" <<E
export PATH=/usr/lib/llvm-20/bin:$T/node_modules/.bin:\$PATH
export W=/wl SYSROOT=$T/sysroot
E
echo "toolchain ready: . $T/env"
