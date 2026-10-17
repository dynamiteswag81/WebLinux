#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int counter = 1;
static int spawn(const char *what) {
	pid_t p = fork();          /* child returns from this function */
	if (p == 0) counter = 100;
	return p;
}
int main(int argc, char **argv) {
	if (argc > 1) { printf("exec'd child says hi, pid %d\n", getpid()); return 5; }
	pid_t p = spawn("x");
	if (p == 0) {
		printf("child: counter=%d pid=%d\n", counter, getpid());
		char *a[] = { "/init", "again", 0 };
		if (fork() == 0) { execv("/init", a); _exit(1); }
		int s; wait(&s); printf("child: grandchild status %d\n", WEXITSTATUS(s));
		return 7;
	}
	int st; waitpid(p, &st, 0);
	printf("parent: child %d exited %d, counter still %d\n", p, WEXITSTATUS(st), counter);
	for (;;) pause();
}
