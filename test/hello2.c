#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <time.h>

static volatile int got;
static void onalarm(int s) { got = s; }
static jmp_buf jb;

int main(int argc, char **argv)
{
	printf("hello from musl! argc=%d pid=%d\n", argc, getpid());
	printf("long double: %Lf, double: %.3f\n", 3.25L * 2, 1.0 / 3);
	char *p = malloc(100000);
	strcpy(p, "malloc works");
	puts(p);
	if (setjmp(jb) == 0) { puts("setjmp first"); longjmp(jb, 7); }
	else puts("longjmp came back");
	signal(SIGALRM, onalarm);
	alarm(1);
	pause();
	printf("signal handler ran: sig=%d\n", got);
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	printf("time: %lld\n", (long long)ts.tv_sec);
	pid_t pid = vfork();
	if (pid == 0) {
		char *args[] = { "/init2", "child", NULL };
		execv("/hello2", args);
		_exit(127);
	}
	printf("vfork parent: child pid=%d\n", pid);
	int st;
	waitpid(pid, &st, 0);
	printf("child exited, status %d\n", WEXITSTATUS(st));
	if (argc > 1) return 3;
	for (;;) pause();
}
