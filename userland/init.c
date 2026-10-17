/*
 * WebLinux /sbin/init.
 *
 * Mounts the virtual file systems, runs /etc/rc once, then keeps a login
 * bash on the console: each shell gets its own session with ttyW0 as the
 * controlling terminal, so job control works.  Reaps orphans and respawns
 * the shell when it exits.
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

#define CONSOLE "/dev/ttyW0"

static char *const shell_env[] = {
	"HOME=/root", "TERM=dumb", "SHELL=/bin/bash", "USER=root", "LOGNAME=root",
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
static pid_t spawn(char *const argv[], int ctty)
{
	pid_t pid = vfork();
	if (pid == 0) {
		setsid();
		if (ctty) {
			int fd = open(CONSOLE, O_RDWR);
			if (fd < 0)
				fd = open("/dev/console", O_RDWR);
			if (fd >= 0) {
				ioctl(fd, TIOCSCTTY, 1);
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
				if (fd > 2)
					close(fd);
			}
		}
		execve(argv[0], argv, shell_env);
		_exit(127);
	}
	return pid;
}

int main(void)
{
	static char *const rc[] = { "/bin/bash", "/etc/rc", NULL };
	static char *const sh[] = { "/bin/bash", "--login", NULL };
	pid_t shell = -1;
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
		pid_t p = spawn(rc, 1);
		while (p > 0 && waitpid(p, &st, 0) < 0 && errno == EINTR)
			;
	}

	for (;;) {
		if (shell < 0) {
			shell = spawn(sh, 1);
			if (shell < 0) {
				sleep(1);
				continue;
			}
		}
		pid_t p = wait(&st);
		if (p == shell) {
			shell = -1;
			sleep(1);
		} else if (p < 0 && errno == ECHILD) {
			shell = -1;
		}
	}
}
