/* Host test of vivaboot.c's pure logic (record validation, hash, argument parsing): cc vivaboot_test.c -o out/vt */
#include <stdio.h>
#include <string.h>
#define VIVA
#include "vivaboot.c"

volatile uint32_t *con_fb;
void con_str(const char *l, const char *s) { (void)l; (void)s; }
void con_hex2(const char *l, const char *a, uint32_t va, const char *b, uint32_t vb) { (void)l; (void)a; (void)va; (void)b; (void)vb; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void make(struct vivaboot_rec *r, const char *base, const char *cmd)
{
	memset(r, 0, sizeof(*r));
	memcpy(r->magic, VIVABOOT_MAGIC, 8);
	r->version = VIVABOOT_VERSION;
	r->flags = 1;
	r->base_hash = vivaboot_hash(base, VIVABOOT_LINE);
	strcpy(r->cmdline, cmd);
	r->line_hash = vivaboot_hash(r->cmdline, VIVABOOT_LINE);
}

int main(void)
{
	static struct vivaboot_rec r;
	const char *base = "debug=0xc ufslun=2 rd=ufsroot0", *want = "debug=0xc ufslun=2 rd=ufsroot0 vivajpeg=1";
	make(&r, base, want);
	CHECK(vivaboot_valid(&r, vivaboot_hash(base, VIVABOOT_LINE)));
	CHECK(!vivaboot_valid(&r, vivaboot_hash("other", VIVABOOT_LINE)));      /* boot_b reflashed: ignored */
	r.flags = 0; CHECK(!vivaboot_valid(&r, r.base_hash)); r.flags = 1;     /* disabled */
	r.cmdline[3] ^= 1; CHECK(!vivaboot_valid(&r, r.base_hash)); r.cmdline[3] ^= 1;   /* corrupted */
	r.magic[0] = 'X'; CHECK(!vivaboot_valid(&r, r.base_hash)); r.magic[0] = 'V';
	r.cmdline[5] = '\n'; r.line_hash = vivaboot_hash(r.cmdline, VIVABOOT_LINE); CHECK(!vivaboot_valid(&r, r.base_hash));
	memset(r.cmdline, 'a', VIVABOOT_LINE); r.line_hash = vivaboot_hash(r.cmdline, VIVABOOT_LINE); CHECK(!vivaboot_valid(&r, r.base_hash));
	CHECK(vivaboot_hash("", 608) == 0xcbf29ce484222325ULL);
	CHECK(vivaboot_hash("a", 608) == 0xaf63dc4c8601ec8cULL);              /* FNV-1a 64 reference value */

	char v[37];
	CHECK(arg("a=1 ufslun=3 x", "ufslun", v, sizeof v) && !strcmp(v, "3"));
	CHECK(!arg("myufslun=3", "ufslun", v, sizeof v));
	CHECK(!arg("ufslunx=3", "ufslun", v, sizeof v));
	CHECK(arg("ufsroot=userdata", "ufsroot", v, sizeof v) && !strcmp(v, "userdata"));
	CHECK(arg("x vivabootfile=0", "vivabootfile", v, sizeof v) && v[0] == '0');
	CHECK(!arg("", "ufslun", v, sizeof v));
	printf("%s\n", fails ? "FAILED" : "vivaboot host test: all ok");
	return fails != 0;
}
