#include <setjmp.h>
#include <stdio.h>
__attribute__((import_module("host"), import_name("fork"))) int hfork(void);
static jmp_buf top;
static int depth(int n) { if (n == 0) return hfork(); return depth(n - 1) + 0; }
static int work(void) { int r = depth(3); printf("fork returned %d\n", r); return r; }
int main(void) {
	if (setjmp(top)) { puts("longjmp landed"); return 0; }
	int r = work();
	if (r == 0) longjmp(top, 1);
	return r;
}
