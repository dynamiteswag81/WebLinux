/*
 * play: play WAV files (PCM, 8 or 16 bit) or a test tone through /dev/dsp.
 *
 *   play file.wav ...
 *   play -t [HZ [SECONDS]]      sine tone (default 440 Hz, 1 s)
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

static int open_dsp(int rate, int channels, int bits)
{
	int fd = open("/dev/dsp", O_WRONLY);
	int fmt = bits == 8 ? AFMT_U8 : AFMT_S16_LE;

	if (fd < 0) {
		perror("play: /dev/dsp");
		return -1;
	}
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0 ||
	    ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0 ||
	    ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0) {
		perror("play: setting up /dev/dsp");
		close(fd);
		return -1;
	}
	return fd;
}

static int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += k;
		n -= k;
	}
	return 0;
}

static int tone(double hz, double secs)
{
	int rate = 44100, n = (int)(rate * secs);
	int16_t buf[4096];
	int fd = open_dsp(rate, 1, 16);

	if (fd < 0)
		return 1;
	for (int i = 0; i < n;) {
		int k = 0;
		for (; k < 4096 && i < n; k++, i++) {
			double env = i < 441 ? i / 441.0 : n - i < 441 ? (n - i) / 441.0 : 1;
			buf[k] = (int16_t)(sin(2 * M_PI * hz * i / rate) * 12000 * env);
		}
		if (write_all(fd, buf, k * 2))
			break;
	}
	close(fd);
	return 0;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }

static int play_wav(const char *path)
{
	unsigned char h[12], ck[8], fmt[16];
	int in = open(path, O_RDONLY), fd = -1, have_fmt = 0;
	char buf[16384];

	if (in < 0) {
		perror(path);
		return 1;
	}
	if (read(in, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
		fprintf(stderr, "%s: not a WAV file\n", path);
		close(in);
		return 1;
	}
	while (read(in, ck, 8) == 8) {
		uint32_t len = le32(ck + 4);
		if (!memcmp(ck, "fmt ", 4)) {
			if (len < 16 || read(in, fmt, 16) != 16)
				break;
			lseek(in, len - 16 + (len & 1), SEEK_CUR);
			if (le16(fmt) != 1 || (le16(fmt + 14) != 8 && le16(fmt + 14) != 16)) {
				fprintf(stderr, "%s: only 8/16-bit PCM is supported\n", path);
				break;
			}
			have_fmt = 1;
		} else if (!memcmp(ck, "data", 4) && have_fmt) {
			int rate = le32(fmt + 4), ch = le16(fmt + 2), bits = le16(fmt + 14);
			printf("%s: %d Hz, %d channel%s, %d bit, %.1f s\n", path, rate, ch,
			       ch > 1 ? "s" : "", bits, (double)len / (rate * ch * bits / 8));
			fd = open_dsp(rate, ch, bits);
			while (fd >= 0 && len) {
				ssize_t k = read(in, buf, len < sizeof(buf) ? len : sizeof(buf));
				if (k <= 0 || write_all(fd, buf, k))
					break;
				len -= k;
			}
			break;
		} else {
			lseek(in, len + (len & 1), SEEK_CUR);
		}
	}
	if (fd >= 0)
		close(fd);
	close(in);
	return fd >= 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	int ret = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: play FILE.wav...\n       play -t [HZ [SECONDS]]\n");
		return 2;
	}
	if (!strcmp(argv[1], "-t"))
		return tone(argc > 2 ? atof(argv[2]) : 440, argc > 3 ? atof(argv[3]) : 1);
	for (int i = 1; i < argc; i++)
		ret |= play_wav(argv[i]);
	return ret;
}
