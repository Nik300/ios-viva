/*
 * VivaAudio.driver: the AudioServerPlugIn that gives iOS 16 (mediaserverd) viva's built-in audio. Installed in
 * /System/Library/Audio/Plug-Ins/HAL/VivaAudio.driver; backed by the kernel's VivaAudio (vivaaudio.h).
 *
 * VirtualAudio.plugin classifies physical HAL devices by UID (research notes, 2026-11-19):
 *   "Codec"    -> the built-in ports. Boolean mute controls on the output scope pick them: element 3 = Built-In
 *                 Receiver ('prec'), element 4 = Built-In Speaker ('pspk'); VirtualAudio unmutes one of the two.
 *                 Output: 48 kHz stereo Float32 -> the kernel's DL1 ring (S32), left = receiver / top speaker,
 *                 right = loudspeaker. Receiver: the mono mix on the left only, amps in their "rcv" scene.
 *                 Input: silence (microphones not driven yet).
 *   "Actuator" -> the Taptic Engine ('pact', VirtualAudioDevice_Actuator on top): with closed-loop haptics every
 *                 iOS vibration arrives here as 48 kHz Float32 mono. viva's LRA (AW86224, f0 ~205 Hz) is driven in
 *                 CONT mode, so only the envelope goes to the kernel (VA_UC_HAPTIC, 2 ms steps).
 * The Codec clock is the kernel's period interrupt (va_status); the Actuator has a host-time clock.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <os/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
#include <dlfcn.h>

#include "vivaaudio.h"

#define kPlugInBundleID         "it.viva.audio.VivaAudio"
#define kRate                   48000.0
#define kActuatorRingFrames     4800
#define kSafetyOffset           96      /* frames: the DL1 prefetch */
#define kSpeakerHighPassHz      350.0   /* 2nd-order Butterworth high-pass on the loudspeaker route: below this the
                                         * small speaker only rattles (iOS's D22 tuning boosts the lows) */
#define kSpeakerHeadroom        0.708f  /* -3 dB on the loudspeaker route: iOS's D22 speaker tuning (virtual bass,
                                         * compressor) drives viva's speaker into audible distortion at full volume */
#define kEnvelopeStepFrames     96      /* 2 ms */
#define kEnvelopeFloor          0.02f   /* below this the motor is off */

enum {
	kObjPlugIn = kAudioObjectPlugInObject,
	kObjCodec = 2,
	kObjCodecOut,
	kObjCodecIn,
	kObjCodecVolume,
	kObjMuteReceiver,
	kObjMuteSpeaker,
	kObjInDataSource,       /* input data source selector: the built-in microphones ('ssc#' / 'ssrc' on 'inpt') */
	kObjActuator = 10,
	kObjActuatorOut,
	/* "Hawking": the always-on listening input ('phki'). VirtualAudio's routes for the default device require the
	 * mandatory Hawking VAD to have its port; without it every speaker route fails and vdef falls back to
	 * Null_Device. Silence, its own host-time clock. */
	kObjHawking = 20,
	kObjHawkingIn,
};
#define kElementReceiver        3
/* VirtualAudio's Device_Codec_Aspen reads the input data sources and makes the Built-In Microphone port from them
 * (mic IDs 'imi1'...); without the property it throws and drops the whole Codec. viva: bottom and top mics. */
static const UInt32 gMics[] = { 'imi1', 'imi2' };
static UInt32 gMicCurrent = 'imi1';
#define kElementSpeaker         4
#define kVolumeMinDB            (-40.0f)

static os_log_t gLog;

/* Debug log file (the phone has no `log` tool): VivaAudio.log in mediaserverd's home, the first kTraceMax property
 * calls in detail, then only sets, IO and errors. */
#define kLogPath                "/private/var/mobile/Library/Logs/VivaAudio.log"
#define kTraceMax               4000
static FILE *gFile;
static unsigned gTraced;

static void __printflike(1, 2)
dlog(const char *fmt, ...)
{
	if (gFile == NULL) {
		gFile = fopen(kLogPath, "a");
		if (gFile == NULL) {
			gFile = fopen("/private/var/tmp/VivaAudio.log", "a");
		}
		if (gFile == NULL) {
			return;
		}
		setvbuf(gFile, NULL, _IOLBF, 0);
	}
	va_list ap;
	va_start(ap, fmt);
	fprintf(gFile, "%llu ", (unsigned long long)(mach_absolute_time() / 24000));     /* ms at 24 MHz */
	vfprintf(gFile, fmt, ap);
	fputc('\n', gFile);
	va_end(ap);
}

static const char *
fcc(UInt32 v, char *b)
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (24 - 8 * i));
		b[i] = c >= 32 && c < 127 ? c : '.';
	}
	b[4] = 0;
	return b;
}
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost;
static UInt32 gRefCount;

/* kernel */
static io_connect_t gConn;
static volatile struct va_status *gStatus;
static volatile int32_t *gRing;
static Boolean gRing16;

/* loudspeaker high-pass: biquad (RBJ cookbook), direct form I state per channel */
static Float32 gHP[5];                  /* b0 b1 b2 a1 a2 (a0 = 1) */
static Float32 gHPState[2][4];          /* x1 x2 y1 y2 */

static void
highPassInit(void)
{
	Float64 w = 2.0 * M_PI * kSpeakerHighPassHz / kRate, c = cos(w), al = sin(w) / (2.0 * M_SQRT1_2);
	Float64 a0 = 1.0 + al;
	gHP[0] = (Float32)((1.0 + c) / 2.0 / a0);
	gHP[1] = (Float32)(-(1.0 + c) / a0);
	gHP[2] = gHP[0];
	gHP[3] = (Float32)(-2.0 * c / a0);
	gHP[4] = (Float32)((1.0 - al) / a0);
}

static inline Float32
highPass(int ch, Float32 x)
{
	Float32 *z = gHPState[ch];
	Float32 y = gHP[0] * x + gHP[1] * z[0] + gHP[2] * z[1] - gHP[3] * z[2] - gHP[4] * z[3];
	z[1] = z[0];
	z[0] = x;
	z[3] = z[2];
	z[2] = y;
	return y;
}
static UInt32 gRingFrames = 4800;
static Float64 gTicksPerFrame;

/* state */
static UInt32 gCodecIO, gActuatorIO, gHawkingIO;
static Boolean gMuteReceiver = true, gMuteSpeaker = false;
static Float32 gVolume = 1.0f;          /* scalar */
static Float32 gGain = 1.0f;            /* linear, from gVolume */
static UInt64 gActuatorAnchor, gActuatorSeed = 1;
static Boolean gHapticOn;
#define kCaptureFrames          (48000 * 4)
static FILE *gCapture;
static UInt32 gCaptured;
static UInt32 gStreamActive[4] = { 1, 1, 1, 1 };   /* kAudioStreamPropertyIsActive of Codec out, Codec in, Actuator */

