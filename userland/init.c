/*
 * WebLinux /sbin/init.
 *
 * Mounts the virtual file systems, runs /etc/rc once, then keeps a login
 * bash on each console: ttyW0 (the host's debug console) and tty1 (the
 * frame buffer console, if there is one).  Each shell gets its own session
 * with its tty as the controlling terminal, so job control works.  Reaps
 * orphans and respawns shells when they exit.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct console {
	const char *dev;
	char *term;
	pid_t shell;
};

static struct console consoles[] = {
	{ "/dev/ttyW0", "TERM=dumb", -1 },
	{ "/dev/tty1", "TERM=linux", -1 },
};

static char *shell_env[] = {
	"TERM=dumb", "HOME=/root", "SHELL=/bin/bash", "USER=root", "LOGNAME=root",
	"PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", NULL
};

static void mnt(const char *src, const char *dir, const char *type,
		unsigned long flags, const char *data)
{
	mkdir(dir, 0755);
	if (mount(src, dir, type, flags, data) < 0 && errno != EBUSY)
		fprintf(stderr, "init: mount %s on %s: %s\n", type, dir, strerror(errno));
}

/* Child side of the vfork: only syscalls, then exec. */
static pid_t spawn(char *const argv[], const struct console *con)
{
	shell_env[0] = con->term;
	pid_t pid = vfork();
	if (pid == 0) {
		setsid();
		int fd = open(con->dev, O_RDWR);
		if (fd < 0)
			_exit(126);
		ioctl(fd, TIOCSCTTY, 1);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		execve(argv[0], argv, shell_env);
		_exit(127);
	}
	return pid;
}

int main(void)
{
	static char *const rc[] = { "/bin/bash", "/etc/rc", NULL };
	static char *const sh[] = { "/bin/bash", "--login", NULL };
	unsigned int i, n = 0;
	int st;

	if (getpid() != 1)
		return 1;
	mnt("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mnt("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mnt("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mnt("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620,ptmxmode=666");
	mnt("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
	mnt("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
	mnt("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");

	if (access(rc[1], R_OK) == 0) {
		pid_t p = spawn(rc, &consoles[0]);
		while (p > 0 && waitpid(p, &st, 0) < 0 && errno == EINTR)
			;
	}

	/* consoles that exist */
	for (i = 0; i < sizeof(consoles) / sizeof(consoles[0]); i++)
		if (access(consoles[i].dev, R_OK | W_OK) == 0)
			consoles[n++] = consoles[i];

	for (;;) {
		for (i = 0; i < n; i++)
			if (consoles[i].shell < 0)
				consoles[i].shell = spawn(sh, &consoles[i]);
		pid_t p = wait(&st);
		if (p < 0) {
			if (errno == ECHILD)
				sleep(1);
			continue;
		}
		for (i = 0; i < n; i++) {
			if (p == consoles[i].shell) {
				consoles[i].shell = -1;
				/* don't spin if the shell can't start */
				if (WIFEXITED(st) && WEXITSTATUS(st) >= 126)
					sleep(5);
			}
		}
	}
}
