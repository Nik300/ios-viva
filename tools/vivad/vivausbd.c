/*
 * vivausbd: the "standard iPhone" USB functions of the viva phone (patch 0182,
 * docs/VIVA-USB-PERSONA.md). The kernel (bsd/dev/viva_usb.c) only moves bulk
 * transfers through /dev/vivaptp and /dev/vivamux; this daemon speaks the
 * protocols, so a Mac/PC without the Viva Link tool sees an iPhone:
 *   - PTP/MTP responder (Image Capture, Photos, Windows Explorer) over
 *     /private/var/mobile/Media;
 *   - the usbmux device side (Finder/iTunes/libimobiledevice -> lockdownd and
 *     the services lockdownd starts), bridged to localhost TCP.
 *
 * Startup: UDID from MobileGestalt (fallback: SHA-1 of IOPlatformUUID or of a
 * random seed kept in /private/var/root/.vivausbd-udid; logged),
 * open both nodes (retried until they exist), VIVAUSB_SET_SERIAL(UDID),
 * VIVAUSB_ATTACH. Threads: mux reader, mux writer, mux socket poller, PTP
 * reader, PTP responder. The nodes do not support poll/select: blocking
 * read() in dedicated threads. read() == -1/ECONNRESET (bus reset, unplug,
 * SET_CONFIGURATION 0): all usbmux connections, the version/seq state and the
 * PTP session are dropped.
 *
 * ---------------------------------------------------------------- usbmux ---
 * Wire format (host side = usbmuxd src/device.c; all integers big-endian).
 * Every mux packet is exactly one USB transfer in each direction; the OUT
 * side is still reassembled as a byte stream by header length here.
 *
 *   v1 header (8):   u32 protocol, u32 length (whole packet incl. header)
 *   v2 header (16):  u32 protocol, u32 length, u32 magic 0xfeedface,
 *                    u16 tx_seq, u16 rx_seq
 *   protocol: 0 VERSION, 1 CONTROL, 2 SETUP, 6 TCP
 *
 *   1. host -> VERSION, v1 header, payload u32 major=2, u32 minor=0, u32 pad.
 *      device <- VERSION, v1 header, payload major 2, minor 0, pad 0.
 *      From here on both sides use v2 headers.
 *   2. host -> SETUP (v2, tx_seq 0 / rx_seq 0xffff), payload 1 byte 0x07.
 *      No reply. The host resets its tx_seq when sending SETUP; it does not
 *      check seq numbers. The device keeps its own tx_seq (+1 per packet,
 *      reset at VERSION) and echoes the host's last tx_seq in rx_seq.
 *   3. CONTROL (device -> host only: payload[0] 3 = error text, 5/7 info);
 *      ignored when received.
 *   4. TCP: payload = struct tcphdr (20 bytes, BSD layout, BE): u16 sport,
 *      u16 dport, u32 seq, u32 ack, u8 offset 0x50, u8 flags
 *      (FIN 1, SYN 2, RST 4, PSH 8, ACK 0x10), u16 window (bytes >> 8),
 *      u16 sum 0, u16 urp 0; then data. Host sport = its connection id,
 *      dport = device port; the device answers with the ports swapped.
 *        host SYN (seq 0, ack 0, win 128K>>8) -> device connects to
 *          127.0.0.1:dport (62078 = lockdownd; any other port too: services
 *          started by lockdownd listen on dynamic ports; 62078 falls back to
 *          lockdownd's launchd Unix socket /var/run/lockdown.sock)
 *          ok:   SYN|ACK seq 0 ack 1 (host then sends ACK, seq/ack 1)
 *          fail: RST
 *        data both ways with flags == ACK exactly (usbmuxd tears the
 *          connection down on any other flag combination, so no FIN/PSH);
 *          seq/ack count bytes from 1. The device sends at most
 *          host_win - (snd_nxt - host_ack) unacknowledged bytes, and ACKs
 *          (window updates) as its receive queue drains to the socket.
 *        close: RST in either direction (usbmuxd flushes what it already
 *          has to its client). RST for unknown connections.
 *   Device -> host packets stay <= 32 KiB, and data packets never have a
 *   length that is a multiple of 512 (no ZLP dependency on the host).
 *
 * ------------------------------------------------------------------- PTP ---
 * Containers (little-endian): u32 length, u16 type (1 command, 2 data,
 * 3 response, 4 event), u16 code, u32 transaction id, then params / data.
 * A data phase is sent as one or more write()s (<= 64 KiB), the first
 * starting with the container header, totalling the container length; the
 * kernel closes the IN transfer when the container is complete. Storage
 * 0x00010001 "iPhone" = /private/var/mobile/Media; handles are assigned per
 * session, lazily, directory by directory; symlinks are never listed or
 * followed, names with '/' or "." / ".." are refused. ECANCELED from read()
 * (class CANCEL_REQUEST / DEVICE_RESET) aborts the current transaction.
 *
 * ----------------------------------------------------------------- build ---
 *   tools/vivad/build.sh (iOS SDK; CI: .github/workflows/xnu.yml) -> out/vivausbd
 *   macOS test build:  clang -O2 -Wall -o /tmp/vivausbd tools/vivad/vivausbd.c -lpthread
 *   tests: python3 tools/vivad/tests/run_tests.py
 *
 * ---------------------------------------------------------------- launchd ---
 * Root LaunchDaemon com.viva.vivausbd, RunAtLoad + KeepAlive, stderr on the
 * console, added to the launchd job cache like vivad:
 *   tools/viva-link.py pull /System/Library/xpc/launchd.plist launchd.plist
 *   tools/viva-launchd-add.py launchd.plist launchd-new.plist \
 *       /usr/libexec/vivausbd com.viva.vivausbd
 *   tools/viva-link.py push tools/vivad/out/vivausbd /usr/libexec/vivausbd 755
 *   tools/viva-link.py push launchd-new.plist /System/Library/xpc/launchd.plist 644
 *
 * Options: --udid X, --root DIR (PTP storage), --map PORT=HOST:PORT or
 * PORT=/unix/socket (repeatable; replaces the default targets of PORT),
 * --owner UID:GID (files written via PTP, default 501:501), --no-ptp,
 * --no-mux, -v, --test-sock DIR (endpoints = Unix stream sockets DIR/ptp.sock
 * and DIR/mux.sock with framing u32 LE length + bytes per transfer; a frame of
 * length 0xffffffff followed by u32 LE 1 = ECONNRESET, 2 = ECANCELED; a client
 * disconnect = ECONNRESET).
 */
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "vivausb.h"

#define PTP_DEV      "/dev/vivaptp"
#define MUX_DEV      "/dev/vivamux"
#define MEDIA_ROOT   "/private/var/mobile/Media"
#define LOCKDOWN_PORT 62078
#define LOCKDOWN_SOCK "/var/run/lockdown.sock"
#define UDID_FILE    "/private/var/root/.vivausbd-udid"

static int verbose;
static uid_t own_uid = 501;
static gid_t own_gid = 501;
static char udid[64];
static char storage_root[1024] = MEDIA_ROOT;

static void
logm(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "vivausbd: %s\n", buf);
}
#define vlog(...) do { if (verbose) logm(__VA_ARGS__); } while (0)

/* ================================================================ endpoints */

/*
 * One USB function endpoint pair: the /dev node, or (--test-sock) a listening
 * Unix stream socket whose client sends/receives framed transfers.
 */
struct ep {
	const char *name;
	int fd;                         /* device node, or connected test client */
	int lfd;                        /* test: listening socket */
	bool test;
	pthread_mutex_t wlock;
	pthread_mutex_t clock;          /* test: protects fd changes */
	/* test: rest of the current frame not yet returned by ep_read */
	uint32_t frame_left;
};

static bool
readn(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	while (n) {
		ssize_t r = read(fd, p, n);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			return false;
		}
		p += r;
		n -= (size_t)r;
	}
	return true;
}

static bool
writen(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	while (n) {
		ssize_t r = write(fd, p, n);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			return false;
		}
		p += r;
		n -= (size_t)r;
	}
	return true;
}

/* Like read() on the node: bytes of one OUT transfer, -1 with ECONNRESET/ECANCELED. */
static ssize_t
ep_read(struct ep *e, void *buf, size_t len)
{
	if (!e->test) {
		for (;;) {
			ssize_t r = read(e->fd, buf, len);
			if (r < 0 && errno == EINTR) {
				continue;
			}
			if (r == 0) {           /* never expected from the node */
				errno = EIO;
				return -1;
			}
			return r;
		}
	}
	for (;;) {
		if (e->fd < 0) {
			int c = accept(e->lfd, NULL, NULL);
			if (c < 0) {
				if (errno == EINTR || errno == ECONNABORTED) {
					continue;
				}
				return -1;
			}
			pthread_mutex_lock(&e->clock);
			e->fd = c;
			e->frame_left = 0;
			pthread_mutex_unlock(&e->clock);
			vlog("%s: test client connected", e->name);
		}
		if (e->frame_left == 0) {
			uint32_t l;
			if (!readn(e->fd, &l, 4)) {
				goto gone;
			}
			if (l == 0xffffffffu) {
				uint32_t code;
				if (!readn(e->fd, &code, 4)) {
					goto gone;
				}
				errno = code == 2 ? ECANCELED : ECONNRESET;
				return -1;
			}
			if (l == 0) {
				continue;       /* a ZLP is never reported */
			}
			e->frame_left = l;
		}
		size_t n = len < e->frame_left ? len : e->frame_left;
		if (!readn(e->fd, buf, n)) {
			goto gone;
		}
		e->frame_left -= (uint32_t)n;
		return (ssize_t)n;
gone:
		pthread_mutex_lock(&e->clock);
		close(e->fd);
		e->fd = -1;
		pthread_mutex_unlock(&e->clock);
		vlog("%s: test client gone", e->name);
		errno = ECONNRESET;
		return -1;
	}
}

/* Like write() on the node: one IN transfer (or one piece of a PTP container). */
static int
ep_write(struct ep *e, const void *buf, size_t len)
{
	int ret = 0;
	pthread_mutex_lock(&e->wlock);
	if (!e->test) {
		for (;;) {
			ssize_t r = write(e->fd, buf, len);
			if (r < 0 && errno == EINTR) {
				continue;
			}
			if (r < 0) {
				ret = -errno;
			} else if ((size_t)r != len) {
				ret = -EIO;
			}
			break;
		}
	} else {
		pthread_mutex_lock(&e->clock);
		int fd = e->fd;
		pthread_mutex_unlock(&e->clock);
		uint32_t l = (uint32_t)len;
		if (fd < 0) {
			ret = -ENXIO;
		} else if (!writen(fd, &l, 4) || !writen(fd, buf, len)) {
			ret = -EIO;
		}
	}
	pthread_mutex_unlock(&e->wlock);
	return ret;
}