static UInt32 *
streamActive(AudioObjectID o)
{
	return &gStreamActive[o == kObjCodecOut ? 0 : (o == kObjCodecIn ? 1 : (o == kObjHawkingIn ? 3 : 2))];
}

/* ------------------------------------------------------------------------------------------------ kernel */

static Boolean
kernelOpen(void)
{
	if (gConn != IO_OBJECT_NULL) {
		return true;
	}
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("VivaAudio"));
	if (s == IO_OBJECT_NULL) {
		os_log_error(gLog, "VivaAudio: no VivaAudio service");
		return false;
	}
	kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0, &gConn);
	IOObjectRelease(s);
	if (kr != KERN_SUCCESS) {
		os_log_error(gLog, "VivaAudio: IOServiceOpen 0x%x", kr);
		gConn = IO_OBJECT_NULL;
		return false;
	}
	mach_vm_address_t a = 0;
	mach_vm_size_t n = 0;
	if (IOConnectMapMemory64(gConn, VA_MAP_STATUS, mach_task_self(), &a, &n, kIOMapAnywhere | kIOMapReadOnly) !=
	    KERN_SUCCESS) {
		os_log_error(gLog, "VivaAudio: status map failed");
		return false;
	}
	gStatus = (volatile struct va_status *)a;
	a = 0;
	n = 0;
	if (IOConnectMapMemory64(gConn, VA_MAP_RING, mach_task_self(), &a, &n, kIOMapAnywhere | kIOMapWriteCombineCache) !=
	    KERN_SUCCESS) {
		os_log_error(gLog, "VivaAudio: ring map failed");
		return false;
	}
	gRing = (volatile int32_t *)a;
	/*
	 * The kernel (0222/0223) programs DL1 with HD_MODE 2, which on the MT6781 reads 16-bit stereo, not 32-bit
	 * (measured 2026-11-19: 192000 bytes/s at 48 kHz; HD_MODE 1 = 24-in-32 reads 384000). Until the kernel uses
	 * HD_MODE 1 the ring holds 2 x int16 frames: twice as many frames in the same bytes.
	 */
	Boolean legacy = gStatus->frameBytes == 8 && gStatus->ringFrames == 4800 &&
	    gStatus->sampleFormat != VA_FORMAT_S32;                                 /* 0222/0223 kernels */
	gRing16 = legacy || gStatus->frameBytes == 4;                               /* 0224: S16_LE, says so */
	gRingFrames = legacy ? 9600 : (gStatus->ringFrames ? gStatus->ringFrames : 9600);
	os_log(gLog, "VivaAudio: kernel open, ring %u frames, %s", gRingFrames, gRing16 ? "16-bit" : "24-bit in S32");
	return true;
}

static UInt32
codecOutputs(Boolean *receiverScene)
{
	*receiverScene = false;
	if (!gMuteSpeaker) {
		return VA_OUT_SPK | VA_OUT_RCV;         /* stereo: top + bottom, as Android's media path */
	}
	if (!gMuteReceiver) {
		*receiverScene = true;
		return VA_OUT_RCV;
	}
	return 0;
}

static UInt32 gKernelOutputs = ~0u;     /* what the kernel was last told (~0: nothing yet) */

/* Only real changes go to the kernel: VirtualAudio re-sends the same mute at every volume step, and a start
 * re-zeroes the ring (an audible gap). */
static void
kernelRoute(void)
{
	if (gConn == IO_OBJECT_NULL || gCodecIO == 0) {
		return;
	}
	Boolean rcv;
	UInt32 o = codecOutputs(&rcv);
	UInt32 key = o | (rcv ? 0x100 : 0);
	if (key == gKernelOutputs) {
		return;
	}
	gKernelOutputs = key;
	uint64_t in[2] = { o, rcv };
	kern_return_t kr = o ? IOConnectCallScalarMethod(gConn, VA_UC_START, in, 2, NULL, NULL) :
	    IOConnectCallScalarMethod(gConn, VA_UC_STOP, NULL, 0, NULL, NULL);
	os_log(gLog, "VivaAudio: route outputs %u%s: 0x%x", o, rcv ? " (receiver)" : "", kr);
	dlog("route outputs %u%s: 0x%x", o, rcv ? " (receiver)" : "", kr);
}

static void
kernelHaptic(const struct va_haptic *h)
{
	if (gConn != IO_OBJECT_NULL) {
		IOConnectCallStructMethod(gConn, VA_UC_HAPTIC, h, sizeof(*h), NULL, NULL);
	}
}

/* ------------------------------------------------------------------------------------------------ helpers */

static Float32
dbFromScalar(Float32 s)
{
	return s <= 0.0f ? kVolumeMinDB : kVolumeMinDB * (1.0f - s);
}

static Float32
scalarFromDB(Float32 db)
{
	db = db < kVolumeMinDB ? kVolumeMinDB : (db > 0.0f ? 0.0f : db);
	return 1.0f - db / kVolumeMinDB;
}

static void
setVolumeScalar(Float32 s)
{
	gVolume = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
	gGain = gVolume <= 0.0f ? 0.0f : powf(10.0f, dbFromScalar(gVolume) / 20.0f);
}

static AudioStreamBasicDescription
streamFormat(UInt32 channels)
{
	AudioStreamBasicDescription f = {
		.mSampleRate = kRate,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 4 * channels,
		.mFramesPerPacket = 1,
		.mBytesPerFrame = 4 * channels,
		.mChannelsPerFrame = channels,
		.mBitsPerChannel = 32,
	};
	return f;
}

#define RET(type, value) do { \
	if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
	*(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define RETSTR(s) RET(CFStringRef, CFSTR(s))

/* ------------------------------------------------------------------------------------------------ COM */

static HRESULT VA_QueryInterface(void *self, REFIID iid, LPVOID *out);
static ULONG VA_AddRef(void *self);
static ULONG VA_Release(void *self);
static OSStatus VA_Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host);
static OSStatus VA_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
    const AudioServerPlugInClientInfo *client, AudioObjectID *out) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus VA_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) {
	return kAudioHardwareUnsupportedOperationError; }
static OSStatus VA_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev,
    const AudioServerPlugInClientInfo *client) { return 0; }
static OSStatus VA_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev,
    const AudioServerPlugInClientInfo *client) { return 0; }
static OSStatus VA_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action,
    void *info) { return 0; }
static OSStatus VA_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action,
    void *info) { return 0; }
