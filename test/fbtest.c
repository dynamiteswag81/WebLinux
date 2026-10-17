/* mmap /dev/fb0 and draw a gradient box; print the mapping. */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		perror("fb0");
		return 1;
	}
	uint8_t *p = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	for (unsigned y = 100; y < 300; y++)
		for (unsigned x = 300; x < 700; x++)
			*(uint32_t *)(p + y * f.line_length + x * 4) =
				((x - 300) * 255 / 400) << 16 | ((y - 100) * 255 / 200) << 8 | 0x80;
	printf("fbtest: %ux%u %ubpp mapped at %p smem %#lx ok\n", v.xres, v.yres,
	       v.bits_per_pixel, p, f.smem_start);
	return 0;
}