static int
ep_open(struct ep *e, const char *path, const char *testdir, const char *name)
{
	memset(e, 0, sizeof(*e));
	e->name = name;
	e->fd = e->lfd = -1;
	pthread_mutex_init(&e->wlock, NULL);
	pthread_mutex_init(&e->clock, NULL);
	if (testdir) {
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%s.sock", testdir, name);
		unlink(sa.sun_path);
		e->test = true;
		e->lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (e->lfd < 0 || bind(e->lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(e->lfd, 1) < 0) {
			logm("%s: %s", sa.sun_path, strerror(errno));
			return -1;
		}
		return 0;
	}
	for (int tries = 0;; tries++) {
		e->fd = open(path, O_RDWR);
		if (e->fd >= 0) {
			return 0;
		}
		if (tries == 0) {
			logm("%s: %s, waiting", path, strerror(errno));
		}
		sleep(tries < 30 ? 1 : 5);
	}
}

static struct ep ptp_ep, mux_ep;

/* ===================================================================== UDID */

typedef const void *CFTypeRef;
typedef const void *CFStringRef;

static bool
udid_from_gestalt(void)
{
	void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
	if (!mg || !cf) {
		return false;
	}
	CFTypeRef (*MGCopyAnswer)(CFStringRef) = (CFTypeRef (*)(CFStringRef))dlsym(mg, "MGCopyAnswer");
	CFStringRef (*mkstr)(const void *, const char *, uint32_t) =
	    (CFStringRef (*)(const void *, const char *, uint32_t))dlsym(cf, "CFStringCreateWithCString");
	bool (*getstr)(CFStringRef, char *, long, uint32_t) =
	    (bool (*)(CFStringRef, char *, long, uint32_t))dlsym(cf, "CFStringGetCString");
	unsigned long (*typeid_of)(CFTypeRef) = (unsigned long (*)(CFTypeRef))dlsym(cf, "CFGetTypeID");
	unsigned long (*strtype)(void) = (unsigned long (*)(void))dlsym(cf, "CFStringGetTypeID");
	void (*rel)(CFTypeRef) = (void (*)(CFTypeRef))dlsym(cf, "CFRelease");
	if (!MGCopyAnswer || !mkstr || !getstr || !typeid_of || !strtype || !rel) {
		return false;
	}
	const uint32_t utf8 = 0x08000100;
	CFStringRef key = mkstr(NULL, "UniqueDeviceID", utf8);
	if (!key) {
		return false;
	}
	CFTypeRef v = MGCopyAnswer(key);
	rel(key);
	bool ok = false;
	if (v) {
		if (typeid_of(v) == strtype()) {
			ok = getstr(v, udid, sizeof(udid), utf8) && udid[0];
		}
		rel(v);
	}
	return ok;
}

/* IOPlatformExpertDevice's IOPlatformUUID (string property), via dlopen'ed IOKit. */
static bool
platform_uuid(char *out, size_t sz)
{
	void *iok = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
	if (!iok || !cf) {
		return false;
	}
	void *(*matching)(const char *) = (void *(*)(const char *))dlsym(iok, "IOServiceMatching");
	unsigned (*getsvc)(unsigned, void *) = (unsigned (*)(unsigned, void *))dlsym(iok, "IOServiceGetMatchingService");
	CFTypeRef (*getprop)(unsigned, CFStringRef, const void *, unsigned) =
	    (CFTypeRef (*)(unsigned, CFStringRef, const void *, unsigned))dlsym(iok, "IORegistryEntryCreateCFProperty");
	int (*objrel)(unsigned) = (int (*)(unsigned))dlsym(iok, "IOObjectRelease");
	CFStringRef (*mkstr)(const void *, const char *, uint32_t) =
	    (CFStringRef (*)(const void *, const char *, uint32_t))dlsym(cf, "CFStringCreateWithCString");
	bool (*getstr)(CFStringRef, char *, long, uint32_t) =
	    (bool (*)(CFStringRef, char *, long, uint32_t))dlsym(cf, "CFStringGetCString");
	unsigned long (*typeid_of)(CFTypeRef) = (unsigned long (*)(CFTypeRef))dlsym(cf, "CFGetTypeID");
	unsigned long (*strtype)(void) = (unsigned long (*)(void))dlsym(cf, "CFStringGetTypeID");
	void (*rel)(CFTypeRef) = (void (*)(CFTypeRef))dlsym(cf, "CFRelease");
	if (!matching || !getsvc || !getprop || !objrel || !mkstr || !getstr || !typeid_of || !strtype || !rel) {
		return false;
	}
	unsigned svc = getsvc(0, matching("IOPlatformExpertDevice"));
	if (!svc) {
		return false;
	}
	bool ok = false;
	CFStringRef k = mkstr(NULL, "IOPlatformUUID", 0x08000100);
	CFTypeRef v = k ? getprop(svc, k, NULL, 0) : NULL;
	if (v) {
		ok = typeid_of(v) == strtype() && getstr(v, out, (long)sz, 0x08000100) && out[0];
		rel(v);
	}
	if (k) {
		rel(k);
	}
	objrel(svc);
	return ok;
}

static void
get_udid(void)
{
	if (udid[0]) {
		return;
	}
	if (udid_from_gestalt()) {
		logm("UDID %s (MobileGestalt)", udid);
		return;
	}
	/*
	 * Stable fallback, 40 hex digits (pre-2018 UDID format): SHA-1 of IOPlatformUUID, else a
	 * random value kept in UDID_FILE. lockdownd answers with its own UniqueDeviceID, so a
	 * mismatch with the USB serial confuses pairing: this is logged loudly.
	 */
	char seed[128] = "";
	const char *how = "IOPlatformUUID";
	if (!platform_uuid(seed, sizeof(seed))) {
		how = UDID_FILE;
		int fd = open(UDID_FILE, O_RDONLY);
		ssize_t n = fd >= 0 ? read(fd, seed, sizeof(seed) - 1) : -1;
		if (fd >= 0) {
			close(fd);
		}
		if (n <= 0) {
			arc4random_buf(seed, 32);
			for (int i = 0; i < 32; i++) {
				seed[i] = "0123456789abcdef"[(uint8_t)seed[i] & 15];
			}
			seed[32] = 0;
			fd = open(UDID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (fd >= 0) {
				(void)write(fd, seed, 32);
				close(fd);
			}
		} else {
			seed[n] = 0;
		}
	}
	uint8_t md[CC_SHA1_DIGEST_LENGTH];
	CC_SHA1(seed, (CC_LONG)strlen(seed), md);
	for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) {
		snprintf(udid + 2 * i, 3, "%02x", md[i]);
	}
	logm("UDID %s (MobileGestalt failed: derived from %s; lockdownd may disagree)", udid, how);
}

/* =================================================================== usbmux */

#define MUX_VERSION 0
#define MUX_CONTROL 1
#define MUX_SETUP   2
#define MUX_TCP     6
#define MUX_MAGIC   0xfeedfaceu

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_ACK 0x10

#define MUX_MAX_TX      32768                   /* device -> host packet */
#define MUX_MAX_RX      (1 << 20)               /* sanity limit host -> device */
#define MUX_HDR2        16
#define TCP_HDR         20
#define RCV_CAP         (256 * 1024)            /* receive window per connection */
#define MAX_CONNS       64
#define TXQ_LIMIT       (512 * 1024)            /* stop reading sockets above this */

struct mconn {
	bool used;
	bool closing;           /* host sent RST: flush outq to the socket, then close */
	int fd;
	uint32_t id;
	uint16_t hport;         /* host's port (its sport) */
	uint16_t dport;         /* device port */
	uint32_t snd_nxt;       /* next seq we send */
	uint32_t host_ack;      /* host's last ack of our data */
	uint32_t host_win;      /* host's window in bytes */
	uint32_t rcv_nxt;       /* next host seq expected = our ack */
	uint32_t ack_sent, win_sent;
	uint8_t *q;             /* host -> socket bytes not yet written */
	size_t qlen, qcap;
};

struct txpkt {
	struct txpkt *next;
	size_t len;
	uint8_t data[];
};

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t txcond = PTHREAD_COND_INITIALIZER;
static struct mconn conns[MAX_CONNS];
static uint32_t conn_ids;
static int mux_version;         /* 0 until VERSION, then 2 */
static uint16_t mux_tx_seq, mux_rx_seq;
static struct txpkt *txhead, *txtail;
static size_t txbytes;
static int wake_pipe[2];

struct target {
	uint16_t port;
	char spec[200];         /* "HOST:PORT" or "/path" */
};
static struct target targets[32];
static int ntargets;

static void
put16be(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}
static void
put32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}
static uint16_t
get16be(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}
static uint32_t
get32be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void
wake(void)
{
	char c = 1;
	(void)write(wake_pipe[1], &c, 1);
}

/* Queue one packet for the writer thread. Caller holds mx. */
static void
mux_queue(int proto, const uint8_t *hdr, size_t hdrlen, const uint8_t *data, size_t len)
{
	size_t mh = (proto == MUX_VERSION || mux_version < 2) ? 8 : MUX_HDR2;
	size_t total = mh + hdrlen + len;
	struct txpkt *p = malloc(sizeof(*p) + total);
	if (!p) {
		return;
	}
	p->next = NULL;
	p->len = total;
	put32be(p->data, (uint32_t)proto);
	put32be(p->data + 4, (uint32_t)total);
	if (mh == MUX_HDR2) {
		put32be(p->data + 8, MUX_MAGIC);
		put16be(p->data + 12, mux_tx_seq);
		put16be(p->data + 14, mux_rx_seq);
		mux_tx_seq++;
	}
	if (hdrlen) {
		memcpy(p->data + mh, hdr, hdrlen);
	}
	if (len) {
		memcpy(p->data + mh + hdrlen, data, len);
	}
	if (txtail) {
		txtail->next = p;
	} else {
		txhead = p;
	}
	txtail = p;
	txbytes += total;
	pthread_cond_signal(&txcond);
}

static uint32_t
conn_rwin(struct mconn *c)
{
	return c->qlen >= RCV_CAP ? 0 : (uint32_t)(RCV_CAP - c->qlen);
}

/* TCP packet with the ports of c (or explicit ports when c is NULL). Caller holds mx. */
static void
tcp_send_raw(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
    uint32_t win, const uint8_t *data, size_t len)
{
	uint8_t th[TCP_HDR] = { 0 };
	put16be(th, sport);
	put16be(th + 2, dport);
	put32be(th + 4, seq);
	put32be(th + 8, ack);
	th[12] = 0x50;
	th[13] = flags;
	put16be(th + 14, (uint16_t)((win >> 8) > 0xffff ? 0xffff : (win >> 8)));
	mux_queue(MUX_TCP, th, TCP_HDR, data, len);
}

static void
tcp_send(struct mconn *c, uint8_t flags, const uint8_t *data, size_t len)
{
	uint32_t win = conn_rwin(c);
	tcp_send_raw(c->dport, c->hport, c->snd_nxt, c->rcv_nxt, flags, win, data, len);
	c->snd_nxt += (uint32_t)len;
	c->ack_sent = c->rcv_nxt;
	c->win_sent = win;
}

/* Window update when worth it. Caller holds mx. */
static void
maybe_ack(struct mconn *c)
{
	uint32_t win = conn_rwin(c);
	if (c->ack_sent != c->rcv_nxt || win >= c->win_sent + RCV_CAP / 4 || (c->win_sent == 0 && win > 0)) {
		tcp_send(c, TH_ACK, NULL, 0);
	}
}

