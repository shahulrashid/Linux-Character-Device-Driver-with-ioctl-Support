// SPDX-License-Identifier: GPL-2.0
/*
 * test_app.c - user-space client / automated test suite for /dev/mychardev
 *
 *   ./test_app            run the short demo (open, write, read, clear, close)
 *   ./test_app --test     run the automated test suite (exit code 0 = all pass)
 *   ./test_app -d PATH    use a different device node
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mychardev.h"

#define DEFAULT_DEV "/dev/" MYCHARDEV_NAME

static const char *g_dev = DEFAULT_DEV;
static int g_pass, g_fail;

#define CHECK(cond, name)                                              \
	do {                                                           \
		int ok_ = (cond);                                      \
		int e_ = errno;                                        \
		if (ok_) {                                             \
			g_pass++;                                      \
			printf("  [PASS] %s\n", name);                  \
		} else {                                               \
			g_fail++;                                      \
			printf("  [FAIL] %s (errno=%d: %s)\n", name,   \
			       e_, strerror(e_));                      \
		}                                                      \
	} while (0)

/* setup-only write whose result we deliberately ignore */
static void put(int fd, const char *s, size_t n)
{
	if (write(fd, s, n) < 0)
		perror("setup write");
}

/* volatile NULL so the compiler does not warn about our deliberate bad pointer */
static char *volatile g_badptr = NULL;

static int open_dev(int flags)
{
	int fd = open(g_dev, flags);

	if (fd < 0)
		fprintf(stderr, "open(%s): %s\n", g_dev, strerror(errno));
	return fd;
}

/* ---------------------------------------------------------------- */
/* demo                                                             */
/* ---------------------------------------------------------------- */

static int run_demo(void)
{
	const char *msg = "Hello Linux";
	char rbuf[128] = {0};
	ssize_t n;
	int fd = open_dev(O_RDWR);

	if (fd < 0)
		return 1;
	printf("Device opened successfully\n");
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);         /* start from a clean state */

	printf("Writing: %s\n", msg);
	n = write(fd, msg, strlen(msg) + 1);       /* include the NUL byte */
	if (n < 0) {
		perror("write");
		close(fd);
		return 1;
	}
	printf("Bytes written: %zd\n", n);

	lseek(fd, 0, SEEK_SET);                    /* rewind before reading */
	n = read(fd, rbuf, sizeof(rbuf) - 1);
	if (n < 0) {
		perror("read");
		close(fd);
		return 1;
	}
	printf("Reading from device: %s\n", rbuf);

	printf("Sending command: CLEAR_BUFFER\n");
	if (ioctl(fd, MYCHARDEV_CLEAR_BUFFER) < 0) {
		perror("ioctl");
		close(fd);
		return 1;
	}
	printf("Buffer cleared successfully\n");

	printf("Closing device\n");
	close(fd);
	return 0;
}

/* ---------------------------------------------------------------- */
/* automated tests                                                  */
/* ---------------------------------------------------------------- */

static unsigned int get_len(int fd)
{
	unsigned int v = (unsigned int)-1;

	ioctl(fd, MYCHARDEV_GET_LENGTH, &v);
	return v;
}

static void test_basic_roundtrip(void)
{
	const char *msg = "Hello Linux";
	char rbuf[64] = {0};
	int fd = open_dev(O_RDWR);

	printf("\nTest: basic write/read round trip\n");
	CHECK(fd >= 0, "open() succeeds");
	if (fd < 0)
		return;
	CHECK(ioctl(fd, MYCHARDEV_CLEAR_BUFFER) == 0, "ioctl CLEAR_BUFFER");
	CHECK(write(fd, msg, strlen(msg) + 1) == (ssize_t)strlen(msg) + 1,
	      "write() returns number of bytes written (12)");
	CHECK(get_len(fd) == strlen(msg) + 1, "GET_LENGTH == 12");
	CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek(0) rewinds");
	CHECK(read(fd, rbuf, sizeof(rbuf)) == (ssize_t)strlen(msg) + 1,
	      "read() returns 12 bytes");
	CHECK(strcmp(rbuf, msg) == 0, "data read back matches data written");
	CHECK(read(fd, rbuf, sizeof(rbuf)) == 0, "read() at end of data returns 0");
	close(fd);
}

static void test_empty_and_clear(void)
{
	char rbuf[16];
	int fd = open_dev(O_RDWR);

	printf("\nTest: empty buffer and CLEAR_BUFFER\n");
	if (fd < 0) { CHECK(0, "open()"); return; }
	put(fd, "abc", 3);
	CHECK(ioctl(fd, MYCHARDEV_CLEAR_BUFFER) == 0, "CLEAR_BUFFER succeeds");
	CHECK(get_len(fd) == 0, "GET_LENGTH == 0 after clear");
	lseek(fd, 0, SEEK_SET);
	CHECK(read(fd, rbuf, sizeof(rbuf)) == 0, "read() on empty buffer returns 0");
	CHECK(write(fd, "", 0) == 0, "zero-length write returns 0");
	close(fd);
}

static void test_partial_read(void)
{
	char rbuf[8] = {0};
	int fd = open_dev(O_RDWR);

	printf("\nTest: partial reads\n");
	if (fd < 0) { CHECK(0, "open()"); return; }
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	put(fd, "0123456789", 10);
	lseek(fd, 0, SEEK_SET);
	CHECK(read(fd, rbuf, 4) == 4 && memcmp(rbuf, "0123", 4) == 0, "first 4 bytes");
	CHECK(read(fd, rbuf, 4) == 4 && memcmp(rbuf, "4567", 4) == 0, "next 4 bytes");
	CHECK(read(fd, rbuf, 4) == 2 && memcmp(rbuf, "89", 2) == 0, "last 2 bytes (short read)");
	CHECK(read(fd, rbuf, 4) == 0, "then EOF");
	close(fd);
}

