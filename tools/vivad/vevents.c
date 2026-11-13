/*
 * vevents: what the user does on the phone, as one line per event, for `viva-link.py watch`.
 *
 * Started by the host's link server through vivad (`run vevents`) whenever a host tool is attached; its stdout
 * comes back as 'O' frames and the server turns the lines starting with "EV " into watch events:
 *
 *   EV app BUNDLE-ID PID             frontmost application changed (com.apple.springboard = home / lock screen)
 *   EV touch down|up N X Y [MS DIST] finger N, X Y in 0..1 of the screen (portrait, origin top left);
 *                                    on up: how long it was down and how far it moved (0..1): a tap is short and still
 *   EV key down|up NAME              power, volup, voldown, other keys as PAGE:USAGE
 *   EV lock 0|1                      lock screen shown (1) / unlocked (0)
 *   EV display 0|1                   screen turned off / on
 *   EV screenshot PATH               a new image in the camera roll (side + volume up, or AssistiveTouch)
 *   EV crash PATH                    a new crash / jetsam report
 *   EV hello VERSION FEATURES        at start; EV bye REASON at exit
 *
 * One instance: a newer one sends SIGTERM to the older (pid in /private/var/tmp/vevents.pid). It exits by itself
 * when no host tool has been active for 30 s (sysctl kern.viva_linkstate != 2, patch 0182), so it costs nothing
 * once the Mac goes away. Everything except libSystem comes through dlsym (built without the iOS SDK, see build.sh).
 */
#include <dirent.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libproc.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define VERSION   1
#define PIDFILE   "/private/var/tmp/vevents.pid"
#define DCIM      "/private/var/mobile/Media/DCIM"
#define CRASHDIR  "/private/var/mobile/Library/Logs/CrashReporter"
#define IDLE_EXIT 30            /* seconds without an active host tool */
#ifndef PRIO_DARWIN_ROLE        /* private in the macOS SDK headers (bsd/sys/resource.h) */
#define PRIO_DARWIN_ROLE          6
#define PRIO_DARWIN_ROLE_UI_FOCAL 1
#endif

typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFRunLoop *CFRunLoopRef;
typedef const struct __CFURL *CFURLRef;
typedef struct __CFBundle *CFBundleRef;
typedef void *HIDClient, *HIDEvent, *HIDService;
typedef void (*HIDCallback)(void *target, void *refcon, HIDService sender, HIDEvent event);

static struct {
    CFRunLoopRef (*CFRunLoopGetCurrent)(void);
    void (*CFRunLoopRun)(void);
    CFStringRef *kCFRunLoopDefaultMode;
    long (*CFArrayGetCount)(CFArrayRef);
    const void *(*CFArrayGetValueAtIndex)(CFArrayRef, long);
    unsigned char (*CFStringGetCString)(CFStringRef, char *, long, uint32_t);
    void (*CFRelease)(CFTypeRef);
    HIDClient (*IOHIDEventSystemClientCreateWithType)(const void *, int, const void *);
    void (*IOHIDEventSystemClientScheduleWithRunLoop)(HIDClient, CFRunLoopRef, CFStringRef);
    void (*IOHIDEventSystemClientRegisterEventCallback)(HIDClient, HIDCallback, void *, void *);
    uint32_t (*IOHIDEventGetType)(HIDEvent);
    long (*IOHIDEventGetIntegerValue)(HIDEvent, uint32_t);
    double (*IOHIDEventGetFloatValue)(HIDEvent, uint32_t);
    CFArrayRef (*IOHIDEventGetChildren)(HIDEvent);
    CFURLRef (*CFURLCreateFromFileSystemRepresentation)(const void *, const unsigned char *, long, unsigned char);
    CFBundleRef (*CFBundleCreate)(const void *, CFURLRef);
    CFStringRef (*CFBundleGetIdentifier)(CFBundleRef);
} f;

#define kCFStringEncodingUTF8 0x08000100
enum { kTypeKeyboard = 3, kTypeDigitizer = 11 };
#define FIELD(type, n) (((uint32_t)(type) << 16) | (n))
#define kKeyPage    FIELD(kTypeKeyboard, 0)
#define kKeyUsage   FIELD(kTypeKeyboard, 1)
#define kKeyDown    FIELD(kTypeKeyboard, 2)
#define kDigX       FIELD(kTypeDigitizer, 0)
#define kDigY       FIELD(kTypeDigitizer, 1)
#define kDigIndex   FIELD(kTypeDigitizer, 5)
#define kDigIdent   FIELD(kTypeDigitizer, 6)
#define kDigTouch   FIELD(kTypeDigitizer, 9)

