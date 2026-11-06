/*
 * viva: the VIVABOOT record (vivaboot.h) read from the root partition before XNU, so the boot-args Viva Manager saves
 * (kern.viva_nextbootargs) apply on the next boot although LK write-protects boot_b while iOS runs.
 *
 * READ-ONLY UFS access: there is no SCSI WRITE in this file. LK leaves the MediaTek UFS host (ufshci@11270000) enabled
 * with the link up; like the kernel's sweet_ufs.c, this points the transfer request list at its own memory (checking
 * the host took it before ringing a doorbell), issues polled READ(10)s on the root's LUN, and gives the host its list
 * back. MMU off: memory is Device, DMA needs no cache maintenance, every access is aligned.
 *
 * Anything unexpected (host state, no GPT, no partition, timeout, no valid record) = the payload's command line, as
 * before this existed.
 */
#include <stdint.h>
#include "console.h"
#include "vivaboot.h"

#define UFS_BASE	0x11270000UL	/* ufshci@11270000 (viva DTB, VIVA.md) */

#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)

#define BLOCK		4096u
#define BOUNCE		(8u * BLOCK)

struct utrd {
	uint32_t dw0, dw1, dw2, dw3;
	uint32_t ucdba, ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length, DWORDs */
	uint32_t prdt;		/* [31:16] PRDT offset (DWORDs), [15:0] entries */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t bounce[BOUNCE] __attribute__((aligned(BLOCK)));
static char line[VIVABOOT_LINE];
static uint8_t lun, tag;

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

static uint64_t ticks(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static uint64_t freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

static void zero(volatile uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		p[i] = 0;
}

static uint64_t le(const volatile uint8_t *p, int n)
{
	uint64_t v = 0;
	for (int i = n - 1; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* READ(10) of count 4 KiB blocks at lba into bounce. 0 = done. */
static int ufs_read(uint64_t lba, uint32_t count)
{
	if (lba > 0xffffffffULL || count == 0 || count * BLOCK > BOUNCE)
		return -1;
	uint32_t len = count * BLOCK;
	volatile struct utrd *u = &utrl[0];
	volatile struct ucd *c = &ucd;
	zero((volatile uint8_t *)u, sizeof(*u));
	zero((volatile uint8_t *)c, sizeof(*c));

	c->cmd[0] = 0x01;		/* COMMAND UPIU */
	c->cmd[1] = 0x40;		/* R: data in */
	c->cmd[2] = lun;
	c->cmd[3] = ++tag;
	c->cmd[12] = (uint8_t)(len >> 24);
	c->cmd[13] = (uint8_t)(len >> 16);
	c->cmd[14] = (uint8_t)(len >> 8);
	c->cmd[15] = (uint8_t)len;
	c->cmd[16] = 0x28;		/* READ(10) */
	c->cmd[18] = (uint8_t)(lba >> 24);
	c->cmd[19] = (uint8_t)(lba >> 16);
	c->cmd[20] = (uint8_t)(lba >> 8);
	c->cmd[21] = (uint8_t)lba;
	c->cmd[23] = (uint8_t)(count >> 8);
	c->cmd[24] = (uint8_t)count;

	uint64_t ucd_pa = (uintptr_t)&ucd, buf_pa = (uintptr_t)bounce;
	u->dw0 = (1u << 28) | (2u << 25) | (1u << 24);	/* UFS storage, device to memory, interrupt */
	u->dw2 = 0x0f;					/* OCS invalid until updated */
	u->ucdba = (uint32_t)ucd_pa;
	u->ucdbau = (uint32_t)(ucd_pa >> 32);
	u->resp = (512u / 4) << 16 | (512u / 4);
	u->prdt = (1024u / 4) << 16 | 1;
	c->prdt[0].addr_lo = (uint32_t)buf_pa;
	c->prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
	c->prdt[0].dbc = len - 1;
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t deadline = ticks() + freq();		/* 1 s */
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() > deadline) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return -2;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	if ((u->dw2 & 0xff) || c->resp[0] != 0x21 || c->resp[6] || c->resp[7])
		return -3;
	return 0;
}

/* Value of key=... in a command line (first match, up to a space), 0 when absent. */
static int arg(const char *cmd, const char *key, char *out, int cap)
{
	for (int i = 0; i < VIVABOOT_LINE && cmd[i]; i++) {
		if (i && cmd[i - 1] != ' ')
			continue;
		int k = 0;
		while (key[k] && cmd[i + k] == key[k])
			k++;
		if (key[k] || cmd[i + k] != '=')
			continue;
		int n = 0;
		for (int j = i + k + 1; j < VIVABOOT_LINE && cmd[j] && cmd[j] != ' ' && n < cap - 1; j++)
			out[n++] = cmd[j];
		out[n] = 0;
		return 1;
	}
	return 0;
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* GPT partition `name` on the current LUN: first block and count. 0 = found. */
static int find_part(const char *name, uint64_t *first, uint64_t *count)
{
	if (ufs_read(1, 1))
		return -1;
	static const char efi[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
	for (int i = 0; i < 8; i++)
		if (bounce[i] != (uint8_t)efi[i])
			return -2;
	uint64_t entries = le(bounce + 72, 8);
	uint32_t nent = (uint32_t)le(bounce + 80, 4), esz = (uint32_t)le(bounce + 84, 4);
	if (esz != 128 || nent == 0 || nent > 1024)
		return -3;
	uint32_t per = BOUNCE / esz;
	for (uint32_t base = 0; base < nent; base += per) {
		uint32_t n = nent - base < per ? nent - base : per;
		if (ufs_read(entries + base * esz / BLOCK, (n * esz + BLOCK - 1) / BLOCK))
			return -4;
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *e = bounce + i * esz;
			char nm[37];
			int c;
			for (c = 0; c < 36; c++) {
				uint32_t ch = e[56 + 2 * c] | e[57 + 2 * c] << 8;
				if (ch == 0 || ch > 0x7f)
					break;
				nm[c] = (char)ch;
			}
			nm[c] = 0;
			if (!same(nm, name))
				continue;
			uint64_t f = le(e + 32, 8), l = le(e + 40, 8);
			if (l < f + VIVABOOT_TAIL_BLOCKS)
				return -5;
			*first = f;
			*count = l - f + 1;
			return 0;
		}
	}
	return -6;
}

const char *viva_bootfile(const char *payload)
{
	char v[16];
	if (arg(payload, "vivabootfile", v, sizeof(v)) && v[0] == '0') {
		con_str("BOOTFILE", "off (vivabootfile=0)");
		return payload;
	}
	lun = 2;
	if (arg(payload, "ufslun", v, sizeof(v)) && v[0] >= '0' && v[0] <= '7' && !v[1])
		lun = (uint8_t)(v[0] - '0');
	char root[37];
	if (!arg(payload, "ufsroot", root, sizeof(root))) {
		const char *d = "userdata";
		int i = 0;
		for (; d[i]; i++)
			root[i] = d[i];
		root[i] = 0;
	}

	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY) || ufs_rd(REG_UTRLDBR)) {
		con_hex2("BOOTFILE", "hce=", hce, "hcs=", hcs);
		return payload;
	}
	/* Take the request list; give LK's back afterwards (the kernel takes it again). */
	uint32_t lba_lo = ufs_rd(REG_UTRLBA), lba_hi = ufs_rd(REG_UTRLBAU), rsr = ufs_rd(REG_UTRLRSR);
	uint64_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLRSR, 0);
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	const char *out = payload;
	int err = -10;
	if (ufs_rd(REG_UTRLBA) == (uint32_t)list && ufs_rd(REG_UTRLBAU) == (uint32_t)(list >> 32) &&
	    (ufs_rd(REG_UTRLRSR) & 1)) {
		uint64_t first = 0, count = 0;
		err = find_part(root, &first, &count);
		if (!err)
			err = ufs_read(first + count - VIVABOOT_TAIL_BLOCKS, 1) ? -20 : 0;
		if (!err) {
			const struct vivaboot_rec *rec = (const struct vivaboot_rec *)bounce;
			if (vivaboot_valid(rec, vivaboot_hash(payload, VIVABOOT_LINE))) {
				for (int i = 0; i < VIVABOOT_LINE; i++)
					line[i] = rec->cmdline[i];
				out = line;
			} else {
				err = 1;	/* no record, disabled, or for another payload */
			}
		}
	}
	ufs_wr(REG_UTRLRSR, 0);
	ufs_wr(REG_UTRLBA, lba_lo);
	ufs_wr(REG_UTRLBAU, lba_hi);
	ufs_wr(REG_UTRLRSR, rsr);
	if (out == line)
		con_str("BOOTFILE", "boot-args from userdata (VIVABOOT)");
	else if (err == 1)
		con_str("BOOTFILE", "none: boot-args from the payload");
	else
		con_hex2("BOOTFILE", "err=", (uint32_t)err, "lun=", lun);
	return out;
}