static Boolean VA_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a);
static OSStatus VA_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, Boolean *out);
static OSStatus VA_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 *out);
static OSStatus VA_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 inDataSize, UInt32 *outDataSize,
    void *outData);
static OSStatus VA_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 size, const void *data);
static OSStatus getData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q,
    UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus VA_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client);
static OSStatus VA_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client);
static OSStatus VA_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
    Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed);
static OSStatus VA_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
    Boolean *will, Boolean *inPlace);
static OSStatus VA_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) { return 0; }
static OSStatus VA_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream,
    UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary);
static OSStatus VA_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) { return 0; }

static AudioServerPlugInDriverInterface gInterface = {
	NULL, VA_QueryInterface, VA_AddRef, VA_Release, VA_Initialize, VA_CreateDevice, VA_DestroyDevice,
	VA_AddDeviceClient, VA_RemoveDeviceClient, VA_PerformConfigChange, VA_AbortConfigChange, VA_HasProperty,
	VA_IsPropertySettable, VA_GetPropertyDataSize, VA_GetPropertyData, VA_SetPropertyData, VA_StartIO, VA_StopIO,
	VA_GetZeroTimeStamp, VA_WillDoIOOperation, VA_BeginIOOperation, VA_DoIOOperation, VA_EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriver = &gInterfacePtr;

/*
 * VirtualAudio builds a closed-loop speaker-protection DSP chain for every D22 speaker route ('clsp'), which needs
 * Apple's amp telemetry ("Speaker" device, SpCl calibration); without it the route fails. Its own escape hatch
 * (RunTimeDefaults, func_d7920) builds the default chain when MGCopyAnswer("InternalBuild") is true AND the
 * preference com.apple.audio.virtualaudio RawModeDisablesSpeakerProcessing is set. Only VirtualAudio's import of
 * MGCopyAnswer is rebound here (its indirect symbol pointers), so nothing else in mediaserverd sees InternalBuild.
 */
static CFTypeRef (*gRealMGCopyAnswer)(CFStringRef, CFDictionaryRef);

static CFTypeRef
vaMGCopyAnswer(CFStringRef key, CFDictionaryRef opts)
{
	if (key != NULL && CFEqual(key, CFSTR("InternalBuild"))) {
		return kCFBooleanTrue;
	}
	return gRealMGCopyAnswer ? gRealMGCopyAnswer(key, opts) : NULL;
}

static void
rebindImage(const struct mach_header *mh, intptr_t slide, const char *symbol, void *replacement)
{
	const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
	const struct load_command *lc = (const struct load_command *)(h + 1);
	const struct segment_command_64 *linkedit = NULL;
	const struct symtab_command *symtab = NULL;
	const struct dysymtab_command *dysymtab = NULL;
	for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT") == 0) {
			linkedit = (const struct segment_command_64 *)lc;
		} else if (lc->cmd == LC_SYMTAB) {
			symtab = (const struct symtab_command *)lc;
		} else if (lc->cmd == LC_DYSYMTAB) {
			dysymtab = (const struct dysymtab_command *)lc;
		}
	}
	if (linkedit == NULL || symtab == NULL || dysymtab == NULL) {
		return;
	}
	uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
	const struct nlist_64 *syms = (const struct nlist_64 *)(base + symtab->symoff);
	const char *strs = (const char *)(base + symtab->stroff);
	const uint32_t *indirect = (const uint32_t *)(base + dysymtab->indirectsymoff);
	lc = (const struct load_command *)(h + 1);
	for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
		if (lc->cmd != LC_SEGMENT_64) {
			continue;
		}
		const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
		const struct section_64 *sect = (const struct section_64 *)(seg + 1);
		for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
			uint32_t type = sect->flags & SECTION_TYPE;
			if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) {
				continue;
			}
			void **ptrs = (void **)((uintptr_t)slide + sect->addr);
			for (uint32_t k = 0; k < sect->size / sizeof(void *); k++) {
				uint32_t si = indirect[sect->reserved1 + k];
				if (si == INDIRECT_SYMBOL_ABS || si == INDIRECT_SYMBOL_LOCAL || si == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
					continue;
				}
				if (strcmp(strs + syms[si].n_un.n_strx, symbol) != 0) {
					continue;
				}
				uintptr_t page = (uintptr_t)&ptrs[k] & ~(uintptr_t)(getpagesize() - 1);
				if (mprotect((void *)page, getpagesize(), PROT_READ | PROT_WRITE) != 0) {
					dlog("rebind %s: mprotect failed", symbol);
					continue;
				}
				ptrs[k] = replacement;
				mprotect((void *)page, getpagesize(), PROT_READ);
				dlog("rebound %s in %s", symbol, sect->sectname);
			}
		}
	}
}

static void
imageAdded(const struct mach_header *mh, intptr_t slide)
{
	Dl_info info;
	if (dladdr(mh, &info) && info.dli_fname && strstr(info.dli_fname, "VirtualAudio.plugin/VirtualAudio")) {
		if (gRealMGCopyAnswer == NULL) {
			gRealMGCopyAnswer = dlsym(RTLD_DEFAULT, "MGCopyAnswer");
		}
		rebindImage(mh, slide, "_MGCopyAnswer", (void *)vaMGCopyAnswer);
	}
}

void *VA_Create(CFAllocatorRef alloc, CFUUIDRef type);

void *
VA_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
	if (!CFEqual(type, kAudioServerPlugInTypeUUID)) {
		return NULL;
	}
	gLog = os_log_create(kPlugInBundleID, "hal");
	os_log(gLog, "VivaAudio: plug-in created");
	dlog("--- plug-in created in pid %d (uid %d)", getpid(), getuid());
	_dyld_register_func_for_add_image(imageAdded);   /* also called now for the images already loaded */
	/* debugging aid: with OS_ACTIVITY_DT_MODE in launchd's environment os_log also goes to stderr, which launchd
	 * points at /dev/null; send it to a file instead when that file exists */
	if (access("/private/var/mobile/Library/Logs/mediaserverd.stderr", W_OK) == 0) {
		FILE *f = fopen("/private/var/mobile/Library/Logs/mediaserverd.stderr", "a");
		int r = f ? dup2(fileno(f), 2) : -1;
		dlog("stderr -> mediaserverd.stderr: dup2 %d, OS_ACTIVITY_DT_MODE=%s", r,
		    getenv("OS_ACTIVITY_DT_MODE") ?: "(unset)");
		fprintf(stderr, "VivaAudio: stderr of pid %d\n", getpid());
		os_log(gLog, "VivaAudio: os_log test line");
	}
	return gDriver;
}