static void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ev(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("EV ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int load(void)
{
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    void *iok = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!cf)
        return -1;
#define SYM(lib, name) (*(void **)&f.name = lib ? dlsym(lib, #name) : NULL)
    SYM(cf, CFRunLoopGetCurrent); SYM(cf, CFRunLoopRun); SYM(cf, kCFRunLoopDefaultMode);
    SYM(cf, CFArrayGetCount); SYM(cf, CFArrayGetValueAtIndex); SYM(cf, CFStringGetCString); SYM(cf, CFRelease);
    SYM(iok, IOHIDEventSystemClientCreateWithType); SYM(iok, IOHIDEventSystemClientScheduleWithRunLoop);
    SYM(iok, IOHIDEventSystemClientRegisterEventCallback); SYM(iok, IOHIDEventGetType);
    SYM(iok, IOHIDEventGetIntegerValue); SYM(iok, IOHIDEventGetFloatValue); SYM(iok, IOHIDEventGetChildren);
    SYM(cf, CFURLCreateFromFileSystemRepresentation); SYM(cf, CFBundleCreate); SYM(cf, CFBundleGetIdentifier);
#undef SYM
    return f.CFRunLoopGetCurrent && f.CFRunLoopRun && f.kCFRunLoopDefaultMode && f.CFRelease ? 0 : -1;
}

/* ---- touches and keys ---- */

#define MAX_FINGERS 10
static struct finger {
    long ident;
    int down;
    uint64_t t0;
    double x0, y0, x, y, dist;
} fingers[MAX_FINGERS];

static struct finger *finger_for(long ident)
{
    struct finger *free_slot = NULL;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (fingers[i].down && fingers[i].ident == ident)
            return &fingers[i];
        if (!fingers[i].down && !free_slot)
            free_slot = &fingers[i];
    }
    return free_slot;
}

static void finger_event(HIDEvent e)
{
    long ident = f.IOHIDEventGetIntegerValue(e, kDigIdent);
    int touch = f.IOHIDEventGetIntegerValue(e, kDigTouch) != 0;
    double x = f.IOHIDEventGetFloatValue(e, kDigX), y = f.IOHIDEventGetFloatValue(e, kDigY);
    struct finger *fi = finger_for(ident);
    if (!fi)
        return;
    if (touch && !fi->down) {
        *fi = (struct finger){ .ident = ident, .down = 1, .t0 = now_ms(), .x0 = x, .y0 = y, .x = x, .y = y };
        ev("touch down %ld %.3f %.3f", ident, x, y);
    } else if (touch) {
        double dx = x - fi->x0, dy = y - fi->y0, d = dx * dx + dy * dy;
        if (d > fi->dist)
            fi->dist = d;
        fi->x = x;
        fi->y = y;
    } else if (fi->down) {
        double dx = x - fi->x0, dy = y - fi->y0;
        /* the lift report may carry 0,0 on some digitizers: fall back to the last position seen */
        if (x == 0 && y == 0) {
            x = fi->x;
            y = fi->y;
            dx = x - fi->x0;
            dy = y - fi->y0;
        }
        double d = dx * dx + dy * dy;
        if (d < fi->dist)
            d = fi->dist;
        fi->down = 0;
        ev("touch up %ld %.3f %.3f %llu %.3f", ident, x, y, (unsigned long long)(now_ms() - fi->t0), __builtin_sqrt(d));
    }
}

static void key_event(HIDEvent e)
{
    long page = f.IOHIDEventGetIntegerValue(e, kKeyPage), usage = f.IOHIDEventGetIntegerValue(e, kKeyUsage);
    int down = f.IOHIDEventGetIntegerValue(e, kKeyDown) != 0;
    const char *name = NULL;
    if (page == 0x0c && usage == 0x30)
        name = "power";
    else if (page == 0x0c && usage == 0xe9)
        name = "volup";
    else if (page == 0x0c && usage == 0xea)
        name = "voldown";
    else if (page == 0x0c && usage == 0x40)
        name = "menu";
    else if (page == 0x0b && usage == 0x2f)
        name = "mute";
    else if (page == 0xff01)
        return;                 /* WillReset sequence markers (0192) */
    if (name)
        ev("key %s %s", down ? "down" : "up", name);
    else
        ev("key %s %lx:%lx", down ? "down" : "up", page, usage);
}

