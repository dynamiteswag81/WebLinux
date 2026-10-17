#!/bin/sh
# Run the kernel build for ARCH=wasm (MSYS2 CLANG64 or Linux).
# Host tools must be POSIX binaries (MSYS gcc on Windows); the target is
# wasm32 clang.
cd "${W:-/wl}/linux" || exit 1
exec make ARCH=wasm LLVM=1 \
	HOSTCC=/usr/bin/gcc HOSTCXX=/usr/bin/g++ HOSTLD=/usr/bin/ld HOSTAR=/usr/bin/ar \
	"$@"