static HRESULT
VA_QueryInterface(void *self, REFIID iid, LPVOID *out)
{
	CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
	Boolean ok = CFEqual(u, IUnknownUUID) || CFEqual(u, kAudioServerPlugInDriverInterfaceUUID);
	CFRelease(u);
	if (!ok) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	pthread_mutex_lock(&gLock);
	gRefCount++;
	pthread_mutex_unlock(&gLock);
	*out = gDriver;
	return S_OK;
}

static ULONG
VA_AddRef(void *self)
{
	pthread_mutex_lock(&gLock);
	ULONG r = ++gRefCount;
	pthread_mutex_unlock(&gLock);
	return r;
}

static ULONG
VA_Release(void *self)
{
	pthread_mutex_lock(&gLock);
	ULONG r = gRefCount ? --gRefCount : 0;
	pthread_mutex_unlock(&gLock);
	return r;
}

static OSStatus
VA_Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
	gHost = host;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	gTicksPerFrame = (1e9 / kRate) * tb.denom / tb.numer;
	setVolumeScalar(1.0f);
	highPassInit();
	gActuatorAnchor = mach_absolute_time();     /* the Hawking clock runs from here even if the Actuator never starts */
	Boolean k = kernelOpen();
	os_log(gLog, "VivaAudio: initialized, %.3f host ticks per frame", gTicksPerFrame);
	dlog("initialized: kernel %s, ring %u frames %s, %.3f ticks/frame", k ? "open" : "NOT open", gRingFrames,
	    gRing16 ? "16-bit" : "24-bit in S32", gTicksPerFrame);
	return 0;
}

/* ------------------------------------------------------------------------------------------------ properties */

static Boolean
VA_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a)
{
	UInt32 size;
	Boolean has = VA_GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
	if (gTraced < kTraceMax) {
		char s1[5], s2[5];
		gTraced++;
		dlog("has pid %d obj %u '%s' '%s' %u -> %d", pid, o, fcc(a->mSelector, s1), fcc(a->mScope, s2), a->mElement,
		    has);
	}
	return has;
}

static OSStatus
VA_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    Boolean *out)
{
	UInt32 size;
	OSStatus e = VA_GetPropertyDataSize(d, o, pid, a, 0, NULL, &size);
	if (e) {
		return e;
	}
	switch (a->mSelector) {
	case kAudioLevelControlPropertyScalarValue:
	case kAudioLevelControlPropertyDecibelValue:
		*out = o == kObjCodecVolume;
		break;
	case kAudioBooleanControlPropertyValue:
		*out = o == kObjMuteReceiver || o == kObjMuteSpeaker;
		break;
	case kAudioDevicePropertyNominalSampleRate:
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		*out = true;            /* only to the one format we have */
		break;
	case kAudioStreamPropertyIsActive:
		*out = o == kObjCodecOut || o == kObjCodecIn || o == kObjActuatorOut || o == kObjHawkingIn;
		break;
	case kAudioSelectorControlPropertyCurrentItem:
		*out = o == kObjInDataSource;
		break;
	default:
		*out = false;
	}
	return 0;
}

/* Size by asking for the data into a scratch buffer: one switch for both. */
static OSStatus
VA_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qsize, const void *q, UInt32 *out)
{
	union {
		uint8_t b[512];
		CFStringRef s;
	} scratch;
	UInt32 size = 0;
	OSStatus e = getData(o, a, qsize, q, sizeof(scratch), &size, &scratch);
	if (e == 0) {
		*out = size;
		if (size == sizeof(CFStringRef) && (a->mSelector == kAudioObjectPropertyName ||
		    a->mSelector == kAudioObjectPropertyManufacturer || a->mSelector == kAudioDevicePropertyDeviceUID ||
		    a->mSelector == kAudioDevicePropertyModelUID)) {
			/* the CFSTR constants need no release */
		}
	}
	return e;
}

static OSStatus
objectIDs(const AudioObjectID *ids, UInt32 n, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
	UInt32 k = MIN(n, inDataSize / (UInt32)sizeof(AudioObjectID));
	memcpy(outData, ids, k * sizeof(AudioObjectID));
	*outDataSize = k * (UInt32)sizeof(AudioObjectID);
	return 0;
}