static void test_capacity_limits(void)
{
	unsigned int cap = 0;
	char *big;
	int fd = open_dev(O_RDWR);

	printf("\nTest: buffer limits and partial writes\n");
	if (fd < 0) { CHECK(0, "open()"); return; }
	CHECK(ioctl(fd, MYCHARDEV_GET_CAPACITY, &cap) == 0 && cap > 0,
	      "GET_CAPACITY returns a positive size");
	big = malloc(cap + 100);
	if (!big) { CHECK(0, "malloc"); close(fd); return; }
	memset(big, 'A', cap + 100);

	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	CHECK(write(fd, big, cap + 100) == (ssize_t)cap,
	      "oversized write is truncated to capacity (partial write)");
	CHECK(get_len(fd) == cap, "GET_LENGTH == capacity");
	errno = 0;
	CHECK(write(fd, big, 10) == -1 && errno == ENOSPC,
	      "write to a full buffer fails with ENOSPC");
	free(big);
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	close(fd);
}

static void test_seek_and_append(void)
{
	char rbuf[16] = {0};
	unsigned int cap = 0;
	int fd = open_dev(O_RDWR);

	printf("\nTest: lseek and O_APPEND\n");
	if (fd < 0) { CHECK(0, "open()"); return; }
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	ioctl(fd, MYCHARDEV_GET_CAPACITY, &cap);
	put(fd, "AB", 2);
	CHECK(lseek(fd, 0, SEEK_END) == 2, "lseek(SEEK_END) == data length");
	errno = 0;
	CHECK(lseek(fd, (off_t)cap + 1, SEEK_SET) == -1 && errno == EINVAL,
	      "lseek beyond capacity fails with EINVAL");
	errno = 0;
	CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL,
	      "negative lseek fails with EINVAL");
	close(fd);

	fd = open_dev(O_RDWR | O_APPEND);
	if (fd < 0) { CHECK(0, "open(O_APPEND)"); return; }
	put(fd, "CD", 2);
	lseek(fd, 0, SEEK_SET);
	CHECK(read(fd, rbuf, sizeof(rbuf)) == 4 && memcmp(rbuf, "ABCD", 4) == 0,
	      "O_APPEND write lands after existing data");
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	close(fd);
}

static void test_errors(void)
{
	int fd = open_dev(O_RDWR);

	printf("\nTest: error handling\n");
	if (fd < 0) { CHECK(0, "open()"); return; }
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);

	errno = 0;
	CHECK(ioctl(fd, _IO('x', 0)) == -1 && errno == ENOTTY,
	      "ioctl with wrong magic number -> ENOTTY");
	errno = 0;
	CHECK(ioctl(fd, _IO(MYCHARDEV_IOC_MAGIC, 99)) == -1 && errno == ENOTTY,
	      "ioctl with unknown command number -> ENOTTY");
	errno = 0;
	CHECK(ioctl(fd, MYCHARDEV_GET_LENGTH, NULL) == -1 && errno == EFAULT,
	      "ioctl with bad user pointer -> EFAULT");
	errno = 0;
	CHECK(write(fd, g_badptr, 10) == -1 && errno == EFAULT,
	      "write from NULL pointer -> EFAULT");
	put(fd, "abc", 3);
	lseek(fd, 0, SEEK_SET);
	errno = 0;
	CHECK(read(fd, g_badptr, 3) == -1 && errno == EFAULT,
	      "read into NULL pointer -> EFAULT");
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	close(fd);
}

/* --- concurrency: if the driver had no lock, a reader could observe a
 * block that is half 'A' and half 'B'. Every read must be uniform. ---- */

#define BLK     64
#define THREADS 4
#define ITERS   20000

struct worker { int fd; char c; int bad; };

static void *worker_fn(void *arg)
{
	struct worker *w = arg;
	char out[BLK], in[BLK];

	memset(out, w->c, BLK);
	for (int i = 0; i < ITERS; i++) {
		if (pwrite(w->fd, out, BLK, 0) != BLK) { w->bad++; continue; }
		if (pread(w->fd, in, BLK, 0) != BLK)   { w->bad++; continue; }
		for (int j = 1; j < BLK; j++)
			if (in[j] != in[0]) { w->bad++; break; }
	}
	return NULL;
}

static void test_concurrency(void)
{
	pthread_t th[THREADS];
	struct worker w[THREADS];
	int fd = open_dev(O_RDWR), bad = 0;

	printf("\nTest: concurrent access (%d threads x %d iterations)\n",
	       THREADS, ITERS);
	if (fd < 0) { CHECK(0, "open()"); return; }
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	for (int i = 0; i < THREADS; i++) {
		w[i] = (struct worker){ .fd = fd, .c = 'A' + i, .bad = 0 };
		pthread_create(&th[i], NULL, worker_fn, &w[i]);
	}
	for (int i = 0; i < THREADS; i++) {
		pthread_join(th[i], NULL);
		bad += w[i].bad;
	}
	errno = 0;
	CHECK(bad == 0, "no torn or failed reads/writes under contention");
	ioctl(fd, MYCHARDEV_CLEAR_BUFFER);
	close(fd);
}

static int run_tests(void)
{
	printf("Running tests against %s\n", g_dev);
	test_basic_roundtrip();
	test_empty_and_clear();
	test_partial_read();
	test_capacity_limits();
	test_seek_and_append();
	test_errors();
	test_concurrency();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	int do_tests = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--test")) {
			do_tests = 1;
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			g_dev = argv[++i];
		} else {
			fprintf(stderr, "usage: %s [--test] [-d device]\n", argv[0]);
			return 2;
		}
	}
	return do_tests ? run_tests() : run_demo();
}
