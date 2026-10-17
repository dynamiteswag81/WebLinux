__attribute__((import_module("host"), import_name("fork"))) int hfork(void);
__attribute__((import_module("linux"), import_name("syscall"))) long sc(long, long, long, long, long, long, long);
static char msg[] = "r=?\n";
__attribute__((noinline)) static int b(int n) { return hfork() + n; }
__attribute__((noinline)) static int a(int n) { return b(n) * 1; }
__attribute__((export_name("_start"))) void _start(long *p) { int r = a(0); msg[2] = '0' + (r % 10); sc(64, 1, (long)msg, 4, 0, 0, 0); sc(94, 0, 0, 0, 0, 0, 0); }