static OSStatus
plugInProperty(const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 inDataSize,
    UInt32 *outDataSize, void *outData)
{
	static const AudioObjectID devices[] = { kObjCodec, kObjActuator, kObjHawking };
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RET(AudioClassID, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RET(AudioClassID, kAudioPlugInClassID);
	case kAudioObjectPropertyOwner:
		RET(AudioObjectID, kAudioObjectUnknown);
	case kAudioObjectPropertyManufacturer:
		RETSTR("viva");
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyDeviceList:
		return objectIDs(devices, 3, inDataSize, outDataSize, outData);
	case kAudioPlugInPropertyTranslateUIDToDevice: {
		if (qsize < sizeof(CFStringRef) || q == NULL) {
			return kAudioHardwareBadPropertySizeError;
		}
		CFStringRef uid = *(const CFStringRef *)q;
		AudioObjectID dev = kAudioObjectUnknown;
		if (uid != NULL && CFEqual(uid, CFSTR("Codec"))) {
			dev = kObjCodec;
		} else if (uid != NULL && CFEqual(uid, CFSTR("Actuator"))) {
			dev = kObjActuator;
		} else if (uid != NULL && CFEqual(uid, CFSTR("Hawking"))) {
			dev = kObjHawking;
		}
		RET(AudioObjectID, dev);
	}
	case kAudioPlugInPropertyResourceBundle:
		RETSTR("");
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus
deviceProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize,
    void *outData)
{
	Boolean codec = o == kObjCodec, hawk = o == kObjHawking;
	Boolean out = a->mScope == kAudioObjectPropertyScopeOutput, in = a->mScope == kAudioObjectPropertyScopeInput,
	    glob = a->mScope == kAudioObjectPropertyScopeGlobal;
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RET(AudioClassID, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RET(AudioClassID, kAudioDeviceClassID);
	case kAudioObjectPropertyOwner:
		RET(AudioObjectID, kObjPlugIn);
	case kAudioObjectPropertyName:
		if (codec) {
			RETSTR("Codec");
		}
		if (hawk) {
			RETSTR("Hawking");
		}
		RETSTR("Actuator");
	case kAudioObjectPropertyManufacturer:
		RETSTR("viva");
	case kAudioObjectPropertyOwnedObjects: {
		static const AudioObjectID c[] = { kObjCodecOut, kObjCodecIn, kObjCodecVolume, kObjMuteReceiver,
			kObjMuteSpeaker, kObjInDataSource };
		static const AudioObjectID co[] = { kObjCodecOut, kObjCodecVolume, kObjMuteReceiver, kObjMuteSpeaker };
		static const AudioObjectID ci[] = { kObjCodecIn, kObjInDataSource };
		static const AudioObjectID ac[] = { kObjActuatorOut }, hk[] = { kObjHawkingIn };
		if (hawk) {
			return objectIDs(hk, out ? 0 : 1, inDataSize, outDataSize, outData);
		}
		if (!codec) {
			return objectIDs(ac, in ? 0 : 1, inDataSize, outDataSize, outData);
		}
		return glob ? objectIDs(c, 6, inDataSize, outDataSize, outData) :
		       (out ? objectIDs(co, 4, inDataSize, outDataSize, outData) :
		       objectIDs(ci, 2, inDataSize, outDataSize, outData));
	}
	case kAudioObjectPropertyControlList: {
		static const AudioObjectID c[] = { kObjCodecVolume, kObjMuteReceiver, kObjMuteSpeaker, kObjInDataSource };
		return objectIDs(c, codec ? 4 : 0, inDataSize, outDataSize, outData);
	}
	case kAudioDevicePropertyDeviceUID:
		if (codec) {
			RETSTR("Codec");
		}
		if (hawk) {
			RETSTR("Hawking");
		}
		RETSTR("Actuator");
	case kAudioDevicePropertyModelUID:
		if (codec) {
			RETSTR("it.viva.audio.codec");
		}
		if (hawk) {
			RETSTR("it.viva.audio.hawking");
		}
		RETSTR("it.viva.audio.actuator");
	case kAudioDevicePropertyTransportType:
		RET(UInt32, kAudioDeviceTransportTypeBuiltIn);
	case kAudioDevicePropertyRelatedDevices: {
		AudioObjectID me = o;
		return objectIDs(&me, 1, inDataSize, outDataSize, outData);
	}
	case kAudioDevicePropertyClockDomain:
		RET(UInt32, codec ? 1 : (hawk ? 3 : 2));
	case kAudioDevicePropertyDeviceIsAlive:
		RET(UInt32, 1);
	case kAudioDevicePropertyDeviceIsRunning:
		RET(UInt32, codec ? gCodecIO > 0 : (hawk ? gHawkingIO > 0 : gActuatorIO > 0));
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		RET(UInt32, codec);
	case kAudioDevicePropertyLatency:
		RET(UInt32, 0);
	case kAudioDevicePropertySafetyOffset:
		RET(UInt32, codec ? kSafetyOffset : 0);
	case kAudioDevicePropertyStreams: {
		static const AudioObjectID both[] = { kObjCodecOut, kObjCodecIn };
		static const AudioObjectID ao[] = { kObjActuatorOut }, hi[] = { kObjHawkingIn };
		if (hawk) {
			return objectIDs(hi, out ? 0 : 1, inDataSize, outDataSize, outData);
		}
		if (!codec) {
			return objectIDs(ao, in ? 0 : 1, inDataSize, outDataSize, outData);
		}
		return glob ? objectIDs(both, 2, inDataSize, outDataSize, outData) :
		       objectIDs(out ? &both[0] : &both[1], 1, inDataSize, outDataSize, outData);
	}
	case kAudioDevicePropertyNominalSampleRate:
		RET(Float64, kRate);
	case kAudioDevicePropertyAvailableNominalSampleRates: {
		if (inDataSize < sizeof(AudioValueRange)) {
			return kAudioHardwareBadPropertySizeError;
		}
		AudioValueRange r = { kRate, kRate };
		RET(AudioValueRange, r);
	}
	case kAudioDevicePropertyIsHidden:
		RET(UInt32, 0);
	case kAudioDevicePropertyZeroTimeStampPeriod:
		RET(UInt32, codec ? gRingFrames : kActuatorRingFrames);
	case kAudioDevicePropertyClockIsStable:
		RET(UInt32, 1);
	case kAudioDevicePropertyPreferredChannelsForStereo: {
		if (inDataSize < 2 * sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		((UInt32 *)outData)[0] = 1;
		((UInt32 *)outData)[1] = codec ? 2 : 1;     /* Hawking and Actuator: mono */
		*outDataSize = 2 * sizeof(UInt32);
		return 0;
	}
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus
streamProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize,
    void *outData)
{
	Boolean input = o == kObjCodecIn || o == kObjHawkingIn;
	UInt32 ch = o == kObjActuatorOut || o == kObjHawkingIn ? 1 : 2;
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RET(AudioClassID, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RET(AudioClassID, kAudioStreamClassID);
	case kAudioObjectPropertyOwner:
		RET(AudioObjectID, o == kObjActuatorOut ? kObjActuator : (o == kObjHawkingIn ? kObjHawking : kObjCodec));
	case kAudioObjectPropertyName:
		if (o == kObjHawkingIn) {
			RETSTR("Hawking Input");
		}
		if (o == kObjActuatorOut) {
			RETSTR("Actuator");
		}
		if (input) {
			RETSTR("Codec Input");
		}
		RETSTR("Codec Output");
	case kAudioObjectPropertyOwnedObjects:
		*outDataSize = 0;
		return 0;
	case kAudioStreamPropertyIsActive:
		RET(UInt32, *streamActive(o));
	case kAudioStreamPropertyDirection:
		RET(UInt32, input ? 1 : 0);
	case kAudioStreamPropertyTerminalType:
		RET(UInt32, input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
	case kAudioStreamPropertyStartingChannel:
		RET(UInt32, 1);
	case kAudioStreamPropertyLatency:
		RET(UInt32, 0);
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		RET(AudioStreamBasicDescription, streamFormat(ch));
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats: {
		AudioStreamRangedDescription r = { streamFormat(ch), { kRate, kRate } };
		RET(AudioStreamRangedDescription, r);
	}
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus
dataSourceProperty(const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 inDataSize,
    UInt32 *outDataSize, void *outData)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RET(AudioClassID, kAudioSelectorControlClassID);
	case kAudioObjectPropertyClass:
		RET(AudioClassID, kAudioDataSourceControlClassID);
	case kAudioObjectPropertyOwner:
		RET(AudioObjectID, kObjCodec);
	case kAudioObjectPropertyOwnedObjects:
		*outDataSize = 0;
		return 0;
	case kAudioControlPropertyScope:
		RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeInput);
	case kAudioControlPropertyElement:
		RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
	case kAudioSelectorControlPropertyCurrentItem:
		RET(UInt32, gMicCurrent);
	case kAudioSelectorControlPropertyAvailableItems:
		return objectIDs(gMics, 2, inDataSize, outDataSize, outData);
	case kAudioSelectorControlPropertyItemName:
		if (qsize < sizeof(UInt32) || q == NULL) {
			return kAudioHardwareBadPropertySizeError;
		}
		if (*(const UInt32 *)q == 'imi2') {
			RETSTR("Top Microphone");
		}
		RETSTR("Bottom Microphone");
	case kAudioSelectorControlPropertyItemKind:
		RET(UInt32, 0);
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

static OSStatus
controlProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q,
    UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
	if (o == kObjInDataSource) {
		return dataSourceProperty(a, qsize, q, inDataSize, outDataSize, outData);
	}
	Boolean volume = o == kObjCodecVolume;
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RET(AudioClassID, volume ? kAudioLevelControlClassID : kAudioBooleanControlClassID);
	case kAudioObjectPropertyClass:
		RET(AudioClassID, volume ? kAudioVolumeControlClassID : kAudioMuteControlClassID);
	case kAudioObjectPropertyOwner:
		RET(AudioObjectID, kObjCodec);
	case kAudioObjectPropertyOwnedObjects:
		*outDataSize = 0;
		return 0;
	case kAudioControlPropertyScope:
		RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
	case kAudioControlPropertyElement:
		RET(AudioObjectPropertyElement, volume ? kAudioObjectPropertyElementMain :
		    (o == kObjMuteReceiver ? kElementReceiver : kElementSpeaker));
	case kAudioBooleanControlPropertyValue:
		if (volume) {
			break;
		}
		RET(UInt32, o == kObjMuteReceiver ? gMuteReceiver : gMuteSpeaker);
	case kAudioLevelControlPropertyScalarValue:
		if (!volume) {
			break;
		}
		RET(Float32, gVolume);
	case kAudioLevelControlPropertyDecibelValue:
		if (!volume) {
			break;
		}
		RET(Float32, dbFromScalar(gVolume));
	case kAudioLevelControlPropertyDecibelRange: {
		if (!volume) {
			break;
		}
		AudioValueRange r = { kVolumeMinDB, 0.0 };
		RET(AudioValueRange, r);
	}
	case kAudioLevelControlPropertyConvertScalarToDecibels:
		if (!volume || inDataSize < sizeof(Float32)) {
			break;
		}
		RET(Float32, dbFromScalar(*(Float32 *)outData));
	case kAudioLevelControlPropertyConvertDecibelsToScalar:
		if (!volume || inDataSize < sizeof(Float32)) {
			break;
		}
		RET(Float32, scalarFromDB(*(Float32 *)outData));
	default:
		break;
	}
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus
getData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q, UInt32 inDataSize,
    UInt32 *outDataSize, void *outData)
{
	switch (o) {
	case kObjPlugIn:
		return plugInProperty(a, qsize, q, inDataSize, outDataSize, outData);
	case kObjCodec:
	case kObjActuator:
	case kObjHawking:
		return deviceProperty(o, a, inDataSize, outDataSize, outData);
	case kObjCodecOut:
	case kObjCodecIn:
	case kObjActuatorOut:
	case kObjHawkingIn:
		return streamProperty(o, a, inDataSize, outDataSize, outData);
	case kObjCodecVolume:
	case kObjMuteReceiver:
	case kObjMuteSpeaker:
	case kObjInDataSource:
		return controlProperty(o, a, qsize, q, inDataSize, outDataSize, outData);
	default:
		return kAudioHardwareBadObjectError;
	}
}

static OSStatus
VA_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qsize, const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
	OSStatus e = getData(o, a, qsize, q, inDataSize, outDataSize, outData);
	if (gTraced < kTraceMax || e != 0) {
		char s1[5], s2[5], s3[5];
		gTraced += gTraced < kTraceMax;
		Boolean str = a->mSelector == kAudioObjectPropertyName || a->mSelector == kAudioObjectPropertyManufacturer ||
		    a->mSelector == kAudioDevicePropertyDeviceUID || a->mSelector == kAudioDevicePropertyModelUID ||
		    a->mSelector == kAudioPlugInPropertyResourceBundle;
		if (e == 0 && str && *outDataSize == sizeof(CFStringRef)) {
			char str[64] = "";
			CFStringGetCString(*(CFStringRef *)outData, str, sizeof(str), kCFStringEncodingUTF8);
			dlog("get pid %d obj %u '%s' '%s' %u -> \"%s\"", pid, o, fcc(a->mSelector, s1), fcc(a->mScope, s2),
			    a->mElement, str);
		} else if (e == 0 && *outDataSize == 4) {
			dlog("get pid %d obj %u '%s' '%s' %u -> %u ('%s')", pid, o, fcc(a->mSelector, s1), fcc(a->mScope, s2),
			    a->mElement, *(UInt32 *)outData, fcc(*(UInt32 *)outData, s3));
		} else {
			dlog("get pid %d obj %u '%s' '%s' %u -> %s%d (%u bytes)", pid, o, fcc(a->mSelector, s1),
			    fcc(a->mScope, s2), a->mElement, e ? "ERR '" : "", e ? (int)e : 0, e ? 0 : *outDataSize);
		}
	}
	return e;
}

static void
changed(AudioObjectID o, AudioObjectPropertySelector sel)
{
	AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	if (gHost != NULL) {
		gHost->PropertiesChanged(gHost, o, 1, &a);
	}
}

static OSStatus
VA_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qsize, const void *q, UInt32 size, const void *data)
{
	char s1[5], s2[5];
	dlog("set pid %d obj %u '%s' '%s' %u size %u value %u", pid, o, fcc(a->mSelector, s1), fcc(a->mScope, s2),
	    a->mElement, size, size >= 4 ? *(const UInt32 *)data : 0);
	switch (a->mSelector) {
	case kAudioBooleanControlPropertyValue:
		if ((o != kObjMuteReceiver && o != kObjMuteSpeaker) || size < sizeof(UInt32)) {
			return kAudioHardwareIllegalOperationError;
		}
		pthread_mutex_lock(&gLock);
		if (o == kObjMuteReceiver) {
			gMuteReceiver = *(const UInt32 *)data != 0;
		} else {
			gMuteSpeaker = *(const UInt32 *)data != 0;
		}
		os_log(gLog, "VivaAudio: pid %d mutes %s: %u", pid, o == kObjMuteReceiver ? "receiver" : "speaker",
		    *(const UInt32 *)data);
		kernelRoute();
		pthread_mutex_unlock(&gLock);
		changed(o, kAudioBooleanControlPropertyValue);
		return 0;
	case kAudioLevelControlPropertyScalarValue:
	case kAudioLevelControlPropertyDecibelValue:
		if (o != kObjCodecVolume || size < sizeof(Float32)) {
			return kAudioHardwareIllegalOperationError;
		}
		setVolumeScalar(a->mSelector == kAudioLevelControlPropertyScalarValue ? *(const Float32 *)data :
		    scalarFromDB(*(const Float32 *)data));
		changed(o, kAudioLevelControlPropertyScalarValue);
		changed(o, kAudioLevelControlPropertyDecibelValue);
		return 0;
	case kAudioDevicePropertyNominalSampleRate:
		return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioHardwareIllegalOperationError;
	case kAudioSelectorControlPropertyCurrentItem:
		if (o != kObjInDataSource || size < sizeof(UInt32)) {
			return kAudioHardwareIllegalOperationError;
		}
		gMicCurrent = *(const UInt32 *)data == 'imi2' ? 'imi2' : 'imi1';
		changed(o, kAudioSelectorControlPropertyCurrentItem);
		return 0;
	case kAudioStreamPropertyIsActive:
		if ((o != kObjCodecOut && o != kObjCodecIn && o != kObjActuatorOut && o != kObjHawkingIn) ||
		    size < sizeof(UInt32)) {
			return kAudioHardwareIllegalOperationError;
		}
		*streamActive(o) = *(const UInt32 *)data != 0;
		changed(o, kAudioStreamPropertyIsActive);
		return 0;
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		return size >= sizeof(AudioStreamBasicDescription) &&
		       ((const AudioStreamBasicDescription *)data)->mSampleRate == kRate ? 0 :
		       kAudioDeviceUnsupportedFormatError;
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

/* ------------------------------------------------------------------------------------------------ IO */

static OSStatus
VA_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
	dlog("StartIO dev %u client %u (codec %u actuator %u)", dev, client, gCodecIO, gActuatorIO);
	pthread_mutex_lock(&gLock);
	if (dev == kObjCodec) {
		if (gCodecIO++ == 0) {
			if (!kernelOpen()) {
				gCodecIO = 0;
				pthread_mutex_unlock(&gLock);
				return kAudioHardwareNotRunningError;
			}
			kernelRoute();
			if (gCapture == NULL && access("/private/var/mobile/Library/Logs/va_capture.raw", W_OK) == 0) {
				gCapture = fopen("/private/var/mobile/Library/Logs/va_capture.raw", "w");
				gCaptured = 0;
			}
		}
	} else if (dev == kObjHawking) {
		gHawkingIO++;
	} else if (gActuatorIO++ == 0) {
		kernelOpen();
		gActuatorAnchor = mach_absolute_time();
		gActuatorSeed++;
		os_log(gLog, "VivaAudio: actuator IO on");
	}
	pthread_mutex_unlock(&gLock);
	return 0;
}

static OSStatus
VA_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
	dlog("StopIO dev %u client %u", dev, client);
	pthread_mutex_lock(&gLock);
	if (dev == kObjCodec) {
		if (gCodecIO > 0 && --gCodecIO == 0 && gConn != IO_OBJECT_NULL) {
			IOConnectCallScalarMethod(gConn, VA_UC_STOP, NULL, 0, NULL, NULL);
			gKernelOutputs = ~0u;
			os_log(gLog, "VivaAudio: codec IO off");
		}
	} else if (dev == kObjHawking) {
		gHawkingIO -= gHawkingIO > 0;
	} else if (gActuatorIO > 0 && --gActuatorIO == 0) {
		struct va_haptic h = { .count = 1, .steps = { { .ms = 1, .gain = 0 } } };
		kernelHaptic(&h);
		gHapticOn = false;
		os_log(gLog, "VivaAudio: actuator IO off");
	}
	pthread_mutex_unlock(&gLock);
	return 0;
}

static OSStatus
VA_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *sampleTime,
    UInt64 *hostTime, UInt64 *seed)
{
	if (dev == kObjCodec && gStatus != NULL) {
		uint64_t s1, s2, frames, host, sd;
		do {
			s1 = atomic_load_explicit((_Atomic uint64_t *)&gStatus->seq, memory_order_acquire);
			frames = gStatus->frames;
			host = gStatus->hostTime;
			sd = gStatus->startSeed;
			s2 = atomic_load_explicit((_Atomic uint64_t *)&gStatus->seq, memory_order_acquire);
		} while ((s1 & 1) || s1 != s2);
		/*
		 * One timestamp per ring period, computed once and then returned unchanged: re-deriving it from each period
		 * interrupt gives the same sample time slightly different host times (interrupt latency), which the HAL
		 * reads as a broken timeline ("Re-anchoring IO timeline. Sample time is not consecutive") and its IO
		 * cycles then skip a few frames each (stale ring data = noise). Later periods follow the previous anchor
		 * at the measured rate, nudged a little towards the interrupts so long-term drift is still followed.
		 */
		static uint64_t lastZero = ~0ull, lastHost, lastSeed;
		uint64_t zero = frames - frames % gRingFrames;
		uint64_t measured = host - (UInt64)((Float64)(frames - zero) * gTicksPerFrame);
		if (sd != lastSeed || lastZero == ~0ull || zero < lastZero || zero > lastZero + gRingFrames) {
			lastHost = measured;
		} else if (zero == lastZero + gRingFrames) {
			Float64 predicted = (Float64)lastHost + gTicksPerFrame * gRingFrames;
			lastHost = (UInt64)(predicted + ((Float64)measured - predicted) / 16.0);
		}
		lastZero = zero;
		lastSeed = sd;
		*sampleTime = (Float64)zero;
		*hostTime = lastHost;
		*seed = sd;

		return 0;
	}
	/* Actuator (or no kernel): host-time clock, one period = kActuatorRingFrames */
	Float64 period = gTicksPerFrame * kActuatorRingFrames;
	UInt64 n = (UInt64)((Float64)(mach_absolute_time() - gActuatorAnchor) / period);
	*sampleTime = (Float64)(n * kActuatorRingFrames);
	*hostTime = gActuatorAnchor + (UInt64)((Float64)n * period);
	*seed = gActuatorSeed;
	return 0;
}