static void hid_callback(void *target, void *refcon, HIDService sender, HIDEvent e)
{
    (void)target; (void)refcon; (void)sender;
    uint32_t type = f.IOHIDEventGetType(e);
    if (type == kTypeKeyboard) {
        key_event(e);
    } else if (type == kTypeDigitizer) {
        CFArrayRef kids = f.IOHIDEventGetChildren ? f.IOHIDEventGetChildren(e) : NULL;
        long n = kids ? f.CFArrayGetCount(kids) : 0;
        for (long i = 0; i < n; i++) {
            HIDEvent c = (HIDEvent)f.CFArrayGetValueAtIndex(kids, i);
            if (f.IOHIDEventGetType(c) == kTypeDigitizer)
                finger_event(c);
        }
        if (n == 0)
            finger_event(e);
    }
}

static int start_hid(void)
{
    if (!f.IOHIDEventSystemClientCreateWithType || !f.IOHIDEventSystemClientRegisterEventCallback ||
        !f.IOHIDEventSystemClientScheduleWithRunLoop || !f.IOHIDEventGetType || !f.IOHIDEventGetIntegerValue ||
        !f.IOHIDEventGetFloatValue)
        return -1;
    /* 1 = monitor (sees every event without taking it); admin (0) as a fallback */
    HIDClient c = f.IOHIDEventSystemClientCreateWithType(NULL, 1, NULL);
    if (!c)
        c = f.IOHIDEventSystemClientCreateWithType(NULL, 0, NULL);
    if (!c)
        return -1;
    f.IOHIDEventSystemClientRegisterEventCallback(c, hid_callback, NULL, NULL);
    f.IOHIDEventSystemClientScheduleWithRunLoop(c, f.CFRunLoopGetCurrent(), *f.kCFRunLoopDefaultMode);
    return 0;
}

/* ---- notifications ---- */

/* reports the state at start too, so a watcher knows where the phone is */
static void watch_state(const char *name, const char *what)
{
    int token;
    if (notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
            uint64_t st = 0;
            notify_get_state(t, &st);
            ev("%s %d", what, st != 0);
        }) != NOTIFY_STATUS_OK) {
        fprintf(stderr, "vevents: cannot watch %s\n", name);
        return;
    }
    uint64_t st = 0;
    notify_get_state(token, &st);
    ev("%s %d", what, st != 0);
}

/* ---- polled: frontmost app, new files, host still there ---- */

/*
 * Frontmost app from the kernel, no SpringBoard API (SBSCopyFrontmostApplicationDisplayIdentifier answers only
 * platform binaries): RunningBoard gives the focal app the Darwin role UI_FOCAL. SpringBoard and kbd have it too.
 */
static char front[256];
static pid_t front_pid = -1;

static void bundle_id(const char *exe, char *out, size_t len)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", exe);
    char *app = strstr(dir, ".app/");
    if (!app) {
        snprintf(out, len, "%s", strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe);
        return;
    }
    app[4] = 0;
    out[0] = 0;
    if (f.CFURLCreateFromFileSystemRepresentation && f.CFBundleCreate && f.CFBundleGetIdentifier && f.CFStringGetCString) {
        CFURLRef url = f.CFURLCreateFromFileSystemRepresentation(NULL, (const unsigned char *)dir, (long)strlen(dir), 1);
        CFBundleRef b = url ? f.CFBundleCreate(NULL, url) : NULL;
        CFStringRef id = b ? f.CFBundleGetIdentifier(b) : NULL;
        if (!id || !f.CFStringGetCString(id, out, (long)len, kCFStringEncodingUTF8))
            out[0] = 0;
        if (b)
            f.CFRelease(b);
        if (url)
            f.CFRelease(url);
    }
    if (!out[0]) {
        char *name = strrchr(dir, '/');
        snprintf(out, len, "%s", name ? name + 1 : dir);
    }
}

