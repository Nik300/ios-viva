/*
 * VIVABOOT: the boot-args of the next boot kept in the root partition (`userdata`), outside the HFS+ volume, because
 * the MediaTek bootloader write-protects boot_a/boot_b while iOS runs. Written by the kernel (kern.viva_nextbootargs,
 * bsd/dev/sweet_ufs.c, patch 0180), read by the shim before XNU (vivaboot.c). Same layout on both sides.
 *
 * Place: the first 4 KiB block of the last MiB of the partition (block nblocks - VIVABOOT_TAIL_BLOCKS). Little endian.
 * The record applies only while the boot partition's payload still holds the command line it was written against
 * (base_hash): flashing boot_b with another command line drops it; `vivabootfile=0` in that command line ignores it.
 */
#pragma once
#include <stdint.h>

#define VIVABOOT_MAGIC          "VIVABOOT"
#define VIVABOOT_VERSION        1
#define VIVABOOT_TAIL_BLOCKS    256u            /* last 1 MiB of 4 KiB blocks */
#define VIVABOOT_LINE           608             /* = boot_args.CommandLine */

struct vivaboot_rec {
	char magic[8];
	uint32_t version;
	uint32_t flags;                         /* bit 0: enabled */
	uint64_t base_hash;                     /* FNV-1a 64 of the payload command line it replaces */
	uint64_t seq;                           /* incremented on every write */
	uint64_t line_hash;                     /* FNV-1a 64 of cmdline */
	uint8_t reserved[24];
	char cmdline[VIVABOOT_LINE];            /* NUL-terminated, printable ASCII */
};

_Static_assert(sizeof(struct vivaboot_rec) == 672, "VIVABOOT layout");

/* FNV-1a 64 of a NUL-terminated string of at most max bytes. */
static inline uint64_t vivaboot_hash(const char *s, unsigned max)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned i = 0; i < max && s[i]; i++) {
		h ^= (uint8_t)s[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* 1 when rec is an enabled record for a payload whose command line hashes to base. */
static inline int vivaboot_valid(const struct vivaboot_rec *rec, uint64_t base)
{
	static const char want[8] = { 'V', 'I', 'V', 'A', 'B', 'O', 'O', 'T' };
	for (int i = 0; i < 8; i++)
		if (rec->magic[i] != want[i])
			return 0;
	if (rec->version != VIVABOOT_VERSION || !(rec->flags & 1) || rec->base_hash != base)
		return 0;
	unsigned n = 0;
	while (n < VIVABOOT_LINE && rec->cmdline[n]) {
		if (rec->cmdline[n] < 0x20 || rec->cmdline[n] > 0x7e)
			return 0;
		n++;
	}
	if (n == VIVABOOT_LINE)
		return 0;
	return vivaboot_hash(rec->cmdline, VIVABOOT_LINE) == rec->line_hash;
}
