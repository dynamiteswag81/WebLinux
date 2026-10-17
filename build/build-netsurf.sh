#!/bin/sh
# Cross-build NetSurf's framebuffer frontend and everything it needs into
# $SYSROOT, then the browser itself into build/obj/netsurf/netsurf.
#   sh build/build-netsurf.sh [step...]     (default: all steps in order)
# Steps: zlib mbedtls curl png jpeg expat nslibs netsurf.  Logs: build/obj/netsurf/*.log
set -e
. "${W:-/wl}/build/env.sh"
O=$W/build/obj/netsurf
mkdir -p $O
CC="bash $W/build/wasm-cc"
export AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm STRIP=true

fail() { echo "$1 failed, see $2"; grep -E 'error:|Error [0-9]|undefined symbol' "$2" | cut -c1-300 | head -20; exit 1; }

step_zlib() {
	cd $W/src/zlib
	CC="$CC" CFLAGS="-O2" ./configure --static --prefix="$SYSROOT" > $O/zlib.log 2>&1 || fail zlib $O/zlib.log
	make -j8 libz.a >> $O/zlib.log 2>&1 && make install >> $O/zlib.log 2>&1 || fail zlib $O/zlib.log
}

step_mbedtls() {
	cd $W/src/mbedtls
	# no threads, no networking layer of its own (curl does the sockets)
	make -C library -j8 CC="$CC" CFLAGS="-O2 -I../include" AR=llvm-ar \
		libmbedcrypto.a libmbedx509.a libmbedtls.a > $O/mbedtls.log 2>&1 || fail mbedtls $O/mbedtls.log
	cp library/libmbed*.a "$SYSROOT/lib/"
	rm -rf "$SYSROOT/include/mbedtls" "$SYSROOT/include/psa"
	cp -r include/mbedtls include/psa "$SYSROOT/include/"
}

step_curl() {
	mkdir -p $O/curl && cd $O/curl
	if [ ! -f Makefile ]; then
		CC="$CC" CFLAGS="-O2" CPPFLAGS="-I$SYSROOT/include" LDFLAGS="-L$SYSROOT/lib" \
		$W/src/curl/configure --host=wasm32-unknown-linux-musl --prefix="$SYSROOT" \
			--disable-shared --enable-static --with-mbedtls="$SYSROOT" --with-zlib="$SYSROOT" \
			--disable-threaded-resolver --disable-pthreads --disable-ldap --disable-ldaps \
			--disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
			--disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
			--disable-manual --disable-docs --disable-ipv6 --without-libpsl --without-brotli \
			--without-zstd --without-nghttp2 --without-libidn2 --without-librtmp \
			--disable-unix-sockets --without-ca-bundle --without-ca-path \
			--with-ca-fallback=no > $O/curl.log 2>&1 || fail curl $O/curl.log
	fi
	make -j8 >> $O/curl.log 2>&1 || fail curl $O/curl.log
	make install >> $O/curl.log 2>&1 || fail curl $O/curl.log
}

step_png() {
	mkdir -p $O/png && cd $O/png
	[ -f Makefile ] || CC="$CC" CFLAGS="-O2" CPPFLAGS="-I$SYSROOT/include" LDFLAGS="-L$SYSROOT/lib" \
		$W/src/libpng/configure --host=wasm32-unknown-linux-musl --prefix="$SYSROOT" \
		--disable-shared --enable-static --disable-tools --disable-tests \
		> $O/png.log 2>&1 || fail libpng $O/png.log
	make -j8 >> $O/png.log 2>&1 && make install >> $O/png.log 2>&1 || fail libpng $O/png.log
}

step_jpeg() {
	mkdir -p $O/jpeg && cd $O/jpeg
	cmake $W/src/libjpeg-turbo -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=wasm32 \
		-DCMAKE_C_COMPILER=$W/build/wasm-cc -DCMAKE_AR=$(command -v llvm-ar) \
		-DCMAKE_RANLIB=$(command -v llvm-ranlib) -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
		-DENABLE_SHARED=0 -DENABLE_STATIC=1 -DWITH_SIMD=0 -DWITH_TURBOJPEG=0 \
		-DWITH_TOOLS=0 -DWITH_TESTS=0 > $O/jpeg.log 2>&1 || fail libjpeg $O/jpeg.log
	make -j8 jpeg-static >> $O/jpeg.log 2>&1 || fail libjpeg $O/jpeg.log
	cp libjpeg.a "$SYSROOT/lib/"
	cp jconfig.h $W/src/libjpeg-turbo/src/jpeglib.h $W/src/libjpeg-turbo/src/jmorecfg.h \
		$W/src/libjpeg-turbo/src/jerror.h "$SYSROOT/include/"
}

