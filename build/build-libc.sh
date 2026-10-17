#!/bin/sh
# Build the user space sysroot: kernel UAPI headers, musl (wasm32 port from
# port-musl.py) and compiler-rt builtins, all installed into $SYSROOT.
# Needs a configured kernel tree (it reads the generated unistd_32.h, so
# build vmlinux.wasm first).
#   sh build/build-libc.sh        (Linux: after . /opt/wltools/env)
set -e
. "${W:-/wl}/build/env.sh"
LOG=$W/build/obj/libc
mkdir -p $LOG "$SYSROOT/lib" "$SYSROOT/include"

sh $W/build/kmake.sh headers_install INSTALL_HDR_PATH="$SYSROOT" > $LOG/headers.log 2>&1 ||
	{ tail -20 $LOG/headers.log; exit 1; }

python3 $W/build/port-musl.py
cd $W/src/musl
if [ ! -f config.mak ]; then
	CC=clang AR=llvm-ar RANLIB=llvm-ranlib CFLAGS="$WASM_ARCH_FLAGS -fPIC -O2" ./configure \
		--target=wasm32-unknown-linux-musl --prefix="$SYSROOT" \
		--disable-shared CROSS_COMPILE=llvm- > $LOG/musl-configure.log 2>&1 ||
		{ tail -20 $LOG/musl-configure.log; exit 1; }
fi
make -j${JOBS:-8} lib/libc.a lib/crt1.o > $LOG/musl.log 2>&1 ||
	{ grep -E 'error:|Error [0-9]' $LOG/musl.log | head -30; exit 1; }
make install-headers > /dev/null
cp lib/libc.a lib/crt1.o "$SYSROOT/lib/"
# musl keeps libm, libpthread, ... inside libc.a; -lm etc. get empty stubs
for l in m rt pthread crypt util xnet resolv dl; do
	rm -f "$SYSROOT/lib/lib$l.a"; llvm-ar rc "$SYSROOT/lib/lib$l.a"
done

# compiler-rt builtins: every portable C file that builds for wasm32
B=$W/src/llvm-rt/compiler-rt/lib/builtins
O=$W/build/obj/builtins
rm -rf $O; mkdir -p $O
for f in $B/*.c; do
	n=$(basename $f .c)
	case $n in
	atomic|emutls|enable_execute_stack|clear_cache|gcc_personality_v0|\
	os_version_check|trampoline_setup|cpu_model|eprintf) continue ;;
	esac
	clang $WASM_ARCH_FLAGS -fPIC -O2 -nostdinc -isystem "$SYSROOT/include" \
		-isystem "$(clang -print-resource-dir)/include" -w \
		-c $f -o $O/$n.o 2>> $LOG/builtins.log || echo "skipped $n" >> $LOG/builtins.log
done
rm -f "$SYSROOT/lib/libclang_rt.builtins-wasm32.a"
llvm-ar rcs "$SYSROOT/lib/libclang_rt.builtins-wasm32.a" $O/*.o
echo "sysroot ready: $SYSROOT"
