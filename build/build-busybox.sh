#!/bin/sh
# Cross-build BusyBox utilities for WebLinux (run inside MSYS2).
# NOMMU mode (vfork + exec), no shells: bash is the shell.
. "${W:-/wl}/build/env.sh"
SRC=$W/src/busybox-1.37.0
[ -d $SRC ] || { echo "run build/fetch-sources.sh first"; exit 1; }
cd $SRC
# our fixes (patches/busybox/*.patch), applied once
for p in $W/patches/busybox/*.patch; do
	patch -p1 -N --dry-run -s < $p > /dev/null 2>&1 && patch -p1 -N -s < $p
done

BBMAKE="make CC=bash\ $W/build/wasm-cc HOSTCC=/usr/bin/gcc HOSTCFLAGS=-std=gnu17 LD=$W/build/wasm-ld-r AR=llvm-ar STRIP=true"

if [ ! -f .config ]; then
	eval $BBMAKE defconfig > /dev/null
	# turn options off/on
	for o in ASH HUSH SH_IS_ASH SH_IS_HUSH SH_IS_NONE BASH_IS_ASH BASH_IS_HUSH \
		SELINUX FEATURE_HAVE_RPC PAM FEATURE_UTMP FEATURE_WTMP \
		STATIC PIE BUILD_LIBBUSYBOX FEATURE_SHARED_BUSYBOX \
		FEATURE_SUID FEATURE_SYSLOG_INFO LINUXRC INIT \
		TC FEATURE_TC_INGRESS UBIATTACH UBIDETACH UBIMKVOL UBIRMVOL UBIRSVOL UBIUPDATEVOL UBIRENAME \
		HWCLOCK NANDWRITE NANDDUMP FLASHCP FLASH_LOCK FLASH_UNLOCK FLASH_ERASEALL \
		LOADFONT SETFONT LOADKMAP KBD_MODE SETKEYCODES SHOWKEY OPENVT CHVT DEALLOCVT FGCONSOLE \
		INSMOD RMMOD LSMOD MODPROBE DEPMOD MODINFO \
		SWAPON SWAPOFF MKSWAP FSCK_MINIX MKFS_MINIX FEATURE_MINIX2 \
		ZCIP UDHCPD DHCPRELAY DUMPLEASES FEATURE_IPV6 SHA1_HWACCEL SHA256_HWACCEL ; do
		sed -i "s/^CONFIG_$o=.*/# CONFIG_$o is not set/" .config
	done
	sed -i 's/^# CONFIG_NOMMU is not set/CONFIG_NOMMU=y/' .config
	grep -q '^CONFIG_NOMMU=y' .config || echo CONFIG_NOMMU=y >> .config
	yes '' | eval $BBMAKE oldconfig > /dev/null 2>&1
fi

eval $BBMAKE -j16 -k > make.log 2>&1
grep -E 'error:' make.log | sed 's/:[0-9]*:[0-9]*:/:/' | sort | uniq -c | sort -rn | head -30
ls -la busybox_unstripped
