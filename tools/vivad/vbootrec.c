/*
 * vbootrec: raw access to the VIVABOOT record (fork/shim/vivaboot.h, patch 0180) = the boot-args of the next boot in
 * the last MiB of userdata, through the root partition's raw device /dev/rufsroot0 (writable with ufsrw=1). Built
 * next to vivad (tools/vivad/build.sh), pushed to /private/var/root/viva-tests/vbootrec, driven by
 * `tools/viva-link.py bootrec` (edit in vim/nano on the Mac):
 *   vbootrec text              the record as editable key=value text (stdout)
 *   vbootrec raw FILE          the whole 4 KiB block into FILE
 *   vbootrec put-text FILE     write the record from key=value text: line_hash recomputed, seq + 1,
 *                              base_hash=payload = hash of kern.viva_nextbootargs_orig (the boot partition's line)
 *   vbootrec put-raw FILE      write FILE (4096 bytes) as the block, unchanged
 * Writes refuse a block that the HFS+ volume reaches, are read back and compared.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define DEV             "/dev/rufsroot0"
#define BLOCK           4096
#define TAIL_BLOCKS     256
#define LINE            608

struct rec {
	char magic[8];
	uint32_t version, flags;
	uint64_t base_hash, seq, line_hash;
	uint8_t reserved[24];
	char cmdline[LINE];
};
_Static_assert(sizeof(struct rec) == 672, "VIVABOOT layout (shim/vivaboot.h)");

static uint64_t fnv(const char *s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < LINE && s[i]; i++) {
		h ^= (uint8_t)s[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int fail(const char *what)
{
	printf("vbootrec: %s: %s\n", what, strerror(errno));
	return 1;
}

/* Opens the device (VBOOTREC_DEV: another device or an image file, for tests); *off = the record's byte offset. */
static int open_dev(int flags, off_t *off)
{
	const char *path = getenv("VBOOTREC_DEV") ? getenv("VBOOTREC_DEV") : DEV;
	int fd = open(path, flags);
	if (fd < 0)
		return -1;
	uint32_t bsize = 0;
	uint64_t count = 0;
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
		bsize = 512;
		count = (uint64_t)st.st_size / 512;
	} else if (ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) || ioctl(fd, DKIOCGETBLOCKCOUNT, &count) || !bsize) {
		close(fd);
		return -1;
	}
	uint64_t bytes = count * bsize;
	if (bytes < (uint64_t)(TAIL_BLOCKS + 1) * BLOCK) {
		close(fd);
		errno = ENOSPC;
		return -1;
	}
	*off = (off_t)(bytes - (uint64_t)TAIL_BLOCKS * BLOCK);
	return fd;
}

static int read_block(uint8_t *blk, off_t *off)
{
	int fd = open_dev(O_RDONLY, off);
	if (fd < 0)
		return -1;
	ssize_t n = pread(fd, blk, BLOCK, *off);
	close(fd);
	return n == BLOCK ? 0 : -1;
}

/* The HFS+ volume must end before the record (its header: block size BE32 at 1064, total blocks BE32 at 1068). */
static int volume_clear(int fd, off_t off)
{
	uint8_t h[BLOCK];
	if (pread(fd, h, BLOCK, 0) != BLOCK)
		return -1;
	const uint8_t *v = h + 1024;
	uint16_t sig = (uint16_t)(v[0] << 8 | v[1]);
	uint64_t bs = (uint64_t)v[40] << 24 | v[41] << 16 | v[42] << 8 | v[43];
	uint64_t tot = (uint64_t)v[44] << 24 | v[45] << 16 | v[46] << 8 | v[47];
	if ((sig != 0x482b && sig != 0x4858) || !bs || bs * tot > (uint64_t)off) {
		printf("vbootrec: the volume (0x%x, %llu bytes) reaches the record at 0x%llx\n", sig,
		    (unsigned long long)(bs * tot), (unsigned long long)off);
		errno = ENOSPC;
		return -1;
	}
	return 0;
}

