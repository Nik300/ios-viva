/*
 * viva (Redmi Note 11 Pro 4G, MT6781) boot shim: LK boots this as a gzip arm64
 * Image at EL1, MMU off, x0 = DTB. It stops the MediaTek watchdog, checks that
 * LK's framebuffer is where the kernel's early console expects it, and hands
 * the SWEETXNU payload in the ramdisk to loader_boot(). None of sweet's
 * Qualcomm probes (UFS, GIC, IMEM restart reason, PS_HOLD) run here: those
 * addresses belong to other blocks on MediaTek.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

/* toprgu@10007000 (viva DTB); register layout as Linux mtk_wdt.c. */
#define TOPRGU_WDT_MODE		0x10007000UL
#define WDT_MODE_KEY		0x22000000U
#define WDT_MODE_EN		(1U << 0)

#define FDT_BEGIN_NODE		1
#define FDT_END_NODE		2
#define FDT_PROP		3
#define FDT_NOP			4

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static const char *stage;	/* set at runtime: no absolute pointers in data */

static void __attribute__((noreturn)) halt(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	/* slot & 3: 0 sync, 1 irq, 2 fiq, 3 serror (no string table: it would need relocations) */
	con_str("PANIC", "exception");
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X30", regs[29]);
	halt();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	halt();
}

static uint32_t be32p(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* fb_base from LK's /chosen/atag,videolfb (raw LE struct, maybe only 4-byte aligned). */
static uint64_t videolfb_base(uintptr_t dtb)
{
	const uint8_t *fdt = (const uint8_t *)dtb;
	if (be32p(fdt) != 0xd00dfeed)
		return 0;
	const uint8_t *p = fdt + be32p(fdt + 8);
	const char *strings = (const char *)fdt + be32p(fdt + 12);
	const uint8_t *limit = fdt + be32p(fdt + 4);

	while (p < limit) {
		uint32_t tok = be32p(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			while (*p)
				p++;
			p = (const uint8_t *)(((uintptr_t)p + 4) & ~3UL);
		} else if (tok == FDT_PROP) {
			uint32_t len = be32p(p);
			const uint8_t *val = p + 8;
			if (streq(strings + be32p(p + 4), "atag,videolfb") && len >= 8) {
				uint64_t v = 0;
				for (int i = 7; i >= 0; i--)
					v = v << 8 | val[i];
				return v;
			}
			p = (const uint8_t *)(((uintptr_t)val + len + 3) & ~3UL);
		} else if (tok != FDT_END_NODE && tok != FDT_NOP) {
			break;
		}
	}
	return 0;
}

void shim_main(uint64_t dtb)
{
	stage = "watchdog";
	wr32(TOPRGU_WDT_MODE, (rd32(TOPRGU_WDT_MODE) & ~WDT_MODE_EN & 0x00ffffffU) | WDT_MODE_KEY);

	stage = "loader";
	uint64_t fb = videolfb_base(dtb);
	if (fb)
		con_fb = (volatile uint32_t *)fb;
	con_clear(RED);
	con_str("SHIM", "viva: xnu loader");
	con_hex2("EL", "el=", rsr("CurrentEL") >> 2, "wdt=", rd32(TOPRGU_WDT_MODE));
	con_hex("DTB", dtb);
	con_hex("LK FB", fb);
	if (fb != FB_BASE) {
		con_str("STOP", "LK framebuffer moved; rebuild for it");
		halt();
	}
	con_skip();

	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	halt();
}