static void
conn_free(struct mconn *c)
{
	if (c->fd >= 0) {
		close(c->fd);
	}
	free(c->q);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

/* Device-side close: RST to the host, drop the connection. Caller holds mx. */
static void
conn_reset(struct mconn *c, const char *why)
{
	vlog("mux: %u -> %u closed (%s)", c->hport, c->dport, why);
	tcp_send(c, TH_RST, NULL, 0);
	conn_free(c);
}

static struct mconn *
conn_find(uint16_t hport, uint16_t dport)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		if (conns[i].used && conns[i].hport == hport && conns[i].dport == dport) {
			return &conns[i];
		}
	}
	return NULL;
}

/* Drop every connection and the version/seq state. Caller holds mx. */
static void
mux_reset_all(bool flush_tx)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		if (conns[i].used) {
			conn_free(&conns[i]);
		}
	}
	mux_version = 0;
	mux_tx_seq = 0;
	mux_rx_seq = 0;
	if (flush_tx) {
		while (txhead) {
			struct txpkt *p = txhead;
			txhead = p->next;
			free(p);
		}
		txtail = NULL;
		txbytes = 0;
	}
	wake();
}

static int
connect_spec(const char *spec, uint16_t port)
{
	int fd;
	if (spec[0] == '/') {
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		strlcpy(sa.sun_path, spec, sizeof(sa.sun_path));
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			return -1;
		}
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			close(fd);
			return -1;
		}
	} else {
		char host[128] = "127.0.0.1";
		const char *colon = strrchr(spec, ':');
		if (spec[0]) {
			size_t hl = colon ? (size_t)(colon - spec) : strlen(spec);
			if (hl >= sizeof(host)) {
				return -1;
			}
			memcpy(host, spec, hl);
			host[hl] = 0;
			if (colon) {
				port = (uint16_t)atoi(colon + 1);
			}
		}
		struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
		sa.sin_len = sizeof(sa);
		if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
			return -1;
		}
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			return -1;
		}
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			if (errno != EINPROGRESS) {
				close(fd);
				return -1;
			}
			struct pollfd pf = { .fd = fd, .events = POLLOUT };
			int err = 0;
			socklen_t el = sizeof(err);
			if (poll(&pf, 1, 3000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
				close(fd);
				return -1;
			}
		}
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

/* Connect to what the host asked for: --map entries, else 127.0.0.1:port (+ lockdown socket). */
static int
connect_port(uint16_t port)
{
	bool mapped = false;
	for (int i = 0; i < ntargets; i++) {
		if (targets[i].port == port) {
			mapped = true;
			int fd = connect_spec(targets[i].spec, port);
			if (fd >= 0) {
				return fd;
			}
		}
	}
	if (mapped) {
		return -1;
	}
	int fd = connect_spec("", port);
	if (fd < 0 && port == LOCKDOWN_PORT) {
		fd = connect_spec(LOCKDOWN_SOCK, port);
	}
	return fd;
}

/* Write as much of c->q as the socket takes. Caller holds mx. Returns false on socket error. */
static bool
conn_flush(struct mconn *c)
{
	while (c->qlen) {
		ssize_t w = write(c->fd, c->q, c->qlen);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno == EAGAIN;
		}
		memmove(c->q, c->q + w, c->qlen - (size_t)w);
		c->qlen -= (size_t)w;
	}
	return true;
}

static void
mux_tcp_input(const uint8_t *p, size_t len)
{
	if (len < TCP_HDR) {
		return;
	}
	uint16_t hport = get16be(p), dport = get16be(p + 2);
	uint32_t seq = get32be(p + 4), ack = get32be(p + 8);
	size_t off = (size_t)(p[12] >> 4) * 4;
	uint8_t flags = p[13];
	uint32_t win = (uint32_t)get16be(p + 14) << 8;
	if (off < TCP_HDR || off > len) {
		return;
	}
	const uint8_t *data = p + off;
	size_t dlen = len - off;

	if ((flags & (TH_SYN | TH_ACK | TH_RST)) == TH_SYN) {
		/* connect outside the lock: loopback is quick but may take a moment */
		int fd = connect_port(dport);
		pthread_mutex_lock(&mx);
		struct mconn *old = conn_find(hport, dport);
		if (old) {
			conn_free(old);
		}
		if (fd < 0) {
			vlog("mux: SYN %u -> %u refused", hport, dport);
			tcp_send_raw(dport, hport, 0, seq + 1, TH_RST | TH_ACK, 0, NULL, 0);
			pthread_mutex_unlock(&mx);
			return;
		}
		struct mconn *c = NULL;
		for (int i = 0; i < MAX_CONNS; i++) {
			if (!conns[i].used) {
				c = &conns[i];
				break;
			}
		}
		if (!c) {
			logm("mux: too many connections");
			close(fd);
			tcp_send_raw(dport, hport, 0, seq + 1, TH_RST | TH_ACK, 0, NULL, 0);
			pthread_mutex_unlock(&mx);
			return;
		}
		memset(c, 0, sizeof(*c));
		c->used = true;
		c->fd = fd;
		c->id = ++conn_ids;
		c->hport = hport;
		c->dport = dport;
		c->snd_nxt = 0;
		c->host_ack = 0;
		c->host_win = win;
		c->rcv_nxt = seq + 1;
		tcp_send(c, TH_SYN | TH_ACK, NULL, 0);
		c->snd_nxt = 1;         /* SYN takes one sequence number */
		vlog("mux: %u -> %u connected", hport, dport);
		wake();
		pthread_mutex_unlock(&mx);
		return;
	}

	pthread_mutex_lock(&mx);
	struct mconn *c = conn_find(hport, dport);
	if (!c) {
		if (!(flags & TH_RST)) {
			tcp_send_raw(dport, hport, ack, seq + (uint32_t)dlen, TH_RST, 0, NULL, 0);
		}
		pthread_mutex_unlock(&mx);
		return;
	}
	if (flags & (TH_RST | TH_FIN | TH_SYN)) {
		/* host closed (usbmuxd sends RST): deliver what is queued, then close */
		vlog("mux: %u -> %u closed by host", hport, dport);
		if (!conn_flush(c) || c->qlen == 0) {
			conn_free(c);
		} else {
			c->closing = true;
			shutdown(c->fd, SHUT_RD);
		}
		wake();
		pthread_mutex_unlock(&mx);
		return;
	}
	if (flags & TH_ACK) {
		c->host_ack = ack;
		c->host_win = win;
	}
	if (dlen && !c->closing) {
		if (c->qlen + dlen > c->qcap) {
			size_t ncap = c->qcap ? c->qcap : 65536;
			while (ncap < c->qlen + dlen) {
				ncap *= 2;
			}
			uint8_t *nq = realloc(c->q, ncap);
			if (!nq) {
				conn_reset(c, "out of memory");
				pthread_mutex_unlock(&mx);
				return;
			}
			c->q = nq;
			c->qcap = ncap;
		}
		if (c->qlen + dlen > RCV_CAP) {
			vlog("mux: %u -> %u host overran the window (%zu)", hport, dport, c->qlen + dlen);
		}
		memcpy(c->q + c->qlen, data, dlen);
		c->qlen += dlen;
		c->rcv_nxt += (uint32_t)dlen;
		if (!conn_flush(c)) {
			conn_reset(c, "socket write error");
			pthread_mutex_unlock(&mx);
			return;
		}
		maybe_ack(c);
	}
	wake();                 /* window may have opened, or queue needs POLLOUT */
	pthread_mutex_unlock(&mx);
}

static void
mux_packet(const uint8_t *p, size_t len)
{
	uint32_t proto = get32be(p);
	size_t mh = 8;
	if (proto != MUX_VERSION && len >= MUX_HDR2 && get32be(p + 8) == MUX_MAGIC) {
		mh = MUX_HDR2;
		pthread_mutex_lock(&mx);
		mux_rx_seq = get16be(p + 12);
		pthread_mutex_unlock(&mx);
	}
	const uint8_t *pl = p + mh;
	size_t pll = len - mh;
	switch (proto) {
	case MUX_VERSION: {
		uint32_t major = pll >= 4 ? get32be(pl) : 0;
		uint32_t minor = pll >= 8 ? get32be(pl + 4) : 0;
		pthread_mutex_lock(&mx);
		mux_reset_all(false);           /* (re)started host: forget everything */
		uint8_t vh[12] = { 0 };
		put32be(vh, 2);
		put32be(vh + 4, 0);
		mux_queue(MUX_VERSION, vh, sizeof(vh), NULL, 0);
		mux_version = 2;
		mux_tx_seq = 0;
		pthread_mutex_unlock(&mx);
		logm("usbmux: host version %u.%u", major, minor);
		break;
	}
	case MUX_SETUP:
		vlog("mux: setup (%zu bytes)", pll);
		break;
	case MUX_CONTROL:
		vlog("mux: control from host (%zu bytes)", pll);
		break;
	case MUX_TCP:
		mux_tcp_input(pl, pll);
		break;
	default:
		vlog("mux: unknown protocol %u", proto);
		break;
	}
}

static void *
mux_reader(void *arg)
{
	(void)arg;
	size_t cap = 256 * 1024, have = 0;
	uint8_t *buf = malloc(cap);
	for (;;) {
		if (cap - have < 65536) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		ssize_t r = ep_read(&mux_ep, buf + have, cap - have);
		if (r < 0) {
			if (errno == ECONNRESET || errno == ECANCELED) {
				logm("usbmux: USB reset, dropping all connections");
				pthread_mutex_lock(&mx);
				mux_reset_all(true);
				pthread_mutex_unlock(&mx);
				have = 0;
				continue;
			}
			logm("usbmux: read: %s", strerror(errno));
			sleep(1);
			continue;
		}
		have += (size_t)r;
		size_t off = 0;
		while (have - off >= 8) {
			uint32_t l = get32be(buf + off + 4);
			if (l < 8 || l > MUX_MAX_RX) {
				logm("usbmux: bad packet length %u, resyncing", l);
				off = have;
				break;
			}
			if (have - off < l) {
				if (l > cap) {
					cap = l + 65536;
					uint8_t *nb = realloc(buf, cap);
					if (!nb) {
						off = have;
						break;
					}
					buf = nb;
				}
				break;
			}
			mux_packet(buf + off, l);
			off += l;
		}
		memmove(buf, buf + off, have - off);
		have -= off;
	}
	return NULL;
}

static void *
mux_writer(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&mx);
		while (!txhead) {
			pthread_cond_wait(&txcond, &mx);
		}
		struct txpkt *p = txhead;
		txhead = p->next;
		if (!txhead) {
			txtail = NULL;
		}
		pthread_mutex_unlock(&mx);
		int r = ep_write(&mux_ep, p->data, p->len);
		if (r < 0 && r != -ENXIO) {
			vlog("usbmux: write: %s", strerror(-r));
		}
		pthread_mutex_lock(&mx);
		txbytes = txbytes >= p->len ? txbytes - p->len : 0;
		if (txbytes < TXQ_LIMIT) {
			wake();
		}
		pthread_mutex_unlock(&mx);
		free(p);
	}
	return NULL;
}

