/* mmap a regular file privately, read-only (NOMMU: a copy). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *p = argc > 1 ? argv[1] : "/etc/passwd";
	struct stat st;
	int fd = open(p, O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) {
		perror(p);
		return 1;
	}
	char *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	printf("mmap ok: %lld bytes, starts \"%.12s\"\n", (long long)st.st_size, m);
	munmap(m, st.st_size);
	return 0;
}
