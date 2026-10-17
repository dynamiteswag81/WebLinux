#!/bin/sh
# Build mke2fs, tune2fs and e2fsck from e2fsprogs into build/obj/e2fsprogs/.  init
# formats the browser disk with a journal (BusyBox's mke2fs makes ext2
# without one) and checks it at every boot.
set -e
. "${W:-/wl}/build/env.sh"
O=$W/build/obj/e2fsprogs
CC="bash $W/build/wasm-cc"
mkdir -p $O && cd $O
if [ ! -f Makefile ]; then
	# musl has no LFS64 names: 64-bit offsets are the default
	ac_cv_func_lseek64=yes CC="$CC" BUILD_CC=gcc CFLAGS="-O2" AR=llvm-ar RANLIB=llvm-ranlib \
	CPPFLAGS="-Dlseek64=lseek -Doff64_t=off_t -Dstat64=stat -Dfstat64=fstat -Dlstat64=lstat -Dopen64=open" \
	$W/src/e2fsprogs/configure --host=wasm32-unknown-linux-musl --prefix=/usr \
		--disable-nls --disable-shared --disable-fuse2fs --disable-defrag \
		--disable-imager --disable-resizer --disable-debugfs --disable-e2initrd-helper \
		--disable-tdb --disable-uuidd --disable-mmp --disable-tls --disable-backtrace \
		--without-libarchive --disable-rpath > configure.log 2>&1 ||
		{ tail -20 configure.log; exit 1; }
fi
make -j8 libs > build.log 2>&1 || { grep -E 'error|Error' build.log | head -20; exit 1; }
make -j8 -C misc mke2fs tune2fs >> build.log 2>&1 || { grep -E 'error|Error' build.log | head -20; exit 1; }
make -j8 -C e2fsck e2fsck >> build.log 2>&1 || { grep -E 'error|Error' build.log | head -20; exit 1; }
ls -la misc/mke2fs misc/tune2fs e2fsck/e2fsck