/* Bytes we may send now on c. Caller holds mx. */
static size_t
conn_sendable(struct mconn *c)
{
	int32_t inflight = (int32_t)(c->snd_nxt - c->host_ack);
	if (inflight < 0) {
		inflight = 0;
	}
	if ((uint32_t)inflight >= c->host_win) {
		return 0;
	}
	return c->host_win - (uint32_t)inflight;
}

static void *
mux_poller(void *arg)
{
	(void)arg;
	struct pollfd pf[MAX_CONNS + 1];
	int idx[MAX_CONNS + 1];
	uint32_t ids[MAX_CONNS + 1];
	static uint8_t rbuf[MUX_MAX_TX];
	for (;;) {
		int n = 0;
		pf[n].fd = wake_pipe[0];
		pf[n].events = POLLIN;
		n++;
		pthread_mutex_lock(&mx);
		for (int i = 0; i < MAX_CONNS; i++) {
			struct mconn *c = &conns[i];
			if (!c->used) {
				continue;
			}
			short ev = 0;
			if (!c->closing && mux_version && conn_sendable(c) > 0 && txbytes < TXQ_LIMIT) {
				ev |= POLLIN;
			}
			if (c->qlen) {
				ev |= POLLOUT;
			}
			if (!ev) {
				continue;
			}
			pf[n].fd = c->fd;
			pf[n].events = ev;
			idx[n] = i;
			ids[n] = c->id;
			n++;
		}
		pthread_mutex_unlock(&mx);
		int r = poll(pf, (nfds_t)n, 1000);
		if (r < 0) {
			continue;
		}
		if (pf[0].revents) {
			char drain[64];
			(void)read(wake_pipe[0], drain, sizeof(drain));
		}
		pthread_mutex_lock(&mx);
		for (int k = 1; k < n; k++) {
			struct mconn *c = &conns[idx[k]];
			if (!pf[k].revents || !c->used || c->id != ids[k]) {
				continue;
			}
			if (pf[k].revents & (POLLOUT | POLLERR | POLLHUP)) {
				if (c->qlen && !conn_flush(c)) {
					conn_reset(c, "socket write error");
					continue;
				}
				if (c->closing && c->qlen == 0) {
					conn_free(c);
					continue;
				}
				if (!c->closing) {
					maybe_ack(c);
				}
			}
			if ((pf[k].revents & (POLLIN | POLLHUP | POLLERR)) && !c->closing) {
				size_t want = conn_sendable(c);
				if (want == 0) {
					continue;
				}
				size_t maxp = MUX_MAX_TX - MUX_HDR2 - TCP_HDR;
				if (want > maxp) {
					want = maxp;
				}
				ssize_t got = read(c->fd, rbuf, want);
				if (got > 0) {
					size_t g = (size_t)got;
					/* never a multiple of 512 on the wire: keep the last byte for later */
					if (((MUX_HDR2 + TCP_HDR + g) % 512) == 0) {
						/* cannot un-read; send in two packets instead */
						tcp_send(c, TH_ACK, rbuf, g - 1);
						tcp_send(c, TH_ACK, rbuf + g - 1, 1);
					} else {
						tcp_send(c, TH_ACK, rbuf, g);
					}
				} else if (got == 0) {
					conn_reset(c, "service closed");
				} else if (errno != EAGAIN && errno != EINTR) {
					conn_reset(c, strerror(errno));
				}
			}
		}
		pthread_mutex_unlock(&mx);
	}
	return NULL;
}

/* ====================================================================== PTP */

/* container types */
#define PTP_CMD   1
#define PTP_DATA  2
#define PTP_RESP  3
#define PTP_EVENT 4

/* operations */
#define OP_GetDeviceInfo          0x1001
#define OP_OpenSession            0x1002
#define OP_CloseSession           0x1003
#define OP_GetStorageIDs          0x1004
#define OP_GetStorageInfo         0x1005
#define OP_GetNumObjects          0x1006
#define OP_GetObjectHandles       0x1007
#define OP_GetObjectInfo          0x1008
#define OP_GetObject              0x1009
#define OP_GetThumb               0x100A
#define OP_DeleteObject           0x100B
#define OP_SendObjectInfo         0x100C
#define OP_SendObject             0x100D
#define OP_GetDevicePropDesc      0x1014
#define OP_GetDevicePropValue     0x1015
#define OP_GetPartialObject       0x101B
#define OP_GetObjectPropsSupported 0x9801
#define OP_GetObjectPropDesc      0x9802
#define OP_GetObjectPropValue     0x9803
#define OP_GetObjectPropList      0x9805

/* responses */
#define RC_OK                     0x2001
#define RC_GeneralError           0x2002
#define RC_SessionNotOpen         0x2003
#define RC_OperationNotSupported  0x2005
#define RC_IncompleteTransfer     0x2007
#define RC_InvalidStorageID       0x2008
#define RC_InvalidObjectHandle    0x2009
#define RC_DevicePropNotSupported 0x200A
#define RC_StoreFull              0x200C
#define RC_AccessDenied           0x200F
#define RC_NoThumbnailPresent     0x2010
#define RC_NoValidObjectInfo      0x2015
#define RC_InvalidParentObject    0x201A
#define RC_InvalidParameter       0x201D
#define RC_InvalidObjectPropCode  0xA801

/* formats */
#define FMT_UNDEFINED   0x3000
#define FMT_ASSOCIATION 0x3001
#define FMT_TEXT        0x3004
#define FMT_MP3         0x3009
#define FMT_AVI         0x300A
#define FMT_MOV         0x300D
#define FMT_JPEG        0x3801
#define FMT_BMP         0x3804
#define FMT_GIF         0x3807
#define FMT_PNG         0x380B
#define FMT_TIFF        0x380D
#define FMT_MP4         0xB982
#define FMT_HEIF        FMT_UNDEFINED   /* PTP has no HEIF code; hosts go by the extension */

/* properties */
#define DP_BatteryLevel        0x5001
#define DP_DeviceFriendlyName  0xD402
#define OPC_StorageID          0xDC01
#define OPC_ObjectFormat       0xDC02
#define OPC_ProtectionStatus   0xDC03
#define OPC_ObjectSize         0xDC04
#define OPC_ObjectFileName     0xDC07
#define OPC_DateCreated        0xDC08
#define OPC_DateModified       0xDC09
#define OPC_ParentObject       0xDC0B
#define OPC_PersistentUID      0xDC41
#define OPC_Name               0xDC44

#define T_UINT8   0x0002
#define T_UINT16  0x0004
#define T_UINT32  0x0006
#define T_UINT64  0x0008
#define T_UINT128 0x000A
#define T_STR     0xFFFF

#define STORAGE_ID 0x00010001u
#define PTP_CHUNK  65536

/* ---- PTP reader thread: queue of OUT transfers and reset/cancel markers */

struct rxchunk {
	struct rxchunk *next;
	int err;                /* 0 data, ECONNRESET, ECANCELED */
	size_t len, off;
	uint8_t data[];
};

static pthread_mutex_t rxm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rxc = PTHREAD_COND_INITIALIZER;
static struct rxchunk *rxhead, *rxtail;
static size_t rxbytes;
static volatile int ptp_abort;  /* a reset/cancel marker is queued: stop sending */

static void *
ptp_reader(void *arg)
{
	(void)arg;
	static uint8_t buf[PTP_CHUNK];
	for (;;) {
		ssize_t r = ep_read(&ptp_ep, buf, sizeof(buf));
		int err = 0;
		if (r < 0) {
			if (errno != ECONNRESET && errno != ECANCELED) {
				logm("ptp: read: %s", strerror(errno));
				sleep(1);
				continue;
			}
			err = errno;
			r = 0;
		}
		struct rxchunk *ch = malloc(sizeof(*ch) + (size_t)r);
		if (!ch) {
			continue;
		}
		ch->next = NULL;
		ch->err = err;
		ch->len = (size_t)r;
		ch->off = 0;
		memcpy(ch->data, buf, (size_t)r);
		pthread_mutex_lock(&rxm);
		/* bounded queue for long SendObject phases (markers always go in) */
		while (!err && rxbytes > 8 * 1024 * 1024) {
			pthread_cond_wait(&rxc, &rxm);
		}
		if (err) {
			ptp_abort = err;
		}
		if (rxtail) {
			rxtail->next = ch;
		} else {
			rxhead = ch;
		}
		rxtail = ch;
		rxbytes += ch->len;
		pthread_cond_broadcast(&rxc);
		pthread_mutex_unlock(&rxm);
	}
	return NULL;
}

/* Read exactly n bytes of the OUT stream. 0, or -ECONNRESET/-ECANCELED (stream marker consumed). */
static int
rx_get(void *dst, size_t n)
{
	uint8_t *d = dst;
	pthread_mutex_lock(&rxm);
	while (n) {
		while (!rxhead) {
			pthread_cond_wait(&rxc, &rxm);
		}
		struct rxchunk *ch = rxhead;
		if (ch->err) {
			int e = ch->err;
			rxhead = ch->next;
			if (!rxhead) {
				rxtail = NULL;
			}
			free(ch);
			/* clear the abort flag only if no other marker is pending */
			int more = 0;
			for (struct rxchunk *q = rxhead; q; q = q->next) {
				if (q->err) {
					more = q->err;
				}
			}
			ptp_abort = more;
			pthread_mutex_unlock(&rxm);
			return -e;
		}
		size_t k = ch->len - ch->off;
		if (k > n) {
			k = n;
		}
		if (d) {
			memcpy(d, ch->data + ch->off, k);
			d += k;
		}
		ch->off += k;
		n -= k;
		rxbytes -= k;
		if (ch->off == ch->len) {
			rxhead = ch->next;
			if (!rxhead) {
				rxtail = NULL;
			}
			free(ch);
			pthread_cond_broadcast(&rxc);
		}
	}
	pthread_mutex_unlock(&rxm);
	return 0;
}

/* Drop the rest of the transfer currently being read (after a bad container). */
static void
rx_drop_partial(void)
{
	pthread_mutex_lock(&rxm);
	if (rxhead && !rxhead->err && rxhead->off) {
		struct rxchunk *ch = rxhead;
		rxbytes -= ch->len - ch->off;
		rxhead = ch->next;
		if (!rxhead) {
			rxtail = NULL;
		}
		free(ch);
		pthread_cond_broadcast(&rxc);
	}
	pthread_mutex_unlock(&rxm);
}

/* ---- little-endian dataset builder */

struct buf {
	uint8_t *p;
	size_t len, cap;
};

