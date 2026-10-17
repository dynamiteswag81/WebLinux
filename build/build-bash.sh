#!/bin/sh
# Cross-build GNU bash for WebLinux (MSYS2: m < build-bash.sh; Linux: sh build/build-bash.sh).
# fork() works through Asyncify (WASM_FORK=1 in wasm-cc).
set -e
. "${W:-/wl}/build/env.sh"
SRC=$W/src/bash-5.3
OUT=$W/build/obj/bash
mkdir -p $OUT
cd $OUT

if [ ! -f Makefile ]; then
	# answers for tests configure can't run when cross compiling
	export bash_cv_func_sigsetjmp=present bash_cv_job_control_missing=present \
	bash_cv_sys_named_pipes=present bash_cv_getcwd_malloc=yes \
	bash_cv_func_strcoll_broken=no bash_cv_func_ctype_nonascii=no \
	bash_cv_dup2_broken=no bash_cv_pgrp_pipe=no bash_cv_sys_siglist=yes \
	bash_cv_under_sys_siglist=yes bash_cv_opendir_not_robust=no \
	bash_cv_printf_a_format=yes bash_cv_wcwidth_broken=no \
	bash_cv_unusable_rtsigs=no bash_cv_must_reinstall_sighandlers=no \
	bash_cv_wexitstatus_offset=8 bash_cv_fnmatch_equiv_fallback=no \
	bash_cv_func_snprintf=yes bash_cv_func_vsnprintf=yes \
	ac_cv_func_mmap_fixed_mapped=no ac_cv_rl_version=8.3 \
	gt_cv_int_divbyzero_sigfpe=no

	CC="bash $W/build/wasm-cc" CC_FOR_BUILD="/usr/bin/gcc -std=gnu17" \
	AR=llvm-ar RANLIB=llvm-ranlib CFLAGS="-O2" \
	$SRC/configure --host=wasm32-unknown-linux-musl --build=$(sh $SRC/support/config.guess) \
		--prefix=/usr --without-bash-malloc --disable-nls --with-curses=no \
		--enable-readline --enable-job-control > configure.log 2>&1 \
		|| { tail -30 configure.log; exit 1; }
	# PGRP_PIPE makes a job's first child wait on a pipe until the parent
	# has run; our fork() runs the child until it execs, so that deadlocks.
	sed -i 's|^#define PGRP_PIPE 1|/* #undef PGRP_PIPE (WebLinux fork) */|' config.h
fi

WASM_FORK=1 make -j16 > make.log 2>&1 || { grep -E 'error|Error' make.log | head -30; exit 1; }
ls -la bash
