/* Job control probe: a forked child takes the terminal, as bash does. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	sigset_t s, old;
	int st;
	printf("parent pid %d pgrp %d sid %d tty pgrp %d\n", getpid(), getpgrp(),
	       getsid(0), tcgetpgrp(0));
	fflush(stdout);
	pid_t p = fork();
	if (p == 0) {
		int r1 = setpgid(0, 0);
		sigemptyset(&s);
		sigaddset(&s, SIGTTOU);
		sigprocmask(SIG_BLOCK, &s, &old);
		int r2 = tcsetpgrp(0, getpid());
		int e = errno;
		sigprocmask(SIG_SETMASK, &old, NULL);
		fprintf(stderr, "child %d: setpgid %d tcsetpgrp %d (%s) tty pgrp now %d\n",
			getpid(), r1, r2, r2 ? strerror(e) : "ok", tcgetpgrp(0));
		_exit(3);
	}
	waitpid(p, &st, WUNTRACED);
	printf("parent: child status %#x\n", st);
	sigemptyset(&s);
	sigaddset(&s, SIGTTOU);
	sigprocmask(SIG_BLOCK, &s, NULL);
	printf("parent: tcsetpgrp back %d\n", tcsetpgrp(0, getpgrp()));
	return 0;
}