static void
b_need(struct buf *b, size_t n)
{
	if (b->len + n > b->cap) {
		size_t c = b->cap ? b->cap : 512;
		while (c < b->len + n) {
			c *= 2;
		}
		b->p = realloc(b->p, c);
		b->cap = c;
	}
}
static void
b_u8(struct buf *b, uint8_t v)
{
	b_need(b, 1);
	b->p[b->len++] = v;
}
static void
b_u16(struct buf *b, uint16_t v)
{
	b_need(b, 2);
	b->p[b->len++] = (uint8_t)v;
	b->p[b->len++] = (uint8_t)(v >> 8);
}
static void
b_u32(struct buf *b, uint32_t v)
{
	b_u16(b, (uint16_t)v);
	b_u16(b, (uint16_t)(v >> 16));
}
static void
b_u64(struct buf *b, uint64_t v)
{
	b_u32(b, (uint32_t)v);
	b_u32(b, (uint32_t)(v >> 32));
}
static void
b_u16arr(struct buf *b, const uint16_t *a, size_t n)
{
	b_u32(b, (uint32_t)n);
	for (size_t i = 0; i < n; i++) {
		b_u16(b, a[i]);
	}
}
/* PTP string: u8 count of UTF-16 units incl. the NUL, UTF-16LE, NUL; "" = one 0 byte. */
static void
b_str(struct buf *b, const char *s)
{
	uint16_t u[256];
	size_t n = 0;
	const uint8_t *p = (const uint8_t *)s;
	while (*p && n < 253) {
		uint32_t cp;
		if (*p < 0x80) {
			cp = *p++;
		} else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
			cp = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
			p += 2;
		} else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
			cp = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
			p += 3;
		} else if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
		    (p[3] & 0xc0) == 0x80) {
			cp = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3f) << 12 |
			    (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
			p += 4;
		} else {
			cp = 0xfffd;
			p++;
		}
		if (cp >= 0x10000) {
			cp -= 0x10000;
			u[n++] = (uint16_t)(0xd800 | (cp >> 10));
			u[n++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
		} else {
			u[n++] = (uint16_t)cp;
		}
	}
	if (n == 0) {
		b_u8(b, 0);
		return;
	}
	b_u8(b, (uint8_t)(n + 1));
	for (size_t i = 0; i < n; i++) {
		b_u16(b, u[i]);
	}
	b_u16(b, 0);
}

/* Parse a PTP string at *off of p[len] into UTF-8. */
static bool
get_str(const uint8_t *p, size_t len, size_t *off, char *out, size_t outsz)
{
	if (*off >= len) {
		return false;
	}
	size_t n = p[(*off)++];
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		if (*off + 2 > len) {
			return false;
		}
		uint32_t c = (uint32_t)p[*off] | (uint32_t)p[*off + 1] << 8;
		*off += 2;
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && *off + 2 <= len) {
			uint32_t c2 = (uint32_t)p[*off] | (uint32_t)p[*off + 1] << 8;
			if (c2 >= 0xdc00 && c2 < 0xe000) {
				*off += 2;
				i++;
				c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
			}
		}
		if (c == 0) {
			continue;
		}
		uint8_t enc[4];
		size_t el;
		if (c < 0x80) {
			enc[0] = (uint8_t)c;
			el = 1;
		} else if (c < 0x800) {
			enc[0] = (uint8_t)(0xc0 | c >> 6);
			enc[1] = (uint8_t)(0x80 | (c & 0x3f));
			el = 2;
		} else if (c < 0x10000) {
			enc[0] = (uint8_t)(0xe0 | c >> 12);
			enc[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
			enc[2] = (uint8_t)(0x80 | (c & 0x3f));
			el = 3;
		} else {
			enc[0] = (uint8_t)(0xf0 | c >> 18);
			enc[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
			enc[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
			enc[3] = (uint8_t)(0x80 | (c & 0x3f));
			el = 4;
		}
		if (o + el + 1 > outsz) {
			return false;
		}
		memcpy(out + o, enc, el);
		o += el;
	}
	out[o] = 0;
	return true;
}

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- object table */

struct obj {
	char *path;             /* relative to storage_root, no leading '/' */
	const char *name;       /* points into path */
	uint32_t parent;        /* handle, 0 = storage root */
	bool dir, listed, gone;
};

static struct obj *objs;        /* handle h = objs[h - 1] */
static uint32_t nobjs, capobjs;
static uint32_t *htab;          /* open addressing: path hash -> handle */
static uint32_t htcap;
static bool session_open;
static uint32_t session_id;

static uint32_t
hash_str(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) {
		h = (h ^ (uint8_t)*s++) * 16777619u;
	}
	return h;
}

static void
objs_reset(void)
{
	for (uint32_t i = 0; i < nobjs; i++) {
		free(objs[i].path);
	}
	nobjs = 0;
	if (htab) {
		memset(htab, 0, htcap * sizeof(*htab));
	}
}

static void
ht_insert(uint32_t h)
{
	uint32_t m = htcap - 1, i = hash_str(objs[h - 1].path) & m;
	while (htab[i]) {
		i = (i + 1) & m;
	}
	htab[i] = h;
}

static uint32_t
ht_find(const char *path)
{
	if (!htcap) {
		return 0;
	}
	uint32_t m = htcap - 1, i = hash_str(path) & m;
	while (htab[i]) {
		if (strcmp(objs[htab[i] - 1].path, path) == 0) {
			return htab[i];
		}
		i = (i + 1) & m;
	}
	return 0;
}

static uint32_t
obj_add(uint32_t parent, const char *rel, bool dir)
{
	uint32_t h = ht_find(rel);
	if (h) {
		objs[h - 1].gone = false;
		objs[h - 1].dir = dir;
		return h;
	}
	if (nobjs == capobjs) {
		capobjs = capobjs ? capobjs * 2 : 1024;
		objs = realloc(objs, capobjs * sizeof(*objs));
	}
	if ((nobjs + 1) * 2 > htcap) {
		free(htab);
		htcap = htcap ? htcap * 2 : 4096;
		while ((nobjs + 1) * 2 > htcap) {
			htcap *= 2;
		}
		htab = calloc(htcap, sizeof(*htab));
		for (uint32_t i = 1; i <= nobjs; i++) {
			ht_insert(i);
		}
	}
	struct obj *o = &objs[nobjs++];
	o->path = strdup(rel);
	const char *sl = strrchr(o->path, '/');
	o->name = sl ? sl + 1 : o->path;
	o->parent = parent;
	o->dir = dir;
	o->listed = false;
	o->gone = false;
	ht_insert(nobjs);
	return nobjs;
}

static struct obj *
obj_get(uint32_t h)
{
	if (h == 0 || h > nobjs || objs[h - 1].gone) {
		return NULL;
	}
	return &objs[h - 1];
}

static void
abs_path(const char *rel, char *out, size_t sz)
{
	if (rel[0]) {
		snprintf(out, sz, "%s/%s", storage_root, rel);
	} else {
		snprintf(out, sz, "%s", storage_root);
	}
}

static bool
name_ok(const char *n)
{
	return n[0] && strcmp(n, ".") && strcmp(n, "..") && !strchr(n, '/') && strlen(n) < 256;
}

/* Enumerate directory `h` (0 = root) once per session; symlinks and specials are skipped. */
static void
obj_list(uint32_t h)
{
	struct obj *d = h ? obj_get(h) : NULL;
	if (h && (!d || !d->dir)) {
		return;
	}
	static bool root_listed;
	if (h == 0 && nobjs == 0) {
		root_listed = false;
	}
	if (h ? d->listed : root_listed) {
		return;
	}
	char dpath[2048];
	char *rel = strdup(h ? d->path : "");
	abs_path(rel, dpath, sizeof(dpath));
	DIR *dir = opendir(dpath);
	if (dir) {
		struct dirent *de;
		while ((de = readdir(dir))) {
			if (!name_ok(de->d_name)) {
				continue;
			}
			char child[2048], cabs[3072];
			snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
			snprintf(cabs, sizeof(cabs), "%s/%s", dpath, de->d_name);
			struct stat st;
			if (lstat(cabs, &st) < 0 || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
				continue;
			}
			obj_add(h, child, S_ISDIR(st.st_mode));
		}
		closedir(dir);
	}
	free(rel);
	if (h) {
		objs[h - 1].listed = true;      /* objs may have moved */
	} else {
		root_listed = true;
	}
}

static int
obj_stat(struct obj *o, struct stat *st)
{
	char p[2048];
	abs_path(o->path, p, sizeof(p));
	if (lstat(p, st) < 0 || !(S_ISREG(st->st_mode) || S_ISDIR(st->st_mode))) {
		o->gone = true;
		return -1;
	}
	return 0;
}

static uint16_t
fmt_of(const char *name, bool dir)
{
	if (dir) {
		return FMT_ASSOCIATION;
	}
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return FMT_UNDEFINED;
	}
	static const struct {
		const char *ext;
		uint16_t f;
	} t[] = {
		{ "jpg", FMT_JPEG }, { "jpeg", FMT_JPEG }, { "heic", FMT_HEIF }, { "heif", FMT_HEIF },
		{ "png", FMT_PNG }, { "gif", FMT_GIF }, { "tif", FMT_TIFF }, { "tiff", FMT_TIFF },
		{ "bmp", FMT_BMP }, { "mov", FMT_MOV }, { "mp4", FMT_MP4 }, { "m4v", FMT_MP4 },
		{ "avi", FMT_AVI }, { "mp3", FMT_MP3 }, { "txt", FMT_TEXT },
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (strcasecmp(dot + 1, t[i].ext) == 0) {
			return t[i].f;
		}
	}
	return FMT_UNDEFINED;
}

static void
date_str(time_t t, char *out, size_t sz)
{
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, sz, "%Y%m%dT%H%M%S", &tm);
}

/* ---- container I/O */

static void
put_hdr(uint8_t *p, uint32_t len, uint16_t type, uint16_t code, uint32_t tid)
{
	p[0] = (uint8_t)len;
	p[1] = (uint8_t)(len >> 8);
	p[2] = (uint8_t)(len >> 16);
	p[3] = (uint8_t)(len >> 24);
	p[4] = (uint8_t)type;
	p[5] = (uint8_t)(type >> 8);
	p[6] = (uint8_t)code;
	p[7] = (uint8_t)(code >> 8);
	p[8] = (uint8_t)tid;
	p[9] = (uint8_t)(tid >> 8);
	p[10] = (uint8_t)(tid >> 16);
	p[11] = (uint8_t)(tid >> 24);
}

static void
ptp_respond(uint16_t rc, uint32_t tid, int np, const uint32_t *params)
{
	uint8_t b[12 + 5 * 4];
	put_hdr(b, (uint32_t)(12 + 4 * np), PTP_RESP, rc, tid);
	for (int i = 0; i < np; i++) {
		b[12 + 4 * i] = (uint8_t)params[i];
		b[13 + 4 * i] = (uint8_t)(params[i] >> 8);
		b[14 + 4 * i] = (uint8_t)(params[i] >> 16);
		b[15 + 4 * i] = (uint8_t)(params[i] >> 24);
	}
	int r = ep_write(&ptp_ep, b, 12 + 4 * (size_t)np);
	if (r < 0) {
		vlog("ptp: response write: %s", strerror(-r));
	}
}