step_expat() {
	mkdir -p $O/expat && cd $O/expat
	cmake $W/src/expat/expat -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=wasm32 \
		-DCMAKE_C_COMPILER=$W/build/wasm-cc -DCMAKE_AR=$(command -v llvm-ar) \
		-DCMAKE_RANLIB=$(command -v llvm-ranlib) -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DEXPAT_SHARED_LIBS=OFF \
		-DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
		-DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_PKGCONFIG=ON > $O/expat.log 2>&1 || fail expat $O/expat.log
	make -j8 >> $O/expat.log 2>&1 && make install >> $O/expat.log 2>&1 || fail expat $O/expat.log
}

NS=$W/src/netsurf-all-3.11
NSHOST=wasm32-unknown-linux-musl
NSLIBS="libnslog libwapcaplet libparserutils libcss libhubbub libdom libnsbmp libnsgif
	libnsutils libutf8proc libnspsl libsvgtiny libnsfb"
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"

# our changes to NetSurf (patches/netsurf/*.patch), applied once
apply_patches() {
	for p in $W/patches/netsurf/*.patch; do
		patch -d $NS -p1 -N --dry-run -s < $p > /dev/null 2>&1 || continue
		patch -d $NS -p1 -N -s < $p
	done
}

step_nslibs() {
	apply_patches
	# nsgenbind runs at build time: host compiler
	make -C $NS/nsgenbind install PREFIX=$O/host Q= DESTDIR= NSSHARED=$NS/buildsystem > $O/nsgenbind.log 2>&1 ||
		fail nsgenbind $O/nsgenbind.log
	# libnsfb: our Linux fbdev/evdev surface
	cp $W/userland/netsurf/linux.c $NS/libnsfb/src/surface/linux.c
	grep -q 'linux.c' $NS/libnsfb/src/surface/Makefile ||
		sed -i 's/^SURFACE_HANDLER_yes := surface.c ram.c/SURFACE_HANDLER_yes := surface.c ram.c linux.c/' \
			$NS/libnsfb/src/surface/Makefile
	for L in $NSLIBS; do
		echo "   $L"
		make -C $NS/$L install HOST=$NSHOST PREFIX="$SYSROOT" DESTDIR= Q= \
			CC="$CC" AR=llvm-ar NSSHARED=$NS/buildsystem WARNFLAGS='-w' \
			PATH="$O/host/bin:$PATH" > $O/$L.log 2>&1 || fail $L $O/$L.log
	done
}

step_netsurf() {
	apply_patches
	make -C $NS/netsurf TARGET=framebuffer HOST=$NSHOST PREFIX=/usr Q= \
		CC="$CC" BUILD_CC=cc AR=llvm-ar NSSHARED=$NS/buildsystem WARNFLAGS='-w' \
		PATH="$O/host/bin:$PATH" \
		NETSURF_FB_FRONTEND=linux NETSURF_USE_DUKTAPE=${NETSURF_JS:-YES} \
		NETSURF_USE_OPENSSL=NO NETSURF_USE_LIBICONV_PLUG=NO NETSURF_USE_WEBP=NO \
		NETSURF_USE_JPEGXL=NO NETSURF_USE_ROSPRITE=NO NETSURF_USE_VIDEO=NO \
		NETSURF_FB_RESPATH='$${HOME}/.netsurf/:/usr/share/netsurf' \
		> $O/netsurf.log 2>&1 || fail netsurf $O/netsurf.log
	rm -rf $O/install
	make -C $NS/netsurf install TARGET=framebuffer HOST=$NSHOST PREFIX=/usr DESTDIR=$O/install \
		Q= CC="$CC" BUILD_CC=cc NSSHARED=$NS/buildsystem NETSURF_FB_FRONTEND=linux \
		NETSURF_USE_DUKTAPE=${NETSURF_JS:-YES} NETSURF_USE_OPENSSL=NO NETSURF_USE_LIBICONV_PLUG=NO \
		NETSURF_USE_WEBP=NO NETSURF_USE_JPEGXL=NO NETSURF_USE_ROSPRITE=NO \
		>> $O/netsurf.log 2>&1 || fail "netsurf install" $O/netsurf.log
	llvm-strip --strip-debug -o $O/netsurf $NS/netsurf/nsfb
	ls -la $O/netsurf; du -sh $O/install/usr/share/netsurf
}

for s in ${@:-zlib mbedtls curl png jpeg expat nslibs netsurf}; do
	echo "== $s"
	step_$s
done