static OSStatus
VA_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will,
    Boolean *inPlace)
{
	*will = (op == kAudioServerPlugInIOOperationWriteMix && dev != kObjHawking) ||
	    (op == kAudioServerPlugInIOOperationReadInput && (dev == kObjCodec || dev == kObjHawking));
	*inPlace = true;
	return 0;
}

static inline int32_t
toS32(Float32 v)
{
	v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
	return (int32_t)(v * 2147483392.0f);
}

/* Debug capture: the first kCaptureFrames frames after each codec StartIO, as iOS hands them over (float stereo),
 * plus each cycle's sample time, when /private/var/mobile/Library/Logs/va_capture.raw exists. */

/* WriteMix of the Codec: interleaved stereo float -> the kernel ring at the cycle's output sample time. */
static void
codecWrite(const Float32 *src, UInt32 frames, Float64 sampleTime)
{
	if (gCapture != NULL && gCaptured < kCaptureFrames) {
		fwrite(src, sizeof(Float32) * 2, frames, gCapture);
		gCaptured += frames;
		dlog("cycle %.0f frames %u", sampleTime, frames);
		if (gCaptured >= kCaptureFrames) {
			fclose(gCapture);
			gCapture = NULL;
		}
	}
	if (gRing == NULL) {
		return;
	}
	Boolean rcv;
	UInt32 o = codecOutputs(&rcv);
	Float32 g = gGain * ((o & VA_OUT_SPK) && !rcv ? kSpeakerHeadroom : 1.0f);
	UInt64 pos = (UInt64)sampleTime % gRingFrames;
	for (UInt32 i = 0; i < frames; i++) {
		Float32 l = src[2 * i] * g, r = src[2 * i + 1] * g;
		int32_t L, R;
		if (rcv || !(o & VA_OUT_SPK)) {
			L = toS32((l + r) * 0.5f);      /* receiver: mono on the left (HS) */
			R = 0;
		} else {
			L = toS32(highPass(0, l));
			R = toS32(highPass(1, r));
		}
		if (gRing16) {
			((volatile uint32_t *)gRing)[pos] = ((uint32_t)(uint16_t)(L >> 16)) | ((uint32_t)(uint16_t)(R >> 16) << 16);
		} else {
			gRing[2 * pos] = L;
			gRing[2 * pos + 1] = R;
		}
		if (++pos == gRingFrames) {
			pos = 0;
		}
	}
}