/* Data phase from memory. false = aborted (reset/cancel, or write error). */
static bool
ptp_send_buf(uint16_t code, uint32_t tid, const uint8_t *d, size_t len)
{
	static uint8_t chunk[PTP_CHUNK];
	size_t total = 12 + len, sent = 0;
	put_hdr(chunk, total > 0xffffffffu ? 0xffffffffu : (uint32_t)total, PTP_DATA, code, tid);
	size_t fill = 12;
	while (sent < total) {
		size_t k = PTP_CHUNK - fill;
		size_t dleft = len - (sent + fill - 12);
		if (k > dleft) {
			k = dleft;
		}
		memcpy(chunk + fill, d + (sent + fill - 12), k);
		fill += k;
		if (ptp_abort) {
			return false;
		}
		int r = ep_write(&ptp_ep, chunk, fill);
		if (r < 0) {
			vlog("ptp: data write: %s", strerror(-r));
			return false;
		}
		sent += fill;
		fill = 0;
	}
	return true;
}

/* Data phase from a file range. */
static bool
ptp_send_file(uint16_t code, uint32_t tid, int fd, off_t off, uint64_t len)
{
	static uint8_t chunk[PTP_CHUNK];
	uint64_t total = 12 + len, sent = 0;
	put_hdr(chunk, total > 0xffffffffu ? 0xffffffffu : (uint32_t)total, PTP_DATA, code, tid);
	size_t fill = 12;
	uint64_t fpos = 0;
	while (sent < total) {
		size_t k = PTP_CHUNK - fill;
		if (k > len - fpos) {
			k = (size_t)(len - fpos);
		}
		size_t got = 0;
		while (got < k) {
			ssize_t r = pread(fd, chunk + fill + got, k - got, off + (off_t)(fpos + got));
			if (r < 0 && errno == EINTR) {
				continue;
			}
			if (r <= 0) {
				/* file shrank: pad with zeros so the container stays well-formed */
				memset(chunk + fill + got, 0, k - got);
				got = k;
				break;
			}
			got += (size_t)r;
		}
		fill += k;
		fpos += k;
		if (ptp_abort) {
			return false;
		}
		int r = ep_write(&ptp_ep, chunk, fill);
		if (r < 0) {
			vlog("ptp: data write: %s", strerror(-r));
			return false;
		}
		sent += fill;
		fill = 0;
	}
	return true;
}

/*
 * Data phase from the host. With sink_fd >= 0 the payload is streamed into it,
 * otherwise collected in *out (limit 1 MiB). Returns 0, RC_* on a bad phase, or
 * -ECONNRESET/-ECANCELED.
 */
static int
ptp_recv_data(uint32_t tid, int sink_fd, struct buf *out, uint64_t *got_len)
{
	uint8_t h[12];
	int r = rx_get(h, 12);
	if (r) {
		return r;
	}
	uint32_t len = le32(h);
	uint16_t type = le16(h + 4);
	(void)tid;
	if (type != PTP_DATA || len < 12) {
		/* not a data phase: the host skipped it; drop the rest of that container */
		if (len > 12 && len < 4096) {
			rx_get(NULL, len - 12);
		} else {
			rx_drop_partial();
		}
		return RC_IncompleteTransfer;
	}
	uint64_t left = len - 12;
	*got_len = left;
	static uint8_t tmp[PTP_CHUNK];
	bool werr = false;
	while (left) {
		size_t k = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
		r = rx_get(tmp, k);
		if (r) {
			return r;
		}
		if (sink_fd >= 0) {
			if (!werr && !writen(sink_fd, tmp, k)) {
				werr = true;
			}
		} else if (out) {
			if (out->len + k <= (1u << 20)) {
				b_need(out, k);
				memcpy(out->p + out->len, tmp, k);
				out->len += k;
			}
		}
		left -= k;
	}
	return werr ? RC_StoreFull : 0;
}

/* ---- datasets */

static const uint16_t ops_supported[] = {
	OP_GetDeviceInfo, OP_OpenSession, OP_CloseSession, OP_GetStorageIDs, OP_GetStorageInfo,
	OP_GetNumObjects, OP_GetObjectHandles, OP_GetObjectInfo, OP_GetObject, OP_GetThumb,
	OP_DeleteObject, OP_SendObjectInfo, OP_SendObject, OP_GetDevicePropDesc, OP_GetDevicePropValue,
	OP_GetPartialObject, OP_GetObjectPropsSupported, OP_GetObjectPropDesc, OP_GetObjectPropValue,
	OP_GetObjectPropList,
};
static const uint16_t events_supported[] = { 0x4002 /* ObjectAdded */, 0x4003 /* ObjectRemoved */ };
static const uint16_t devprops_supported[] = { DP_BatteryLevel, DP_DeviceFriendlyName };
static const uint16_t formats_supported[] = {
	FMT_UNDEFINED, FMT_ASSOCIATION, FMT_JPEG, FMT_PNG, FMT_GIF, FMT_TIFF, FMT_BMP, FMT_MOV, FMT_MP4,
	FMT_AVI, FMT_MP3, FMT_TEXT,
};
static const uint16_t objprops_supported[] = {
	OPC_StorageID, OPC_ObjectFormat, OPC_ProtectionStatus, OPC_ObjectSize, OPC_ObjectFileName,
	OPC_DateCreated, OPC_DateModified, OPC_ParentObject, OPC_PersistentUID, OPC_Name,
};

static void
ds_deviceinfo(struct buf *b)
{
	b_u16(b, 100);                          /* StandardVersion */
	b_u32(b, 6);                            /* VendorExtensionID: Microsoft (MTP) */
	b_u16(b, 100);
	b_str(b, "microsoft.com: 1.0;");
	b_u16(b, 0);                            /* FunctionalMode */
	b_u16arr(b, ops_supported, sizeof(ops_supported) / 2);
	b_u16arr(b, events_supported, sizeof(events_supported) / 2);
	b_u16arr(b, devprops_supported, sizeof(devprops_supported) / 2);
	b_u16arr(b, NULL, 0);                   /* CaptureFormats */
	b_u16arr(b, formats_supported, sizeof(formats_supported) / 2);
	b_str(b, "Apple Inc.");
	b_str(b, "iPhone");
	b_str(b, "16.7");
	b_str(b, udid);
}

static int
ds_storageinfo(struct buf *b)
{
	struct statfs sf;
	if (statfs(storage_root, &sf) < 0) {
		return RC_GeneralError;
	}
	b_u16(b, 0x0003);                       /* FixedRAM */
	b_u16(b, 0x0002);                       /* GenericHierarchical */
	b_u16(b, 0x0000);                       /* ReadWrite */
	b_u64(b, (uint64_t)sf.f_blocks * sf.f_bsize);
	b_u64(b, (uint64_t)sf.f_bavail * sf.f_bsize);
	b_u32(b, 0xffffffffu);
	b_str(b, "iPhone");
	b_str(b, "iPhone");
	return 0;
}

static int
ds_objectinfo(struct buf *b, uint32_t h)
{
	struct obj *o = obj_get(h);
	struct stat st;
	if (!o || obj_stat(o, &st) < 0) {
		return RC_InvalidObjectHandle;
	}
	char cd[32], md[32];
	date_str(st.st_birthtimespec.tv_sec ? st.st_birthtimespec.tv_sec : st.st_mtime, cd, sizeof(cd));
	date_str(st.st_mtime, md, sizeof(md));
	uint16_t f = fmt_of(o->name, o->dir);
	uint64_t sz = o->dir ? 0 : (uint64_t)st.st_size;
	b_u32(b, STORAGE_ID);
	b_u16(b, f);
	b_u16(b, 0);                            /* ProtectionStatus */
	b_u32(b, sz > 0xffffffffu ? 0xffffffffu : (uint32_t)sz);
	b_u16(b, 0);                            /* ThumbFormat: none */
	b_u32(b, 0);
	b_u32(b, 0);
	b_u32(b, 0);
	b_u32(b, 0);                            /* ImagePixWidth/Height/BitDepth unknown */
	b_u32(b, 0);
	b_u32(b, 0);
	b_u32(b, o->parent);                    /* 0 = storage root */
	b_u16(b, o->dir ? 0x0001 : 0x0000);     /* GenericFolder */
	b_u32(b, 0);
	b_u32(b, 0);                            /* SequenceNumber */
	b_str(b, o->name);
	b_str(b, cd);
	b_str(b, md);
	b_str(b, "");
	return 0;
}

static int
prop_type(uint16_t p)
{
	switch (p) {
	case OPC_StorageID:
	case OPC_ParentObject:
		return T_UINT32;
	case OPC_ObjectFormat:
	case OPC_ProtectionStatus:
		return T_UINT16;
	case OPC_ObjectSize:
		return T_UINT64;
	case OPC_PersistentUID:
		return T_UINT128;
	case OPC_ObjectFileName:
	case OPC_DateCreated:
	case OPC_DateModified:
	case OPC_Name:
		return T_STR;
	}
	return 0;
}

static int
prop_value(struct buf *b, uint32_t h, uint16_t p)
{
	struct obj *o = obj_get(h);
	struct stat st;
	if (!o || obj_stat(o, &st) < 0) {
		return RC_InvalidObjectHandle;
	}
	char ds[32];
	switch (p) {
	case OPC_StorageID:
		b_u32(b, STORAGE_ID);
		break;
	case OPC_ParentObject:
		b_u32(b, o->parent);
		break;
	case OPC_ObjectFormat:
		b_u16(b, fmt_of(o->name, o->dir));
		break;
	case OPC_ProtectionStatus:
		b_u16(b, 0);
		break;
	case OPC_ObjectSize:
		b_u64(b, o->dir ? 0 : (uint64_t)st.st_size);
		break;
	case OPC_PersistentUID:
		b_u64(b, (uint64_t)st.st_ino);
		b_u64(b, (uint64_t)st.st_dev << 32 | h);
		break;
	case OPC_ObjectFileName:
	case OPC_Name:
		b_str(b, o->name);
		break;
	case OPC_DateCreated:
		date_str(st.st_birthtimespec.tv_sec ? st.st_birthtimespec.tv_sec : st.st_mtime, ds, sizeof(ds));
		b_str(b, ds);
		break;
	case OPC_DateModified:
		date_str(st.st_mtime, ds, sizeof(ds));
		b_str(b, ds);
		break;
	default:
		return RC_InvalidObjectPropCode;
	}
	return 0;
}

static void
collect_handles(uint32_t parent, bool recursive, uint32_t fmt, uint32_t **out, uint32_t *n, uint32_t *cap)
{
	obj_list(parent);
	uint32_t count = nobjs;  /* children may be appended while recursing */
	for (uint32_t i = 0; i < count; i++) {
		if (objs[i].gone || objs[i].parent != parent) {
			continue;
		}
		uint32_t h = i + 1;
		if (!fmt || fmt == 0xffffffffu || fmt_of(objs[i].name, objs[i].dir) == fmt) {
			if (*n == *cap) {
				*cap = *cap ? *cap * 2 : 256;
				*out = realloc(*out, *cap * sizeof(**out));
			}
			(*out)[(*n)++] = h;
		}
		if (recursive && objs[i].dir) {
			collect_handles(h, true, fmt, out, n, cap);
			count = count < nobjs ? count : nobjs;
		}
	}
}

