#!/bin/sh
# Build the desktop (userland/desktop) into build/obj/desktop/wldesk.
# The 8x16 font comes from the kernel tree (lib/fonts/font_8x16.c).
set -e
. "${W:-/wl}/build/env.sh"
S=$W/userland/desktop
O=$W/build/obj/desktop
mkdir -p $O
python3 - $W/linux/lib/fonts/font_8x16.c $O/font8x16.h <<'PY'
import re, sys
src = open(sys.argv[1]).read()
body = src[src.index('FONTDATAMAX, 0 }, {') + 19:]
body = re.sub(r'/\*.*?\*/', '', body[:body.index('} };')], flags=re.S)
bytes_ = re.findall(r'0x([0-9a-fA-F]{2})', body)
assert len(bytes_) == 4096, len(bytes_)
with open(sys.argv[2], 'w') as f:
    f.write('/* generated from linux/lib/fonts/font_8x16.c (GPL-2.0) */\n')
    f.write('static const unsigned char font8x16[4096] = {\n')
    for i in range(0, 4096, 16):
        f.write('\t' + ', '.join('0x' + b for b in bytes_[i:i + 16]) + ',\n')
    f.write('};\n')
PY
bash $W/build/wasm-cc -O2 -Wall -Wno-unused-result -I$O $S/desk.c $S/gfx.c $S/term.c -o $O/wldesk
ls -la $O/wldesk