/*
 * WriteMix of the Actuator: an envelope follower over iOS's haptic waveform (Apple's ~145 Hz drive signal, period
 * ~7 ms) becomes a drive level per 2 ms step. Instant attack, ~20 ms release, on/off thresholds with hysteresis:
 * a plain per-2-ms peak fell to zero near every zero crossing and chopped long buzzes into many short ones.
 */
#define kEnvReleasePerFrame     0.99896f        /* exp(-1 / (0.020 s * 48000)): 20 ms */
#define kEnvOn                  0.04f
#define kEnvOff                 0.02f
static Float32 gEnv;
static Boolean gEnvActive;

/*
 * Crisp taps (0227): when the envelope switches on from silence, a "tap" step (kernel wave 2: two full-scale cycles
 * then an anti-phase brake cycle, ~15 ms) fires at once with a punchier gain; only if the waveform is still on after
 * kTapWindowFrames does the continuous drive (wave 1 loop, the user-tuned curve) take over. iPhone taps are short
 * and decisive; driving them with the looped sine made every tap a soft, long buzz.
 */
#define kTapWindowFrames        (48 * 30)       /* 30 ms */
enum { kHapIdle, kHapTap, kHapContinuous };
static int gHapMode = kHapIdle;
static UInt32 gHapSinceOnset;