static int write_block(const uint8_t *blk)
{
	off_t off;
	int fd = open_dev(O_RDWR, &off);
	if (fd < 0)
		return fail(DEV);
	if (volume_clear(fd, off)) {
		close(fd);
		return 1;
	}
	uint8_t back[BLOCK];
	if (pwrite(fd, blk, BLOCK, off) != BLOCK) {
		close(fd);
		return fail("write");
	}
	ioctl(fd, DKIOCSYNCHRONIZE, NULL);
	ssize_t n = pread(fd, back, BLOCK, off);
	close(fd);
	if (n != BLOCK || memcmp(back, blk, BLOCK)) {
		printf("vbootrec: read back differs\n");
		return 1;
	}
	printf("vbootrec: written at byte 0x%llx of %s\n", (unsigned long long)off, getenv("VBOOTREC_DEV") ? getenv("VBOOTREC_DEV") : DEV);
	return 0;
}

static int cmd_text(void)
{
	uint8_t blk[BLOCK];
	off_t off;
	if (read_block(blk, &off))
		return fail(DEV);
	struct rec r;
	memcpy(&r, blk, sizeof(r));
	char payload[LINE + 1] = "", src[128] = "";
	size_t len = LINE;
	if (sysctlbyname("kern.viva_nextbootargs_orig", payload, &len, NULL, 0))
		payload[0] = 0;
	len = sizeof(src) - 1;
	if (sysctlbyname("kern.viva_nextbootargs_src", src, &len, NULL, 0))
		strcpy(src, "?");
	int magic = !memcmp(r.magic, "VIVABOOT", 8);
	char line[LINE + 1];
	memcpy(line, r.cmdline, LINE);
	line[LINE] = 0;
	printf("# VIVABOOT record: " DEV " byte 0x%llx (block 0x%llx of userdata), %s\n", (unsigned long long)off,
	    (unsigned long long)(off / BLOCK), magic ? "present" : "empty (no magic)");
	printf("# kern.viva_nextbootargs_src: %s\n", src);
	printf("# boot partition line (kern.viva_nextbootargs_orig), hash %016llx:\n#   %s\n",
	    (unsigned long long)fnv(payload), payload);
	printf("# line_hash on disk %016llx, of the line below %016llx%s\n", (unsigned long long)r.line_hash,
	    (unsigned long long)fnv(line), r.line_hash == fnv(line) ? "" : " (MISMATCH)");
	printf("# Save to write. line_hash is recomputed, seq goes up by one. base_hash=payload = the line above;\n");
	printf("# flags=0 (or an empty cmdline) disables the record. Lines starting with # are ignored.\n");
	printf("magic=%.8s\n", magic ? r.magic : "VIVABOOT");
	printf("version=%u\n", magic ? r.version : 1);
	printf("flags=%u\n", magic ? r.flags : 0);
	printf("seq=%llu\n", (unsigned long long)(magic ? r.seq : 0));
	if (magic && r.base_hash != fnv(payload))
		printf("base_hash=%016llx\n", (unsigned long long)r.base_hash);
	else
		printf("base_hash=payload\n");
	printf("cmdline=%s\n", magic ? line : payload);
	return 0;
}

static int cmd_raw(const char *path)
{
	uint8_t blk[BLOCK];
	off_t off;
	if (read_block(blk, &off))
		return fail(DEV);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(blk, 1, BLOCK, f) != BLOCK)
		return fail(path);
	fclose(f);
	printf("vbootrec: block at byte 0x%llx -> %s\n", (unsigned long long)off, path);
	return 0;
}

static int cmd_put_raw(const char *path)
{
	uint8_t blk[BLOCK];
	FILE *f = fopen(path, "rb");
	if (!f)
		return fail(path);
	size_t n = fread(blk, 1, BLOCK, f);
	fclose(f);
	if (n != BLOCK) {
		printf("vbootrec: %s: %zu bytes, need %d\n", path, n, BLOCK);
		return 1;
	}
	return write_block(blk);
}