/* parent param: 0xffffffff = root level, 0 = whole storage (recursive), else that folder. */
static int
handles_for(uint32_t storage, uint32_t fmt, uint32_t parent, uint32_t **out, uint32_t *n)
{
	uint32_t cap = 0;
	*out = NULL;
	*n = 0;
	if (storage != 0xffffffffu && storage != STORAGE_ID) {
		return RC_InvalidStorageID;
	}
	if (parent == 0xffffffffu) {
		collect_handles(0, false, fmt, out, n, &cap);
	} else if (parent == 0) {
		collect_handles(0, true, fmt, out, n, &cap);
	} else {
		struct obj *o = obj_get(parent);
		if (!o) {
			return RC_InvalidObjectHandle;
		}
		if (!o->dir) {
			return RC_InvalidParentObject;
		}
		collect_handles(parent, false, fmt, out, n, &cap);
	}
	return 0;
}

/* ---- deletion (no symlink is ever followed: entries are lstat'ed, links unlinked) */

static int
rm_tree(const char *p)
{
	struct stat st;
	if (lstat(p, &st) < 0) {
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		return unlink(p);
	}
	DIR *d = opendir(p);
	if (!d) {
		return -1;
	}
	struct dirent *de;
	int ret = 0;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
			continue;
		}
		char c[2048];
		snprintf(c, sizeof(c), "%s/%s", p, de->d_name);
		if (rm_tree(c) < 0) {
			ret = -1;
		}
	}
	closedir(d);
	return rmdir(p) < 0 ? -1 : ret;
}

static void
mark_gone(uint32_t h)
{
	objs[h - 1].gone = true;
	for (uint32_t i = 0; i < nobjs; i++) {
		if (!objs[i].gone && objs[i].parent == h) {
			mark_gone(i + 1);
		}
	}
}

/* ---- SendObjectInfo state */

static uint32_t pend_handle;    /* object announced by SendObjectInfo, waiting for SendObject */
static uint64_t pend_size;

/* ---- battery: CurrentCapacity/MaxCapacity of the IOPMPowerSource (VivaBattery), via dlopen'ed IOKit */

