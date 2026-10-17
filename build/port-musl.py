#!/usr/bin/env python3
# Adds the wasm32 (WebLinux) architecture to the musl source tree in
# src/musl.  Idempotent: rerunning rewrites the port files.
#
#   python build/port-musl.py

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MUSL = os.path.join(ROOT, 'src', 'musl')
KUNISTD = os.path.join(ROOT, 'linux', 'arch', 'wasm', 'include', 'generated',
                       'uapi', 'asm', 'unistd_32.h')


def write(rel, text):
    p = os.path.join(MUSL, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', newline='\n') as f:
        f.write(text.lstrip('\n'))


def patch(rel, old, new):
    p = os.path.join(MUSL, rel)
    s = open(p, newline='').read()
    if new in s:
        return
    assert old in s, (rel, old[:60])
    s = s.replace(old, new, 1)
    open(p, 'w', newline='').write(s)


def copy(src_rel, dst_rel):
    s = open(os.path.join(MUSL, src_rel), newline='').read()
    write(dst_rel, s)


# ---------------------------------------------------------------- configure
patch('configure', 'riscv32*) ARCH=riscv32 ;;',
      'riscv32*) ARCH=riscv32 ;;\nwasm32*) ARCH=wasm32 ;;')

# ---------------------------------------------------------------- arch/wasm32
write('arch/wasm32/bits/alltypes.h.in', r'''
#define _Addr long
#define _Int64 long long
#define _Reg long

#define __BYTE_ORDER 1234
#define __LONG_MAX 0x7fffffffL

#ifndef __cplusplus
TYPEDEF int wchar_t;
#endif

TYPEDEF int blksize_t;
TYPEDEF unsigned int nlink_t;

TYPEDEF float float_t;
TYPEDEF double double_t;

TYPEDEF struct { long long __ll; long double __ld; } max_align_t;
''')

copy('arch/riscv32/bits/float.h', 'arch/wasm32/bits/float.h')
for f in ('ipcstat.h', 'msg.h', 'sem.h', 'shm.h'):
    copy('arch/riscv32/bits/' + f, 'arch/wasm32/bits/' + f)

write('arch/wasm32/bits/setjmp.h', r'''
/* see src/setjmp/wasm32/rt.c: invocation id, label, longjmp argument */
typedef unsigned long __jmp_buf[8];
''')

write('arch/wasm32/bits/user.h', r'''
#define ELF_NGREG 8
typedef unsigned long elf_greg_t, elf_gregset_t[ELF_NGREG];
typedef struct { unsigned long __dummy; } elf_fpregset_t;
''')

write('arch/wasm32/bits/signal.h', r'''
#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)

#if defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
# define MINSIGSTKSZ 2048
# define SIGSTKSZ 8192
#endif

/* WebAssembly has no user visible registers */
typedef struct mcontext_t {
	unsigned long __regs[8];
} mcontext_t;

#if defined(_GNU_SOURCE) || defined(_BSD_SOURCE)
typedef unsigned long greg_t;
typedef unsigned long gregset_t[8];
struct sigcontext {
	gregset_t gregs;
};
#endif

struct sigaltstack {
	void *ss_sp;
	int ss_flags;
	size_t ss_size;
};

typedef struct __ucontext
{
	unsigned long uc_flags;
	struct __ucontext *uc_link;
	stack_t uc_stack;
	sigset_t uc_sigmask;
	mcontext_t uc_mcontext;
} ucontext_t;

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#endif

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     SIGABRT
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED SIGSYS

#define _NSIG     65
''')

# syscall numbers straight from the kernel's generated header
lines = []
for line in open(KUNISTD):
    m = re.match(r'#define\s+(__NR_\w+)\s+(\d+)', line)
    if m and m.group(1) != '__NR_syscalls':
        lines.append('#define %s %s' % (m.group(1), m.group(2)))
lines += ['#define __NR__llseek __NR_llseek',
          '#define __NR_futex __NR_futex_time64',
          # only referenced from compile-time dead time32 fallbacks
          '#define __NR_nanosleep 101']
write('arch/wasm32/bits/syscall.h.in', '\n'.join(lines) + '\n')

write('arch/wasm32/kstat.h', '')

write('arch/wasm32/atomic_arch.h', r'''
#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	__atomic_compare_exchange_n((int *)p, &t, s, 0,
		__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
	return __atomic_exchange_n((int *)p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
	return __atomic_fetch_add((int *)p, v, __ATOMIC_SEQ_CST);
}

#define a_barrier a_barrier
static inline void a_barrier()
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define a_crash a_crash
static inline void a_crash()
{
	__builtin_trap();
}

#define a_ctz_32 a_ctz_32
static inline int a_ctz_32(unsigned long x)
{
	return __builtin_ctz(x);
}

#define a_clz_32 a_clz_32
static inline int a_clz_32(unsigned long x)
{
	return __builtin_clz(x);
}
''')

write('arch/wasm32/crt_arch.h', r'''
#ifndef _dlstart_c	/* not for the static-pie/ldso start files */
/*
 * The host instantiates the program and calls the exported _start with a
 * pointer to the argc/argv/envp/auxv block the kernel laid out.
 */
/* wasm traps on calls with the wrong arity, so find out which main the
 * program has: clang names main(int, char **) __main_argc_argv and
 * main(void) __main_void; main(int, char **, char **) stays "main". */
int __main_argc_argv(int, char **) __attribute__((__weak__));
int __main_void(void) __attribute__((__weak__));
int __wasm_main_envp(int, char **, char **) __asm__("main") __attribute__((__weak__));
static int __wasm_main3(int argc, char **argv, char **envp)
{
	if (__main_argc_argv)
		return __main_argc_argv(argc, argv);
	if (__main_void)
		return __main_void();
	return __wasm_main_envp(argc, argv, envp);
}
#define main __wasm_main3
void __wasm_call_ctors(void);
hidden void __wasm_libc_init(void);
hidden void _start_c(long *p);

__attribute__((export_name("_start")))
void _start(long *p)
{
	__wasm_libc_init();
	__wasm_call_ctors();
	_start_c(p);
}
#endif
''')

write('arch/wasm32/pthread_arch.h', r'''
extern uintptr_t __wasm_tp;

static inline uintptr_t __get_tp()
{
	return __wasm_tp;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 0

#define MC_PC __regs[0]
''')

write('arch/wasm32/reloc.h', r'''
#define LDSO_ARCH "wasm32"

#define TPOFF_K 0

#define REL_SYMBOLIC    1
#define REL_PLT         2
#define REL_RELATIVE    3
#define REL_COPY        4
#define REL_DTPMOD      5
#define REL_DTPOFF      6
#define REL_TPOFF       7

#define CRTJMP(pc,sp) __builtin_trap()
''')

write('arch/wasm32/syscall_arch.h', r'''
#define __SYSCALL_LL_E(x) \
((union { long long ll; long l[2]; }){ .ll = x }).l[0], \
((union { long long ll; long l[2]; }){ .ll = x }).l[1]
#define __SYSCALL_LL_O(x) __SYSCALL_LL_E((x))

/* src/misc/wasm32/wasm_syscall.c: enters the kernel, runs signal handlers */
long __wasm_syscall(long, long, long, long, long, long, long);

static inline long __syscall0(long n)
{
	return __wasm_syscall(n, 0, 0, 0, 0, 0, 0);
}

static inline long __syscall1(long n, long a)
{
	return __wasm_syscall(n, a, 0, 0, 0, 0, 0);
}

static inline long __syscall2(long n, long a, long b)
{
	return __wasm_syscall(n, a, b, 0, 0, 0, 0);
}

static inline long __syscall3(long n, long a, long b, long c)
{
	return __wasm_syscall(n, a, b, c, 0, 0, 0);
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
	return __wasm_syscall(n, a, b, c, d, 0, 0);
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	return __wasm_syscall(n, a, b, c, d, e, 0);
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	return __wasm_syscall(n, a, b, c, d, e, f);
}

#define IPC_64 0
''')

# ---------------------------------------------------------------- src
write('src/misc/wasm32/wasm_syscall.c', r'''
/*
 * System call entry for WebLinux.
 *
 * The kernel is imported directly ("linux"."syscall").  Signal handlers
 * can't be entered by the kernel, so it leaves a frame in the mailbox we
 * register at startup; after every call we run any pending handler here,
 * then rt_sigreturn restores the signal mask, and the call is restarted
 * if the kernel asked for that.
 */
#include <signal.h>
#include <stdint.h>
#include "syscall.h"

__attribute__((import_module("linux"), import_name("syscall")))
long __wasm_syscall_raw(long, long, long, long, long, long, long);

struct wasm_sigframe {
	uint32_t handler, flags, sig, restart;
	int32_t retval;
	uint32_t pad;
	uint64_t saved_mask;
	unsigned char info[128];
	unsigned char uc[64];
};

static struct wasm_sigmailbox {
	volatile uint32_t pending;
	uint32_t pad;
	struct wasm_sigframe frame;
} mailbox;

static void run_handlers(void)
{
	while (mailbox.pending) {
		struct wasm_sigframe f = mailbox.frame;
		mailbox.pending = 0;
		if (f.flags & SA_SIGINFO) {
			void (*h)(int, siginfo_t *, void *) = (void *)(uintptr_t)f.handler;
			h(f.sig, (siginfo_t *)f.info, f.uc);
		} else {
			void (*h)(int) = (void *)(uintptr_t)f.handler;
			h(f.sig);
		}
		/* restores the mask; may queue another frame */
		__wasm_syscall_raw(SYS_rt_sigreturn, (long)&f, 0, 0, 0, 0, 0);
	}
}

long __wasm_syscall(long n, long a, long b, long c, long d, long e, long f)
{
	long r = __wasm_syscall_raw(n, a, b, c, d, e, f);

	while (__builtin_expect(mailbox.pending, 0)) {
		int restart = mailbox.frame.restart;
		long ret = mailbox.frame.retval;
		run_handlers();
		r = restart ? __wasm_syscall_raw(n, a, b, c, d, e, f) : ret;
	}
	return r;
}

hidden void __wasm_libc_init(void)
{
	__wasm_syscall_raw(SYS_wasm_sigmailbox, (long)&mailbox, 0, 0, 0, 0, 0);
}
''')

write('src/setjmp/wasm32/rt.c', r'''
/*
 * Runtime for LLVM's WebAssembly setjmp/longjmp lowering
 * (-mexception-handling -mllvm -wasm-enable-sjlj).  The compiler turns
 * setjmp into __wasm_setjmp + a catch of the __c_longjmp tag, and longjmp
 * into __wasm_longjmp, which throws that tag with a pointer to {env, val}.
 *
 * jmp_buf layout (relied on by the host's vfork resume):
 *   [0] invocation id  [1] label  [2] arg.env  [3] arg.val
 */
#include <setjmp.h>
#include <stdint.h>

/* The exception tag lowered code throws and catches.  Exported from every
 * program so the host can resume a vfork parent by throwing it. */
__asm__(".globl __c_longjmp\n.tagtype __c_longjmp i32\n__c_longjmp:\n");

struct jmp_buf_impl {
	void *func_invocation_id;
	uint32_t label;
	struct arg {
		void *env;
		int val;
	} arg;
};

void __wasm_setjmp(void *env, uint32_t label, void *func_invocation_id)
{
	struct jmp_buf_impl *jb = env;
	if (label == 0 || func_invocation_id == 0)
		__builtin_trap();
	jb->func_invocation_id = func_invocation_id;
	jb->label = label;
}

uint32_t __wasm_setjmp_test(void *env, void *func_invocation_id)
{
	struct jmp_buf_impl *jb = env;
	if (jb->label == 0)
		__builtin_trap();
	if (jb->func_invocation_id == func_invocation_id)
		return jb->label;
	return 0;
}

void __wasm_longjmp(void *env, int val)
{
	struct jmp_buf_impl *jb = env;
	struct arg *arg = &jb->arg;
	if (val == 0)
		val = 1;
	arg->env = env;
	arg->val = val;
	__builtin_wasm_throw(1, arg);	/* 1: the C longjmp tag */
}

/* for callers that weren't lowered (function pointers, assembly-free libc) */
#undef longjmp
_Noreturn void longjmp(jmp_buf env, int val)
{
	struct jmp_buf_impl *jb = (void *)env;
	struct arg *arg = &jb->arg;
	if (val == 0)
		val = 1;
	arg->env = env;
	arg->val = val;
	__builtin_wasm_throw(1, arg);
}

weak_alias(longjmp, _longjmp);
''')

# longjmp() itself lives in rt.c (calls to it are lowered anyway)
p_ = os.path.join(MUSL, 'src/setjmp/wasm32/longjmp.c')
if os.path.exists(p_):
    os.remove(p_)

# no setjmp() function: every call is lowered by the compiler (rt.c)
p_ = os.path.join(MUSL, 'src/setjmp/wasm32/setjmp.c')
if os.path.exists(p_):
    os.remove(p_)

write('src/signal/wasm32/sigsetjmp.c', r'''
#include <setjmp.h>
#include <signal.h>
#include "syscall.h"

/* sigsetjmp() is a macro: save the mask here, then a lowered setjmp */
void __wasm_sigsetjmp_save(sigjmp_buf buf, int savemask)
{
	buf->__fl = savemask;
	if (savemask)
		__syscall(SYS_rt_sigprocmask, SIG_SETMASK, 0, buf->__ss, _NSIG/8);
}

#undef sigsetjmp
int sigsetjmp(sigjmp_buf buf, int savemask)
{
	__builtin_trap();
}
''')

write('src/signal/wasm32/siglongjmp.c', r'''
#include <setjmp.h>
#include <signal.h>
#include "syscall.h"

_Noreturn void siglongjmp(sigjmp_buf buf, int ret)
{
	if (buf->__fl)
		__syscall(SYS_rt_sigprocmask, SIG_SETMASK, buf->__ss, 0, _NSIG/8);
	longjmp(buf, ret);
}
''')

write('src/signal/wasm32/restore.c', r'''
/* no signal trampolines: handlers run from the syscall wrapper */
''')

write('src/thread/wasm32/__set_thread_area.c', r'''
#include <stdint.h>

uintptr_t __wasm_tp;

int __set_thread_area(void *p)
{
	__wasm_tp = (uintptr_t)p;
	return 0;
}
''')

write('src/thread/wasm32/__unmapself.c', r'''
#include "pthread_impl.h"

/* threads aren't supported, so a thread never has to unmap itself */
_Noreturn void __unmapself(void *base, size_t size)
{
	__syscall(SYS_munmap, base, size);
	for (;;) __syscall(SYS_exit, 0);
}
''')

write('src/thread/wasm32/syscall_cp.c', r'''
#include "pthread_impl.h"
#include "syscall.h"

/* No program counter to inspect: cancellation is checked up front only. */
hidden const char __cp_begin[1], __cp_end[1], __cp_cancel[1];
hidden long __cancel(void);

hidden long __syscall_cp_asm(volatile int *cancel, syscall_arg_t nr,
	syscall_arg_t u, syscall_arg_t v, syscall_arg_t w,
	syscall_arg_t x, syscall_arg_t y, syscall_arg_t z)
{
	if (*cancel)
		return __cancel();
	return __wasm_syscall(nr, u, v, w, x, y, z);
}
''')

write('src/thread/wasm32/clone.c', r'''
/*
 * Only the vfork-style clone (CLONE_VM|CLONE_VFORK) that posix_spawn uses
 * is possible: the child runs on the parent's stack until it execs or
 * exits, exactly what our vfork provides.  The stack argument is unused.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include "syscall.h"

int __clone(int (*func)(void *), void *stack, int flags, void *arg, ...)
{
	pid_t pid;

	if ((flags & (CLONE_VM|CLONE_VFORK)) != (CLONE_VM|CLONE_VFORK))
		return -ENOSYS;

	pid = vfork();
	if (pid == 0) {
		int r = func(arg);
		for (;;) __syscall(SYS_exit, r);
	}
	return pid < 0 ? -errno : pid;
}
''')

write('src/process/wasm32/vfork.c', r'''
/*
 * vfork for WebLinux.  A wasm function can't return twice, so vfork() is a
 * macro (see unistd.h): setjmp(slot) followed by __wasm_vfork().  The
 * child returns 0 straight from the kernel and keeps running on our stack.
 * When it execs or exits, the kernel makes the parent longjmp(slot, pid).
 */
#include <unistd.h>
#include <setjmp.h>
#include <errno.h>
#include "syscall.h"

#define MAX_NEST 16
static jmp_buf slots[MAX_NEST];
static int depth;

struct __jmp_buf_tag *__wasm_vfork_slot(void)
{
	return depth < MAX_NEST ? slots[depth] : 0;
}

pid_t __wasm_vfork(int r)
{
	long ret;

	if (r) {		/* parent, resumed by the kernel */
		depth--;
		return r;
	}
	if (depth >= MAX_NEST) {
		errno = EAGAIN;
		return -1;
	}
	ret = __syscall(SYS_wasm_vfork, (long)slots[depth]);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	depth++;
	return 0;		/* child */
}

#undef vfork
pid_t vfork(void)
{
	/* only usable through the macro, which sees the setjmp */
	errno = ENOSYS;
	return -1;
}
''')

write('src/process/wasm32/fork.c', r'''
/*
 * fork() for WebLinux.  The host saves this program's wasm call stack
 * with Asyncify (the program must be post-processed by wasm-opt --asyncify,
 * see build/wasm-cc WASM_FORK=1) and the kernel snapshots our memory; the
 * child continues from the saved stack and the parent is replayed from it
 * once the child execs or exits.  Programs without Asyncify get ENOSYS.
 */
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#define FORK_STACK_BUF (512 << 10)

__attribute__((import_module("linux"), import_name("fork")))
long __wasm_host_fork(void *buf, unsigned long size);

pid_t fork(void)
{
	static void *buf;
	long r;

	if (!buf && !(buf = malloc(FORK_STACK_BUF))) {
		errno = ENOMEM;
		return -1;
	}
	r = __wasm_host_fork(buf, FORK_STACK_BUF);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
''')

# ---------------------------------------------------------------- Makefile
# Windows hosts: the object list is too long for one command line, so
# hand it to llvm-ar through a response file.
mk = os.path.join(MUSL, 'Makefile')
s_ = open(mk, newline='').read()
s_ = s_.replace('\t$(AR) rc $@ $(AOBJS)\n',
                '\t$(file >$@.rsp,$(AOBJS))\n\t$(AR) rc $@ @$@.rsp\n')
# The setjmp runtime implements the lowering, so it must not be lowered.
SJLJ_RULE = ('\n# wasm32: the setjmp runtime must not itself be lowered\n'
             'obj/src/setjmp/wasm32/rt.o obj/src/setjmp/wasm32/rt.lo: '
             'CFLAGS_ALL := $(filter-out -mllvm -wasm-enable-sjlj,$(CFLAGS_ALL))\n')
if 'setjmp runtime must not' not in s_:
    s_ += SJLJ_RULE
open(mk, 'w', newline='').write(s_)

# ---------------------------------------------------------------- headers
patch('include/setjmp.h', '''#define setjmp setjmp
''', '''#define setjmp setjmp

#ifdef __wasm__
/* setjmp must be called directly in the caller for the wasm lowering */
#define _setjmp setjmp
#if defined(_POSIX_SOURCE) || defined(_POSIX_C_SOURCE) \\
 || defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) \\
 || defined(_BSD_SOURCE)
void __wasm_sigsetjmp_save(sigjmp_buf, int);
#define sigsetjmp(b, s) (__wasm_sigsetjmp_save((b), (s)), setjmp((b)))
#endif
#endif
''')

patch('include/unistd.h', '''pid_t vfork(void);
''', '''pid_t vfork(void);
#ifdef __wasm__
/* vfork returns twice; on wasm that needs setjmp in the caller's frame */
struct __jmp_buf_tag;
int setjmp(struct __jmp_buf_tag *) __attribute__((__returns_twice__));
struct __jmp_buf_tag *__wasm_vfork_slot(void);
pid_t __wasm_vfork(int);
#define vfork() __wasm_vfork(setjmp(__wasm_vfork_slot()))
#endif
''')

print('musl wasm32 port written')