static void
actuatorWrite(const Float32 *src, UInt32 frames)
{
	struct va_haptic h = { 0 };
	Boolean any = false;
	for (UInt32 i = 0; i < frames && h.count < VA_HAPTIC_STEPS; i += kEnvelopeStepFrames) {
		Float32 peak = 0.0f;
		for (UInt32 j = i; j < MIN(frames, i + kEnvelopeStepFrames); j++) {
			Float32 v = fabsf(src[j]);
			gEnv = v > gEnv ? v : gEnv * kEnvReleasePerFrame;
			peak = gEnv > peak ? gEnv : peak;
		}
		Boolean wasActive = gEnvActive;
		gEnvActive = gEnvActive ? peak > kEnvOff : peak > kEnvOn;
		uint8_t gain = 0, kind = 0;
		if (gEnvActive && !wasActive && gHapMode == kHapIdle) {
			/* onset: the tap's strength from the peak of the next 4 ms of the waveform */
			Float32 ahead = peak;
			for (UInt32 j = i; j < MIN(frames, i + 2 * kEnvelopeStepFrames); j++) {
				ahead = fabsf(src[j]) > ahead ? fabsf(src[j]) : ahead;
			}
			gain = (uint8_t)MIN(0x80, (UInt32)(40.0f + 88.0f * powf(MIN(ahead, 1.0f), 0.8f)));
			kind = 1;
			gHapMode = kHapTap;
			gHapSinceOnset = 0;
		} else if (gHapMode == kHapTap) {
			gHapSinceOnset += kEnvelopeStepFrames;
			if (!gEnvActive) {
				gHapMode = kHapIdle;
			} else if (gHapSinceOnset >= kTapWindowFrames) {
				gHapMode = kHapContinuous;
			}
		} else if (gHapMode == kHapContinuous && !gEnvActive) {
			gHapMode = kHapIdle;
		}
		if (gHapMode == kHapContinuous) {
			/* gain curve: 14 + 62 * peak^1.6 (Apple's synced vibes hold ~0.54 with accents up to ~1.0, measured on
			 * "Apertura"): base ~37, accents ~76. A haptic texture following the melody, not a buzz (user tuning
			 * 2026-11-19). */
			gain = (uint8_t)(14.0f + 62.0f * powf(MIN(peak, 1.0f), 1.6f));
		}
		any |= gain != 0;
		struct va_haptic_step *last = h.count ? &h.steps[h.count - 1] : NULL;
		if (last != NULL && kind == 0 && last->reserved == 0 &&
		    (last->gain == gain || (last->gain && gain && abs((int)last->gain - (int)gain) < 8))) {
			last->ms += 2;
		} else {
			h.steps[h.count++] = (struct va_haptic_step){ .ms = 2, .gain = gain, .reserved = kind };
		}
	}
	if (!any && !gHapticOn) {
		return;                 /* silence, and the motor is already off */
	}
	if (h.count > 0 && h.steps[h.count - 1].gain != 0 && h.steps[h.count - 1].reserved == 0) {
		h.steps[h.count - 1].ms += 8;   /* hold until the next cycle replaces it */
	}
	gHapticOn = any;
	kernelHaptic(&h);
}

static OSStatus
VA_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary)
{
	if (op == kAudioServerPlugInIOOperationReadInput) {
		memset(main, 0, (size_t)frames * (dev == kObjHawking ? 1 : 2) * sizeof(Float32));
	} else if (op == kAudioServerPlugInIOOperationWriteMix) {
		if (dev == kObjCodec) {
			codecWrite((const Float32 *)main, frames, cycle->mOutputTime.mSampleTime);
		} else {
			actuatorWrite((const Float32 *)main, frames);
		}
	}
	return 0;
}
