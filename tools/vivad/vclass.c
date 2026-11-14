/*
 * vclass: live IOKit object counts per class and IOKit allocation totals (IORegistry root "IOKitDiagnostics", what
 * ioclasscount shows on macOS). Diff two runs around a test to find a leaking class.
 *   vclass            every class with a non-zero count, plus the allocation totals
 *   vclass NAME..     only those classes
 * CoreFoundation and IOKit through dlsym (built without the iOS SDK, see build.sh).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef const void *CFTypeRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __CFString *CFStringRef;
typedef unsigned int mach_port_t;

static struct {
    mach_port_t (*IORegistryGetRootEntry)(mach_port_t);
    CFTypeRef (*IORegistryEntryCreateCFProperty)(mach_port_t, CFStringRef, const void *, uint32_t);
    CFStringRef (*CFStringCreateWithCString)(const void *, const char *, uint32_t);
    long (*CFDictionaryGetCount)(CFDictionaryRef);
    void (*CFDictionaryGetKeysAndValues)(CFDictionaryRef, const void **, const void **);
    const void *(*CFDictionaryGetValue)(CFDictionaryRef, const void *);
    unsigned char (*CFStringGetCString)(CFStringRef, char *, long, uint32_t);
    unsigned char (*CFNumberGetValue)(CFTypeRef, long, void *);
    unsigned long (*CFGetTypeID)(CFTypeRef);
    unsigned long (*CFNumberGetTypeID)(void);
    unsigned long (*CFDictionaryGetTypeID)(void);
} f;

#define UTF8 0x08000100
#define kCFNumberSInt64Type 4

static CFStringRef str(const char *s) { return f.CFStringCreateWithCString(NULL, s, UTF8); }

static long long num(CFTypeRef v)
{
    long long n = -1;
    if (v && f.CFGetTypeID(v) == f.CFNumberGetTypeID())
        f.CFNumberGetValue(v, kCFNumberSInt64Type, &n);
    return n;
}

int main(int argc, char **argv)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    void *iok = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!cf || !iok)
        return 1;
#define S(l, n) (*(void **)&f.n = dlsym(l, #n))
    S(iok, IORegistryGetRootEntry); S(iok, IORegistryEntryCreateCFProperty); S(cf, CFStringCreateWithCString);
    S(cf, CFDictionaryGetCount); S(cf, CFDictionaryGetKeysAndValues); S(cf, CFDictionaryGetValue);
    S(cf, CFStringGetCString); S(cf, CFNumberGetValue); S(cf, CFGetTypeID); S(cf, CFNumberGetTypeID);
    S(cf, CFDictionaryGetTypeID);
    mach_port_t *mainPort = dlsym(iok, "kIOMainPortDefault");
    mach_port_t root = f.IORegistryGetRootEntry(mainPort ? *mainPort : 0);
    CFDictionaryRef diag = f.IORegistryEntryCreateCFProperty(root, str("IOKitDiagnostics"), NULL, 0);
    if (!diag || f.CFGetTypeID(diag) != f.CFDictionaryGetTypeID()) {
        fprintf(stderr, "vclass: no IOKitDiagnostics\n");
        return 1;
    }
    const char *totals[] = { "Instance allocation", "Container allocation", "IOMalloc allocation",
                             "Pageable allocation", NULL };
    for (int i = 0; totals[i] && argc < 2; i++)
        printf("%-24s %lld KiB\n", totals[i], num(f.CFDictionaryGetValue(diag, str(totals[i]))) >> 10);
    CFDictionaryRef classes = f.CFDictionaryGetValue(diag, str("Classes"));
    if (!classes)
        return 1;
    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            printf("%-40s %lld\n", argv[i], num(f.CFDictionaryGetValue(classes, str(argv[i]))));
        return 0;
    }
    long n = f.CFDictionaryGetCount(classes);
    const void *keys[n], *vals[n];
    f.CFDictionaryGetKeysAndValues(classes, keys, vals);
    for (long i = 0; i < n; i++) {
        long long c = num(vals[i]);
        char name[128];
        if (c > 0 && f.CFStringGetCString(keys[i], name, sizeof(name), UTF8))
            printf("%-40s %lld\n", name, c);
    }
    return 0;
}