static int
battery_level(void)
{
	static void *iok, *cf;
	if (!iok) {
		iok = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
		cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
	}
	if (!iok || !cf) {
		return 100;
	}
	void *(*matching)(const char *) = (void *(*)(const char *))dlsym(iok, "IOServiceMatching");
	unsigned (*getsvc)(unsigned, void *) = (unsigned (*)(unsigned, void *))dlsym(iok, "IOServiceGetMatchingService");
	CFTypeRef (*getprop)(unsigned, CFStringRef, const void *, unsigned) =
	    (CFTypeRef (*)(unsigned, CFStringRef, const void *, unsigned))dlsym(iok, "IORegistryEntryCreateCFProperty");
	int (*objrel)(unsigned) = (int (*)(unsigned))dlsym(iok, "IOObjectRelease");
	CFStringRef (*mkstr)(const void *, const char *, uint32_t) =
	    (CFStringRef (*)(const void *, const char *, uint32_t))dlsym(cf, "CFStringCreateWithCString");
	bool (*numval)(CFTypeRef, long, void *) = (bool (*)(CFTypeRef, long, void *))dlsym(cf, "CFNumberGetValue");
	unsigned long (*typeid_of)(CFTypeRef) = (unsigned long (*)(CFTypeRef))dlsym(cf, "CFGetTypeID");
	unsigned long (*numtype)(void) = (unsigned long (*)(void))dlsym(cf, "CFNumberGetTypeID");
	void (*rel)(CFTypeRef) = (void (*)(CFTypeRef))dlsym(cf, "CFRelease");
	if (!matching || !getsvc || !getprop || !objrel || !mkstr || !numval || !typeid_of || !numtype || !rel) {
		return 100;
	}
	unsigned svc = getsvc(0, matching("IOPMPowerSource"));   /* consumes the matching dict */
	if (!svc) {
		return 100;
	}
	int v[2] = { -1, -1 };
	const char *keys[2] = { "CurrentCapacity", "MaxCapacity" };
	for (int i = 0; i < 2; i++) {
		CFStringRef k = mkstr(NULL, keys[i], 0x08000100);
		CFTypeRef n = k ? getprop(svc, k, NULL, 0) : NULL;
		if (n) {
			if (typeid_of(n) == numtype()) {
				numval(n, 9 /* kCFNumberIntType */, &v[i]);
			}
			rel(n);
		}
		if (k) {
			rel(k);
		}
	}
	objrel(svc);
	if (v[0] < 0) {
		return 100;
	}
	int pct = v[1] > 0 ? v[0] * 100 / v[1] : v[0];
	return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

/* ---- dispatcher */

static void
ptp_session_reset(void)
{
	session_open = false;
	pend_handle = 0;
	objs_reset();
}

static void
ptp_command(uint16_t op, uint32_t tid, const uint32_t *p, int np)
{
	struct buf b = { 0 };
	uint32_t rp[5];
	int nrp = 0;
	int rc = RC_OK;
	char path[2048];
	(void)np;

	switch (op) {
	case OP_GetDeviceInfo:
		ds_deviceinfo(&b);
		goto send_data;

	case OP_OpenSession:
		/* lenient: re-opening (host restarted) starts a fresh session */
		ptp_session_reset();
		session_open = true;
		session_id = p[0];
		vlog("ptp: session %u", session_id);
		break;

	case OP_CloseSession:
		ptp_session_reset();
		break;

	case OP_GetStorageIDs:
		b_u32(&b, 1);
		b_u32(&b, STORAGE_ID);
		goto send_data;

	case OP_GetStorageInfo:
		if (p[0] != STORAGE_ID) {
			rc = RC_InvalidStorageID;
			break;
		}
		if ((rc = ds_storageinfo(&b)) != 0) {
			break;
		}
		goto send_data;

	case OP_GetNumObjects:
	case OP_GetObjectHandles: {
		uint32_t *hs, n;
		if ((rc = handles_for(p[0], p[1], p[2], &hs, &n)) != 0) {
			break;
		}
		if (op == OP_GetNumObjects) {
			rp[0] = n;
			nrp = 1;
			free(hs);
			rc = RC_OK;
			break;
		}
		b_u32(&b, n);
		for (uint32_t i = 0; i < n; i++) {
			b_u32(&b, hs[i]);
		}
		free(hs);
		goto send_data;
	}

	case OP_GetObjectInfo:
		if ((rc = ds_objectinfo(&b, p[0])) != 0) {
			break;
		}
		goto send_data;

	case OP_GetObject:
	case OP_GetPartialObject: {
		struct obj *o = obj_get(p[0]);
		struct stat st;
		if (!o || obj_stat(o, &st) < 0 || o->dir) {
			rc = RC_InvalidObjectHandle;
			break;
		}
		abs_path(o->path, path, sizeof(path));
		int fd = open(path, O_RDONLY | O_NOFOLLOW);
		if (fd < 0) {
			rc = RC_AccessDenied;
			break;
		}
		uint64_t off = 0, len = (uint64_t)st.st_size;
		if (op == OP_GetPartialObject) {
			off = p[1];
			len = off >= (uint64_t)st.st_size ? 0 : (uint64_t)st.st_size - off;
			if (len > p[2]) {
				len = p[2];
			}
			rp[0] = (uint32_t)len;
			nrp = 1;
		}
		bool ok = ptp_send_file(op, tid, fd, (off_t)off, len);
		close(fd);
		if (!ok) {
			free(b.p);
			return;         /* aborted: no response */
		}
		break;
	}

	case OP_GetThumb:
		rc = obj_get(p[0]) ? RC_NoThumbnailPresent : RC_InvalidObjectHandle;
		break;

	case OP_DeleteObject: {
		struct obj *o = obj_get(p[0]);
		if (p[0] == 0xffffffffu) {
			rc = RC_AccessDenied;   /* "delete everything": refuse */
			break;
		}
		if (!o) {
			rc = RC_InvalidObjectHandle;
			break;
		}
		abs_path(o->path, path, sizeof(path));
		if (rm_tree(path) < 0 && errno != ENOENT) {
			rc = RC_AccessDenied;
		}
		mark_gone(p[0]);
		break;
	}

	case OP_SendObjectInfo: {
		struct buf in = { 0 };
		uint64_t dl = 0;
		int r = ptp_recv_data(tid, -1, &in, &dl);
		if (r < 0) {
			free(in.p);
			return;
		}
		if (r) {
			rc = r;
			free(in.p);
			break;
		}
		pend_handle = 0;
		uint32_t storage = p[0], parent = p[1];
		if (storage != 0 && storage != 0xffffffffu && storage != STORAGE_ID) {
			rc = RC_InvalidStorageID;
			free(in.p);
			break;
		}
		if (parent == 0xffffffffu) {
			parent = 0;
		}
		struct obj *po = parent ? obj_get(parent) : NULL;
		if (parent && (!po || !po->dir)) {
			rc = RC_InvalidParentObject;
			free(in.p);
			break;
		}
		if (in.len < 52) {
			rc = RC_NoValidObjectInfo;
			free(in.p);
			break;
		}
		uint16_t fmt = le16(in.p + 4);
		uint32_t csize = le32(in.p + 8);
		size_t off = 52;
		char name[1024];
		if (!get_str(in.p, in.len, &off, name, sizeof(name)) || !name_ok(name)) {
			rc = RC_NoValidObjectInfo;
			free(in.p);
			break;
		}
		free(in.p);
		obj_list(parent);
		char rel[2048];
		snprintf(rel, sizeof(rel), "%s%s%s", po ? po->path : "", po ? "/" : "", name);
		abs_path(rel, path, sizeof(path));
		bool dir = fmt == FMT_ASSOCIATION;
		struct stat st;
		if (lstat(path, &st) == 0 && (S_ISLNK(st.st_mode) || (dir != S_ISDIR(st.st_mode)))) {
			rc = RC_AccessDenied;
			break;
		}
		if (dir) {
			if (mkdir(path, 0755) < 0 && errno != EEXIST) {
				rc = RC_AccessDenied;
				break;
			}
			(void)lchown(path, own_uid, own_gid);
		}
		uint32_t h = obj_add(parent, rel, dir);
		if (dir) {
			objs[h - 1].listed = false;
		} else {
			pend_handle = h;
			pend_size = csize;
		}
		rp[0] = STORAGE_ID;
		rp[1] = p[1] ? p[1] : 0xffffffffu;
		rp[2] = h;
		nrp = 3;
		vlog("ptp: SendObjectInfo %s (%u bytes) -> %u", rel, csize, h);
		break;
	}

	case OP_SendObject: {
		struct obj *o = pend_handle ? obj_get(pend_handle) : NULL;
		int fd = -1;
		if (o) {
			abs_path(o->path, path, sizeof(path));
			fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
			if (fd >= 0) {
				(void)fchown(fd, own_uid, own_gid);
			}
		}
		uint64_t dl = 0;
		/* the data phase is consumed even when there is nowhere to put it */
		int r = ptp_recv_data(tid, fd >= 0 ? fd : -1, NULL, &dl);
		if (fd >= 0) {
			close(fd);
		}
		if (r < 0) {
			if (o) {
				unlink(path);
				o->gone = true;
			}
			pend_handle = 0;
			return;
		}
		if (!o) {
			rc = RC_NoValidObjectInfo;
		} else if (fd < 0) {
			rc = RC_AccessDenied;
		} else if (r) {
			rc = r;
		}
		if (rc != RC_OK && o) {
			unlink(path);
			o->gone = true;
		}
		vlog("ptp: SendObject %llu bytes -> %s (rc %x, announced %llu)", (unsigned long long)dl,
		    o ? o->path : "-", rc, (unsigned long long)pend_size);
		pend_handle = 0;
		break;
	}

	case OP_GetDevicePropDesc:
	case OP_GetDevicePropValue: {
		bool desc = op == OP_GetDevicePropDesc;
		if (p[0] == DP_DeviceFriendlyName) {
			if (desc) {
				b_u16(&b, DP_DeviceFriendlyName);
				b_u16(&b, T_STR);
				b_u8(&b, 0);    /* get only */
				b_str(&b, "iPhone");
			}
			b_str(&b, "iPhone");
			if (desc) {
				b_u8(&b, 0);
			}
		} else if (p[0] == DP_BatteryLevel) {
			int lvl = battery_level();
			if (desc) {
				b_u16(&b, DP_BatteryLevel);
				b_u16(&b, T_UINT8);
				b_u8(&b, 0);
				b_u8(&b, 100);
			}
			b_u8(&b, (uint8_t)lvl);
			if (desc) {
				b_u8(&b, 1);    /* range form */
				b_u8(&b, 0);
				b_u8(&b, 100);
				b_u8(&b, 1);
			}
		} else {
			rc = RC_DevicePropNotSupported;
			break;
		}
		goto send_data;
	}

	case OP_GetObjectPropsSupported:
		b_u16arr(&b, objprops_supported, sizeof(objprops_supported) / 2);
		goto send_data;

	case OP_GetObjectPropDesc: {
		uint16_t pc = (uint16_t)p[0];
		int t = prop_type(pc);
		if (!t) {
			rc = RC_InvalidObjectPropCode;
			break;
		}
		b_u16(&b, pc);
		b_u16(&b, (uint16_t)t);
		b_u8(&b, 0);            /* get only */
		switch (t) {            /* default value */
		case T_UINT16:
			b_u16(&b, 0);
			break;
		case T_UINT32:
			b_u32(&b, 0);
			break;
		case T_UINT64:
			b_u64(&b, 0);
			break;
		case T_UINT128:
			b_u64(&b, 0);
			b_u64(&b, 0);
			break;
		default:
			b_str(&b, "");
			break;
		}
		b_u32(&b, 0);           /* group code */
		b_u8(&b, pc == OPC_DateCreated || pc == OPC_DateModified ? 0x03 : 0x00);  /* DateTime form */
		goto send_data;
	}

	case OP_GetObjectPropValue:
		if (!prop_type((uint16_t)p[1])) {
			rc = RC_InvalidObjectPropCode;
			break;
		}
		if ((rc = prop_value(&b, p[0], (uint16_t)p[1])) != 0) {
			break;
		}
		goto send_data;

	case OP_GetObjectPropList: {
		/* params: handle, format, prop (0xffffffff all), group, depth */
		uint32_t h = p[0], fmt = p[1], pc = p[2], depth = p[4];
		uint32_t *hs = NULL, n = 0;
		if (pc != 0xffffffffu && !prop_type((uint16_t)pc)) {
			rc = pc == 0 ? 0xA807 /* SpecificationByGroupUnsupported */ : RC_InvalidObjectPropCode;
			break;
		}
		if (depth == 0) {
			if (h == 0 || h == 0xffffffffu) {
				hs = NULL;
				n = 0;  /* the storage root itself has no properties */
			} else if (!obj_get(h)) {
				rc = RC_InvalidObjectHandle;
				break;
			} else {
				hs = malloc(sizeof(*hs));
				hs[0] = h;
				n = 1;
			}
		} else {
			uint32_t parent = (h == 0 || h == 0xffffffffu) ? (depth == 0xffffffffu ? 0 : 0xffffffffu) : h;
			if (depth == 0xffffffffu && h != 0 && h != 0xffffffffu) {
				/* everything below h */
				uint32_t cap = 0;
				if (!obj_get(h)) {
					rc = RC_InvalidObjectHandle;
					break;
				}
				collect_handles(h, true, 0, &hs, &n, &cap);
			} else if ((rc = handles_for(STORAGE_ID, 0, parent, &hs, &n)) != 0) {
				break;
			}
		}
		struct buf body = { 0 };
		uint32_t count = 0;
		for (uint32_t i = 0; i < n; i++) {
			struct obj *o = obj_get(hs[i]);
			if (!o || (fmt && fmt != 0xffffffffu && fmt_of(o->name, o->dir) != fmt)) {
				continue;
			}
			for (size_t k = 0; k < sizeof(objprops_supported) / 2; k++) {
				uint16_t q = objprops_supported[k];
				if (pc != 0xffffffffu && q != pc) {
					continue;
				}
				size_t mark = body.len;
				b_u32(&body, hs[i]);
				b_u16(&body, q);
				b_u16(&body, (uint16_t)prop_type(q));
				if (prop_value(&body, hs[i], q) != 0) {
					body.len = mark;
					break;
				}
				count++;
			}
		}
		free(hs);
		b_u32(&b, count);
		b_need(&b, body.len);
		if (body.len) {
			memcpy(b.p + b.len, body.p, body.len);
		}
		b.len += body.len;
		free(body.p);
		rc = RC_OK;
		goto send_data;
	}

	default:
		vlog("ptp: op %04x not supported", op);
		rc = RC_OperationNotSupported;
		break;
	}
	free(b.p);
	ptp_respond((uint16_t)rc, tid, nrp, rp);
	return;

send_data:
	if (!ptp_send_buf(op, tid, b.p, b.len)) {
		free(b.p);
		return;
	}
	free(b.p);
	ptp_respond(RC_OK, tid, nrp, rp);
}

static void *
ptp_responder(void *arg)
{
	(void)arg;
	for (;;) {
		uint8_t h[12];
		int r = rx_get(h, 12);
		if (r == -ECONNRESET) {
			vlog("ptp: USB reset, session dropped");
			ptp_session_reset();
			continue;
		}
		if (r == -ECANCELED) {
			vlog("ptp: transaction cancelled");
			if (pend_handle) {
				pend_handle = 0;
			}
			continue;
		}
		uint32_t len = le32(h);
		uint16_t type = le16(h + 4), code = le16(h + 6);
		uint32_t tid = le32(h + 8);
		if (len < 12) {
			rx_drop_partial();
			continue;
		}
		if (type != PTP_CMD) {
			/* stray data phase (e.g. of an unsupported op): consume it */
			if (len > 12 && rx_get(NULL, len - 12) < 0) {
				continue;
			}
			if (type == PTP_DATA) {
				vlog("ptp: dropped data phase %04x (%u bytes)", code, len);
			}
			continue;
		}
		uint32_t p[5] = { 0 };
		int np = 0;
		if (len > 12 + 20) {
			rx_drop_partial();
			continue;
		}
		uint8_t pb[20];
		if (len > 12) {
			if (rx_get(pb, len - 12) < 0) {
				continue;
			}
			np = (int)(len - 12) / 4;
			for (int i = 0; i < np; i++) {
				p[i] = le32(pb + 4 * i);
			}
		}
		vlog("ptp: op %04x tid %u p %x %x %x", code, tid, p[0], p[1], p[2]);
		ptp_command(code, tid, p, np);
	}
	return NULL;
}

/* ===================================================================== main */

static void
usage(void)
{
	fprintf(stderr, "usage: vivausbd [-v] [--udid X] [--root DIR] [--map PORT=HOST:PORT|PORT=/sock]..\n"
	    "                [--owner UID:GID] [--no-ptp] [--no-mux] [--test-sock DIR]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *testdir = NULL;
	bool use_ptp = true, use_mux = true;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "-v")) {
			verbose = 1;
		} else if (!strcmp(a, "--udid") && v) {
			strlcpy(udid, v, sizeof(udid));
			i++;
		} else if (!strcmp(a, "--root") && v) {
			strlcpy(storage_root, v, sizeof(storage_root));
			i++;
		} else if (!strcmp(a, "--test-sock") && v) {
			testdir = v;
			i++;
		} else if (!strcmp(a, "--owner") && v) {
			unsigned u, g;
			if (sscanf(v, "%u:%u", &u, &g) != 2) {
				usage();
			}
			own_uid = u;
			own_gid = g;
			i++;
		} else if (!strcmp(a, "--map") && v) {
			const char *eq = strchr(v, '=');
			if (!eq || ntargets == (int)(sizeof(targets) / sizeof(targets[0]))) {
				usage();
			}
			targets[ntargets].port = (uint16_t)atoi(v);
			strlcpy(targets[ntargets].spec, eq + 1, sizeof(targets[ntargets].spec));
			ntargets++;
			i++;
		} else if (!strcmp(a, "--no-ptp")) {
			use_ptp = false;
		} else if (!strcmp(a, "--no-mux")) {
			use_mux = false;
		} else {
			usage();
		}
	}
	/* strip a trailing slash so relative paths join cleanly */
	size_t rl = strlen(storage_root);
	while (rl > 1 && storage_root[rl - 1] == '/') {
		storage_root[--rl] = 0;
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	if (pipe(wake_pipe) < 0) {
		return 1;
	}
	fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
	for (int i = 0; i < MAX_CONNS; i++) {
		conns[i].fd = -1;
	}

	get_udid();
	if (ep_open(&ptp_ep, PTP_DEV, testdir, "ptp") < 0 || ep_open(&mux_ep, MUX_DEV, testdir, "mux") < 0) {
		return 1;
	}
	if (!testdir) {
		struct vivausb_serial s;
		memset(&s, 0, sizeof(s));
		strlcpy(s.s, udid, sizeof(s.s));
		if (ioctl(mux_ep.fd, VIVAUSB_SET_SERIAL, &s) < 0) {
			logm("VIVAUSB_SET_SERIAL: %s", strerror(errno));
		}
		if (ioctl(mux_ep.fd, VIVAUSB_ATTACH) < 0) {
			/* EPERM = vivausbstd=0: the link persona stays; keep the nodes open anyway */
			logm("VIVAUSB_ATTACH: %s", strerror(errno));
		}
	}
	logm("ready (storage %s%s)", storage_root, testdir ? ", test sockets" : "");

	pthread_t t;
	if (use_mux) {
		pthread_create(&t, NULL, mux_reader, NULL);
		pthread_create(&t, NULL, mux_writer, NULL);
		pthread_create(&t, NULL, mux_poller, NULL);
	}
	if (use_ptp) {
		pthread_create(&t, NULL, ptp_reader, NULL);
		pthread_create(&t, NULL, ptp_responder, NULL);
	}
	for (;;) {
		pause();
	}
}
