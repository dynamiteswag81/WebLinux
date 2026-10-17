#!/bin/sh
# Assemble the root file system and pack it as public/initramfs.cpio:
# rootfs/ (static files + manifest), /sbin/init (userland/init.c), bash,
# BusyBox with one symlink per applet and the desktop (/usr/bin/wldesk).
# Build bash, BusyBox and the desktop first.
set -e
. "${W:-/wl}/build/env.sh"
R=$W/build/obj/rootfs
BB=$W/src/busybox-1.37.0
rm -rf $R
mkdir -p $R/bin $R/sbin $R/usr/bin $R/usr/sbin
cp -r $W/rootfs/. $R/
rm $R/manifest
bash $W/build/wasm-cc -O2 -Wall $W/userland/init.c -o $R/sbin/init
cp $W/build/obj/bash/bash $R/bin/bash
cp $BB/busybox_unstripped $R/bin/busybox
[ ! -f $W/build/obj/desktop/wldesk ] || cp $W/build/obj/desktop/wldesk $R/usr/bin/
NSO=$W/build/obj/netsurf
if [ -f $NSO/netsurf ]; then
	cp $NSO/netsurf $R/usr/bin/netsurf
	mkdir -p $R/usr/share/netsurf
	cp $NSO/install/usr/share/netsurf/* $R/usr/share/netsurf/
	cp $W/rootfs/usr/share/netsurf/Choices $R/usr/share/netsurf/
fi
[ ! -f $NSO/curl/src/curl ] || cp $NSO/curl/src/curl $R/usr/bin/curl
# root certificates for HTTPS (Mozilla's set, from the build machine)
if [ -d /usr/share/ca-certificates/mozilla ]; then
	mkdir -p $R/etc/ssl/certs
	cat /usr/share/ca-certificates/mozilla/*.crt > $R/etc/ssl/certs/ca-certificates.crt
fi
[ -z "$ROOTFS_EXTRA" ] || cp $ROOTFS_EXTRA $R/bin/

M=$W/build/obj/rootfs.manifest
cp $W/rootfs/manifest $M
[ -f $BB/busybox.links ] || (cd $BB && make busybox.links > /dev/null)
for l in $(cat $BB/busybox.links); do
	[ -e $R$l ] || grep -qF " $l " $M || echo "slink $l /bin/busybox 777 0 0" >> $M
done
node $W/build/mkinitramfs.js $R $M $W/public/initramfs.cpio
