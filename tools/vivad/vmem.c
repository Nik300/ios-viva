/*
 * vmem: who holds the wired memory (kernel accounting by VM tag / allocation site, mach_memory_info: what zprint
 * shows on macOS). Root only (host_priv). Sites with flag WIRED, largest first; with an argument N, the top N.
 *   vmem [N]          e.g. before and after a test, then compare
 */
#include <mach/mach.h>
#include <mach_debug/mach_debug.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* osfmk/mach/vm_statistics.h (private in the macOS SDK) */
#define VM_KERN_SITE_TYPE   0x000000FF
#define VM_KERN_SITE_TAG    0x00000000
#define VM_KERN_SITE_KMOD   0x00000001
#define VM_KERN_SITE_KERNEL 0x00000002
#define VM_KERN_SITE_WIRED  0x00000100
#define VM_KERN_SITE_ZONE   0x00000800

extern kern_return_t mach_memory_info(host_priv_t, mach_zone_name_array_t *, mach_msg_type_number_t *,
    mach_zone_info_array_t *, mach_msg_type_number_t *, mach_memory_info_array_t *, mach_msg_type_number_t *);

static const char *tagname[] = {
    "none", "osfmk", "bsd", "iokit", "libkern", "oskext", "kext", "ipc", "stack", "cpu", "pmap", "pte", "zone",
    "kalloc", "compressor", "compressed_data", "phantom_cache", "waitq", "diag", "log", "file", "mbuf", "ubc",
    "security", "mlock", "reason", "skywalk", "ltable", "hv", "kalloc_data", "retired", "kalloc_type", "triage",
    "recount",
};

static int bysize(const void *a, const void *b)
{
    const mach_memory_info_t *x = a, *y = b;
    return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}

int main(int argc, char **argv)
{
    int top = argc > 1 ? atoi(argv[1]) : 25;
    host_priv_t priv;
    if (host_get_host_priv_port(mach_host_self(), &priv) != KERN_SUCCESS) {
        fprintf(stderr, "vmem: no host_priv (root?)\n");
        return 1;
    }
    mach_zone_name_t *names = NULL;
    mach_zone_info_t *zinfo = NULL;
    mach_memory_info_t *mi = NULL;
    mach_msg_type_number_t nn = 0, nz = 0, nm = 0;
    kern_return_t kr = mach_memory_info(priv, &names, &nn, &zinfo, &nz, &mi, &nm);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "vmem: mach_memory_info: 0x%x\n", kr);
        return 1;
    }
    uint64_t wired = 0, zones = 0;
    for (unsigned i = 0; i < nz; i++)
        zones += zinfo[i].mzi_cur_size;
    mach_memory_info_t *w = calloc(nm, sizeof(*w));
    unsigned nw = 0;
    for (unsigned i = 0; i < nm; i++) {
        if (!(mi[i].flags & VM_KERN_SITE_WIRED) || (mi[i].flags & VM_KERN_SITE_ZONE))
            continue;
        w[nw++] = mi[i];
        wired += mi[i].size;
    }
    qsort(w, nw, sizeof(*w), bysize);
    printf("wired sites %llu MiB (%u sites), zones %llu MiB\n", (unsigned long long)(wired >> 20), nw,
           (unsigned long long)(zones >> 20));
    for (unsigned i = 0; i < nw && (int)i < top; i++) {
        unsigned type = (unsigned)(w[i].flags & VM_KERN_SITE_TYPE);
        const char *kind = type == VM_KERN_SITE_TAG ? "tag" : type == VM_KERN_SITE_KMOD ? "kmod" :
                           type == VM_KERN_SITE_KERNEL ? "kernel" : "counter";
        char label[96];
        if (w[i].name[0])
            snprintf(label, sizeof(label), "%s", w[i].name);
        else if (type == VM_KERN_SITE_TAG && w[i].site < sizeof(tagname) / sizeof(tagname[0]))
            snprintf(label, sizeof(label), "%s", tagname[w[i].site]);
        else
            snprintf(label, sizeof(label), "site 0x%llx tag %u", (unsigned long long)w[i].site, w[i].tag);
        printf("%8llu KiB  %-7s %s\n", (unsigned long long)(w[i].size >> 10), kind, label);
    }
    return 0;
}
