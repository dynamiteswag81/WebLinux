/*
 * WebLinux /sbin/init.
 *
 * If there is a disk (/dev/wda, kept in browser storage by the host), the
 * root becomes an overlay: the initramfs below, the disk's /upper above,
 * so every change persists.  A blank disk is formatted first (ext2).
 *
 * Then mounts the virtual file systems, runs /etc/rc once, then keeps a login
 * bash on ttyW0 (the host's debug console) and the desktop (wldesk) on tty1,
 * the frame buffer console; "weblinux.text" on the command line puts a
 * shell there instead.  Each shell gets its own session
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
	char *const *argv;
	pid_t shell;
};

static char *const login_shell[] = { "/bin/bash", "--login", NULL };
static char *const desktop[] = { "/usr/bin/wldesk", NULL };

static struct console consoles[] = {
	{ "/dev/ttyW0", "TERM=dumb", login_shell, -1 },
	{ "/dev/tty1", "TERM=linux", login_shell, -1 },
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

static int run(char *const argv[])
{
	int st;
	pid_t pid = vfork();
	if (pid == 0) {
		execve(argv[0], argv, shell_env);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* 1: has an ext2/3/4 superblock, 0: all zeros (never used), -1: other */
static int disk_state(const char *dev)
{
	unsigned char b[4096];
	int fd = open(dev, O_RDONLY), i;
	if (fd < 0)
		return -1;
	i = pread(fd, b, sizeof(b), 0);
	close(fd);
	if (i != sizeof(b))
		return -1;
	if (b[1080] == 0x53 && b[1081] == 0xef)
		return 1;
	for (i = 0; i < (int)sizeof(b); i++)
		if (b[i])
			return -1;
	return 0;
}

static int cmdline_has(const char *word)
{
	char buf[1024];
	int fd, n;

	mnt("proc", "/proc", "proc", 0, NULL);
	fd = open("/proc/cmdline", O_RDONLY);
	n = fd < 0 ? 0 : read(fd, buf, sizeof(buf) - 1);
	if (fd >= 0)
		close(fd);
	umount("/proc");
	buf[n > 0 ? n : 0] = 0;
	for (char *p = buf; (p = strstr(p, word)); p++)
		if ((p == buf || p[-1] == ' ') &&
		    (p[strlen(word)] == 0 || p[strlen(word)] == ' ' || p[strlen(word)] == '\n'))
			return 1;
	return 0;
}

/* Overlay the initramfs with the disk and make that the root. */
static void persistent_root(void)
{
	static char *const mkfs[] = { "/sbin/mke2fs", "-q", "-L", "weblinux", "/dev/wda", NULL };
	const char *dev = "/dev/wda";
	int st;

	if (cmdline_has("weblinux.volatile"))
		return;
	mnt("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	st = disk_state(dev);
	if (st == 0) {
		fprintf(stderr, "init: formatting the new disk\n");
		if (run(mkfs) != 0)
			st = -1;
		else
			st = 1;
	}
	if (st != 1) {
		if (access(dev, F_OK) == 0)
			fprintf(stderr, "init: %s has no ext2/3/4 file system; not using it\n", dev);
		goto out;
	}
	mkdir("/.disk", 0700);
	if (mount(dev, "/.disk", "ext4", MS_NOATIME, NULL) < 0) {
		fprintf(stderr, "init: mount %s: %s\n", dev, strerror(errno));
		goto out;
	}
	mkdir("/.disk/upper", 0755);
	mkdir("/.disk/work", 0755);
	mkdir("/.root", 0755);
	if (mount("overlay", "/.root", "overlay", 0,
		  "lowerdir=/,upperdir=/.disk/upper,workdir=/.disk/work") < 0) {
		fprintf(stderr, "init: overlay: %s\n", strerror(errno));
		umount("/.disk");
		goto out;
	}
	mkdir("/.root/mnt", 0755);
	mkdir("/.root/mnt/disk", 0755);
	mount("/.disk", "/.root/mnt/disk", NULL, MS_MOVE, NULL);
	umount2("/dev", MNT_DETACH);
	if (chdir("/.root") == 0 && mount(".", "/", NULL, MS_MOVE, NULL) == 0 &&
	    chroot(".") == 0 && chdir("/") == 0) {
		fprintf(stderr, "init: root is the initramfs + %s (persistent)\n", dev);
		return;
	}
	fprintf(stderr, "init: switching root: %s\n", strerror(errno));
	return;
out:
	umount2("/dev", MNT_DETACH);
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
	unsigned int i, n = 0;
	int st;

	if (getpid() != 1)
		return 1;
	persistent_root();
	if (access(desktop[0], X_OK) == 0 && !cmdline_has("weblinux.text"))
		consoles[1].argv = desktop;
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
				consoles[i].shell = spawn(consoles[i].argv, &consoles[i]);
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
