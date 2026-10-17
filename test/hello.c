/* Minimal libc-free user program: proves exec + syscalls work. */
__attribute__((import_module("linux"), import_name("syscall")))
long sc(long nr, long a, long b, long c, long d, long e, long f);

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sc(64, 1, (long)s, slen(s), 0, 0, 0); }

static char buf[64];
static const char *num(long v) {
	char *p = buf + sizeof(buf) - 1; *p = 0;
	do { *--p = '0' + v % 10; v /= 10; } while (v);
	return p;
}

__attribute__((export_name("_start")))
void _start(long *args)
{
	long argc = args[0];
	char **argv = (char **)(args + 1);
	puts1("Hello from user space! I am a WebAssembly program running on Linux.\n");
	puts1("argv[0] = "); puts1(argc > 0 ? argv[0] : "?"); puts1("\n");
	puts1("pid = "); puts1(num(sc(172, 0, 0, 0, 0, 0, 0))); puts1("\n");
	for (;;) {
		char c[128];
		long n;
		puts1("echo> ");
		n = sc(63, 0, (long)c, sizeof(c), 0, 0, 0);
		if (n <= 0) break;
		sc(64, 1, (long)c, n, 0, 0, 0);
	}
	sc(94, 0, 0, 0, 0, 0, 0);
}
