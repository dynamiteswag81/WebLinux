# WebLinux

Linux running natively as WebAssembly in a browser tab (our own arch port,
no x86 emulation). Repo: https://github.com/dynamiteswag81/WebLinux

Read **STATUS.md** first: goal, the user's rules, layout, design, what works
and the next steps. Keep it current; it is how one session hands over to the
next.

## Sources
`linux/` and `src/` are not in git. `sh build/fetch-sources.sh` downloads
Linux 6.18.57, musl 1.2.6, bash 5.3, BusyBox 1.37.0 and compiler-rt, and
rebuilds `linux/` as a git repo on branch `wasm` = vanilla baseline commit +
`patches/linux/*.patch` (the `arch/wasm` port). Kernel work is committed in
`linux/` and exported back to `patches/linux/` with `git format-patch`
(the script's header has the command); commit the patches here.

## Toolchain
The original machine is Windows with MSYS2 (paths in STATUS.md and
`build/*.sh`: the project is mounted at `/wl`, the sysroot is
`/c/Users/dynam/wltools/sysroot`). On Linux, install clang/lld (LLVM 19+ with
the wasm32 target), binaryen (`wasm-opt`), python3, node 24+, and either
symlink the repo to `/wl` and point `SYSROOT` somewhere without spaces, or
make the scripts take those from the environment.

## Run
`node server.js 8080` serves `public/` with COOP/COEP (SharedArrayBuffer).
`public/vmlinux.wasm` and `public/initramfs.cpio` are the last built ones.