static void poll_front(void)
{
    static int pids[4096];
    int n = proc_listallpids(pids, sizeof(pids));
    pid_t best = -1, sb = -1;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i = 0; i < n; i++) {
        if (getpriority(PRIO_DARWIN_ROLE, pids[i]) != PRIO_DARWIN_ROLE_UI_FOCAL)
            continue;
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0 || !strstr(path, ".app/"))
            continue;
        if (strstr(path, "/SpringBoard.app/"))
            sb = pids[i];
        else
            best = pids[i];
    }
    if (best < 0)
        best = sb;
    if (best == front_pid)
        return;
    front_pid = best;
    char id[256] = "none";
    if (best > 0 && proc_pidpath(best, path, sizeof(path)) > 0)
        bundle_id(path, id, sizeof(id));
    if (strcmp(id, front) != 0) {
        snprintf(front, sizeof(front), "%s", id);
        ev("app %s %d", id, best);
    }
}

/* newest regular file under DIR (one level of subdirectories: DCIM/100APPLE/IMG_0001.PNG) */
static void newest(const char *dir, int depth, char *best, size_t bestlen, struct timespec *bt)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    char path[1024];
    struct stat st;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0)
                newest(path, depth - 1, best, bestlen, bt);
        } else if (st.st_mtimespec.tv_sec > bt->tv_sec ||
                   (st.st_mtimespec.tv_sec == bt->tv_sec && st.st_mtimespec.tv_nsec > bt->tv_nsec)) {
            *bt = st.st_mtimespec;
            snprintf(best, bestlen, "%s", path);
        }
    }
    closedir(d);
}

struct dirwatch {
    const char *dir, *what;
    int depth;
    char last[1024];
    struct timespec t;
};

static void poll_dir(struct dirwatch *w, int report)
{
    char best[1024] = "";
    struct timespec bt = w->t;
    newest(w->dir, w->depth, best, sizeof(best), &bt);
    if (best[0] && (bt.tv_sec != w->t.tv_sec || bt.tv_nsec != w->t.tv_nsec)) {
        w->t = bt;
        if (report && strcmp(best, w->last) != 0)
            ev("%s %s", w->what, best);
        snprintf(w->last, sizeof(w->last), "%s", best);
    }
}

static int link_active(void)
{
    int st = -1;
    size_t len = sizeof(st);
    if (sysctlbyname("kern.viva_linkstate", &st, &len, NULL, 0) != 0)
        return 1;               /* older kernel: never leave on our own */
    return st == 2;
}

static void take_over(void)
{
    char buf[32] = "";
    int fd = open(PIDFILE, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        pid_t old = n > 0 ? (pid_t)atoi(buf) : 0;
        if (old > 1 && old != getpid() && kill(old, SIGTERM) == 0) {
            for (int i = 0; i < 20 && kill(old, 0) == 0; i++)
                usleep(50000);
            kill(old, SIGKILL);
        }
    }
    fd = open(PIDFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dprintf(fd, "%d\n", getpid());
        close(fd);
    }
}

static void quit(const char *why)
{
    ev("bye %s", why);
    char buf[32] = "";
    int fd = open(PIDFILE, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0 && atoi(buf) == getpid())
            unlink(PIDFILE);
    }
    exit(0);
}

int main(int argc, char **argv)
{
    int idle_exit = IDLE_EXIT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stay") == 0)
            idle_exit = 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    take_over();
    if (load() != 0) {
        printf("EV bye no CoreFoundation\n");
        return 1;
    }
    int hid = start_hid() == 0;
    ev("hello %d hid=%d", VERSION, hid);

    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    signal(SIGTERM, SIG_IGN);
    dispatch_source_set_event_handler(term, ^{ quit("replaced"); });
    dispatch_resume(term);

    watch_state("com.apple.springboard.lockstate", "lock");
    watch_state("com.apple.iokit.hid.displayStatus", "display");

    static struct dirwatch shots = { DCIM, "screenshot", 1 }, crashes = { CRASHDIR, "crash", 1 };
    poll_dir(&shots, 0);
    poll_dir(&crashes, 0);
    poll_front();

    __block int idle = 0;
    dispatch_source_t tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), 500 * NSEC_PER_MSEC,
                              100 * NSEC_PER_MSEC);
    __block unsigned n = 0;
    dispatch_source_set_event_handler(tick, ^{
        poll_front();
        if (++n % 2 == 0) {
            poll_dir(&shots, 1);
            poll_dir(&crashes, 1);
            idle = link_active() ? 0 : idle + 1;
            if (idle_exit && idle >= idle_exit)
                quit("host gone");
        }
    });
    dispatch_resume(tick);

    f.CFRunLoopRun();           /* the main queue is drained by the main run loop */
    return 0;
}
