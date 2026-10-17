/* Context switch cost: round trips of one byte between two processes. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int a[2], b[2], n = argc > 1 && argc < 3 ? atoi(argv[1]) : 200;
	char c = 0;

	if (argc > 2) {	/* child: echo bytes back */
		int in = argv[1][0] - '0', out = argv[2][0] - '0';
		for (int fd = 3; fd < 10; fd++)
			if (fd != in && fd != out)
				close(fd);
		while (read(in, &c, 1) == 1 && write(out, &c, 1) == 1)
			;
		return 0;
	}
	pipe(a);
	pipe(b);
	char in[2] = { '0' + a[0], 0 }, out[2] = { '0' + b[1], 0 };
	char *args[] = { argv[0], in, out, NULL };
	pid_t p = vfork();
	if (p == 0) {
		execv(argv[0], args);
		_exit(1);
	}
	close(a[0]);
	close(b[1]);
	double t0 = now();
	for (int i = 0; i < n; i++) {
		if (getenv("PPDEBUG"))
			fprintf(stderr, "trip %d\n", i);
		if (write(a[1], &c, 1) != 1 || read(b[0], &c, 1) != 1)
			break;
	}
	double t = now() - t0;
	close(a[1]);
	waitpid(p, NULL, 0);
	printf("pingpong: %.1f us per round trip (%d)\n", t / n * 1e6, n);
	t0 = now();
	for (int i = 0; i < 20000; i++)
		getppid();
	printf("syscall: %.2f us\n", (now() - t0) / 20000 * 1e6);
	return 0;
}
