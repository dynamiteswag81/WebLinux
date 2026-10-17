#!/bin/sh
# Run the kernel build for ARCH=wasm inside MSYS2 (CLANG64).
# Host tools must be POSIX (MSYS) binaries; the target is wasm32 clang.
cd /wl/linux || exit 1
exec make ARCH=wasm LLVM=1 \
	HOSTCC=/usr/bin/gcc HOSTCXX=/usr/bin/g++ HOSTLD=/usr/bin/ld HOSTAR=/usr/bin/ar \
	"$@"