static int cmd_put_text(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return fail(path);
	uint8_t blk[BLOCK], old[BLOCK];
	off_t off;
	if (read_block(old, &off))
		return fail(DEV);
	struct rec r, prev;
	memcpy(&prev, old, sizeof(prev));
	memset(&r, 0, sizeof(r));
	memcpy(r.magic, "VIVABOOT", 8);
	r.version = 1;
	int have_base = 0, base_payload = 1, have_seq = 0;
	char buf[2048];
	while (fgets(buf, sizeof(buf), f)) {
		buf[strcspn(buf, "\r\n")] = 0;
		if (buf[0] == '#' || !buf[0])
			continue;
		char *eq = strchr(buf, '=');
		if (!eq)
			continue;
		*eq = 0;
		const char *k = buf, *v = eq + 1;
		if (!strcmp(k, "magic")) {
			memset(r.magic, 0, 8);
			memcpy(r.magic, v, strnlen(v, 8));
		} else if (!strcmp(k, "version")) {
			r.version = (uint32_t)strtoul(v, NULL, 0);
		} else if (!strcmp(k, "flags")) {
			r.flags = (uint32_t)strtoul(v, NULL, 0);
		} else if (!strcmp(k, "seq")) {
			r.seq = strtoull(v, NULL, 0);
			have_seq = 1;
		} else if (!strcmp(k, "base_hash")) {
			have_base = 1;
			base_payload = !strcmp(v, "payload");
			if (!base_payload)
				r.base_hash = strtoull(v, NULL, 16);
		} else if (!strcmp(k, "cmdline")) {
			size_t n = strlen(v);
			while (n && v[n - 1] == ' ')
				n--;
			if (n >= LINE) {
				printf("vbootrec: cmdline of %zu bytes (max %d)\n", n, LINE - 1);
				return 1;
			}
			for (size_t i = 0; i < n; i++)
				if (v[i] < 0x20 || v[i] > 0x7e) {
					printf("vbootrec: cmdline: byte 0x%02x at %zu is not printable ASCII\n", (uint8_t)v[i], i);
					return 1;
				}
			memcpy(r.cmdline, v, n);
		}
	}
	fclose(f);
	if (!have_base || base_payload) {
		char payload[LINE + 1] = "";
		size_t len = LINE;
		if (sysctlbyname("kern.viva_nextbootargs_orig", payload, &len, NULL, 0))
			return fail("kern.viva_nextbootargs_orig (base_hash=payload)");
		r.base_hash = fnv(payload);
	}
	if (!r.cmdline[0])
		r.flags &= ~1u;
	uint64_t prev_seq = !memcmp(prev.magic, "VIVABOOT", 8) ? prev.seq : 0;
	if (!have_seq || r.seq == prev_seq)
		r.seq = prev_seq + 1;
	r.line_hash = fnv(r.cmdline);
	memset(blk, 0, sizeof(blk));
	memcpy(blk, &r, sizeof(r));
	off_t o;
	int fd = open_dev(O_RDWR, &o);
	if (fd >= 0) {
		close(fd);
		return write_block(blk);
	}
	/* The raw device refused writing (mounted root): the kernel writes the record (kern.viva_nextbootargs).
	 * It sets base_hash and seq itself; a disabled record = the boot partition's own line. */
	printf("vbootrec: " DEV " not writable (%s): through kern.viva_nextbootargs\n", strerror(errno));
	char payload[LINE + 1] = "";
	size_t len = LINE;
	if (sysctlbyname("kern.viva_nextbootargs_orig", payload, &len, NULL, 0))
		return fail("kern.viva_nextbootargs_orig");
	const char *want = (r.flags & 1) && r.cmdline[0] ? r.cmdline : payload;
	if (sysctlbyname("kern.viva_nextbootargs", NULL, NULL, (void *)want, strlen(want) + 1))
		return fail("kern.viva_nextbootargs");
	printf("vbootrec: written through the kernel%s\n", want == payload ? " (record disabled)" : "");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "text"))
		return cmd_text();
	if (argc == 3 && !strcmp(argv[1], "raw"))
		return cmd_raw(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "put-raw"))
		return cmd_put_raw(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "put-text"))
		return cmd_put_text(argv[2]);
	printf("usage: vbootrec text | raw FILE | put-text FILE | put-raw FILE\n");
	return 2;
}
