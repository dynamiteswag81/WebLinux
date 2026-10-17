/* Nested vfork, as BusyBox's bb_daemonize_or_rexec() does on NOMMU:
 * the vfork child vforks again; the grandchild execs, the child exits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "grandchild")) {
		sleep(1);
		printf("grandchild %d: ppid %d, done\n", getpid(), getppid());
		return 0;
	}
	int st;
	pid_t c = vfork();
	if (c == 0) {
		pid_t g = vfork();
		if (g == 0) {
			char *a[] = { argv[0], "grandchild", NULL };
			execv(argv[0], a);
			_exit(127);
		}
		_exit(g > 0 ? 0 : 1);
	}
	waitpid(c, &st, 0);
	printf("parent %d: child %d exited %d\n", getpid(), c, WEXITSTATUS(st));
	sleep(2);
	printf("parent: still alive\n");
	return 0;
}
