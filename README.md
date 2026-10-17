# WebLinux

Linux running natively as WebAssembly in a browser tab.

This is not an emulator. WebLinux is a port of the Linux kernel (6.18) to a new
architecture, `wasm`: the kernel is compiled to a WebAssembly module, and user programs
(musl libc, bash, BusyBox) are compiled to WebAssembly too. Every task runs in a Web
Worker on one shared memory, and the page provides the "hardware": a timer, a console,
and soon a framebuffer, input, disks, network and sound.

> **Vibe coded.** The whole project, including the kernel port, the libc port, the
> build scripts and the web front end, was written by AI (Claude Code). A human set
> the goals and the rules; the code is generated. Read it with that in mind.

## Status

Working today:
- the kernel boots in the browser (no MMU, one CPU, one worker per task),
- musl libc, `fork()` and `vfork()`, signals,
- `/sbin/init`, then **bash 5.3** as a login shell with job control (`&`, `jobs`,
  Ctrl-C, Ctrl-Z, `bg`, `fg`),
- **BusyBox 1.37** for the usual Unix tools,
- a **desktop** on the frame buffer (windows, taskbar, terminal, file manager, editor),
  with keyboard and mouse,
- **persistence**: the root file system is an overlay on a disk kept in the browser's
  storage (OPFS), so changes survive closing the tab,
- **networking**: an Ethernet device routed by a small NAT in the page; TCP and UDP go
  out through the page server,
- the **NetSurf** web browser (HTML, CSS, images, JavaScript),
- **sound** through Web Audio (ALSA, and OSS `/dev/dsp`).
[STATUS.md](STATUS.md) has the details and the plan.

## Run it

You need Node.js. From a checkout:

```sh
node server.js 8080
```

and open <http://127.0.0.1:8080/>. The server adds the COOP/COEP headers the page needs
for `SharedArrayBuffer` and relays the guest's network connections (it can reach whatever
the machine running it can, so it only listens on localhost). Page options:
`?mode=1280x720` (screen size), `?mem=1024` (MiB), `?disk=mem` (don't persist),
`?cmdline=...`. `public/` holds the last built kernel (`vmlinux.wasm`)
and root file system (`initramfs.cpio`), so no build is needed to try it.

## Build it

On Ubuntu/Debian:

```sh
sh build/setup-linux.sh        # clang/lld 20, binaryen, node 24, playwright
. /opt/wltools/env
sh build/fetch-sources.sh      # Linux 6.18.57 + the wasm port, musl, bash, BusyBox
cd /wl/linux && sh /wl/build/kmake.sh wasm_defconfig && sh /wl/build/kmake.sh -j8 vmlinux.wasm
cp vmlinux.wasm /wl/public/ && cd /wl
sh build/build-libc.sh         # sysroot: kernel headers, musl, compiler-rt builtins
sh build/build-bash.sh
sh build/build-busybox.sh
sh build/build-desktop.sh
sh build/build-netsurf.sh      # curl, mbedTLS, image libraries, NetSurf (slow)
sh build/mkrootfs.sh           # -> public/initramfs.cpio
NODE_PATH=/opt/wltools/node_modules node test/boot-test.mjs test/jobs.json
```

The kernel port lives in `patches/linux/` (applied by `fetch-sources.sh` to a vanilla
tree in `linux/`), the musl port in `build/port-musl.py`.

## License

GPL-2.0 (see [LICENSE](LICENSE)). The kernel port is a modification of Linux, so the
project is under the kernel's license. Bundled third-party programs keep their own
licenses (musl: MIT, bash: GPL-3.0, BusyBox: GPL-2.0).

Copyright dynamiteswag81.
