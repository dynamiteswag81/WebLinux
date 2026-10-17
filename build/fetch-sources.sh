#!/bin/sh
# Fetch the third-party sources into linux/ and src/ (both gitignored) and
# apply the kernel port.  Safe to rerun: existing trees are left alone.
#   sh build/fetch-sources.sh
# linux/ becomes a git repo on branch 'wasm': a "vanilla baseline" commit of
# Linux 6.18.57 plus patches/linux/*.patch (the arch/wasm port).  After
# changing the kernel, commit there and refresh the patches with
#   git -C linux format-patch --binary -o ../patches/linux <baseline>..wasm
set -e
cd "$(dirname "$0")/.."
DL=src/dl
mkdir -p "$DL"
# get URL-DIR FILE [FALLBACK-URL]: download into $DL unless already there.
# The fallback (Ubuntu's source pool) serves the same upstream tarballs for
# networks where kernel.org/gnu.org/busybox.net are unreachable.
get() {
	[ -f "$DL/$2" ] && return 0
	curl -fL --retry 3 -o "$DL/$2" "$1/$2" ||
	{ [ -n "$3" ] && curl -fL --retry 3 -o "$DL/$2" "$3"; }
}
UBU=http://archive.ubuntu.com/ubuntu/pool

if [ ! -d linux ]; then
	if get https://cdn.kernel.org/pub/linux/kernel/v6.x linux-6.18.57.tar.xz; then
		tar xJf "$DL/linux-6.18.57.tar.xz"
		mv linux-6.18.57 linux
	else
		# fallback: the stable tree's GitHub mirror (same tree as the tarball)
		rm -f "$DL/linux-6.18.57.tar.xz"
		git init -q linux-git
		git -C linux-git fetch -q --depth 1 \
			https://github.com/gregkh/linux.git refs/tags/v6.18.57
		git -C linux-git -c advice.detachedHead=false checkout -q FETCH_HEAD
		rm -rf linux-git/.git
		mv linux-git linux
	fi
	(cd linux &&
	 git init -q -b wasm &&
	 git -c core.autocrlf=false add -A &&
	 git -c user.name=baseline -c user.email=baseline@localhost \
		commit -q -m "Linux 6.18.57 (vanilla upstream baseline)" &&
	 git am -q ../patches/linux/*.patch)
fi

if [ ! -d src/musl ]; then
	get https://musl.libc.org/releases musl-1.2.6.tar.gz \
		$UBU/universe/m/musl/musl_1.2.6.orig.tar.gz
	tar xzf "$DL/musl-1.2.6.tar.gz" -C src
	mv src/musl-1.2.6 src/musl
	echo "src/musl: run 'python3 build/port-musl.py' after a kernel build"
	echo "  (it reads linux/arch/wasm/include/generated/uapi/asm/unistd_32.h)"
fi

if [ ! -d src/bash-5.3 ]; then
	get https://ftp.gnu.org/gnu/bash bash-5.3.tar.gz \
		$UBU/main/b/bash/bash_5.3.orig.tar.xz
	tar xf "$DL/bash-5.3.tar.gz" -C src
fi

if [ ! -d src/busybox-1.37.0 ]; then
	get https://busybox.net/downloads busybox-1.37.0.tar.bz2 \
		$UBU/main/b/busybox/busybox_1.37.0.orig.tar.bz2
	tar xjf "$DL/busybox-1.37.0.tar.bz2" -C src
fi

# compiler-rt builtins (sparse llvm-project checkout, pinned)
if [ ! -d src/llvm-rt ]; then
	git clone -q --filter=blob:none --sparse --no-checkout \
		https://github.com/llvm/llvm-project.git src/llvm-rt
	git -C src/llvm-rt sparse-checkout set compiler-rt/lib/builtins
	git -C src/llvm-rt -c advice.detachedHead=false checkout -q \
		0d261d1ca552c95a8f007e061c787ac7132fbcbc
fi
echo "sources ready"
