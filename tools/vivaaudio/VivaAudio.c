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
#include <os/log.h>

#include "vivaaudio.h"

#define kPlugInBundleID         "it.viva.audio.VivaAudio"
#define kRate                   48000.0
#define kActuatorRingFrames     4800
#define kSafetyOffset           96      /* frames: the DL1 prefetch */
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
	kObjActuator = 10,
	kObjActuatorOut,
};
#define kElementReceiver        3
#define kElementSpeaker         4
#define kVolumeMinDB            (-40.0f)

static os_log_t gLog;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost;
static UInt32 gRefCount;

/* kernel */
static io_connect_t gConn;
static volatile struct va_status *gStatus;
static volatile int32_t *gRing;
static UInt32 gRingFrames = 4800;
static Float64 gTicksPerFrame;

/* state */
static UInt32 gCodecIO, gActuatorIO;
static Boolean gMuteReceiver = true, gMuteSpeaker = false;
static Float32 gVolume = 1.0f;          /* scalar */
static Float32 gGain = 1.0f;            /* linear, from gVolume */
static UInt64 gActuatorAnchor, gActuatorSeed = 1;
static Boolean gHapticOn;

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
	gRingFrames = gStatus->ringFrames ? gStatus->ringFrames : 4800;
	os_log(gLog, "VivaAudio: kernel open, ring %u frames", gRingFrames);
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

static void
kernelRoute(void)
{
	if (gConn == IO_OBJECT_NULL || gCodecIO == 0) {
		return;
	}
	Boolean rcv;
	UInt32 o = codecOutputs(&rcv);
	uint64_t in[2] = { o, rcv };
	kern_return_t kr = o ? IOConnectCallScalarMethod(gConn, VA_UC_START, in, 2, NULL, NULL) :
	    IOConnectCallScalarMethod(gConn, VA_UC_STOP, NULL, 0, NULL, NULL);
	os_log(gLog, "VivaAudio: route outputs %u%s: 0x%x", o, rcv ? " (receiver)" : "", kr);
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

void *VA_Create(CFAllocatorRef alloc, CFUUIDRef type);

void *
VA_Create(CFAllocatorRef alloc, CFUUIDRef type)
{
	if (!CFEqual(type, kAudioServerPlugInTypeUUID)) {
		return NULL;
	}
	gLog = os_log_create(kPlugInBundleID, "hal");
	os_log(gLog, "VivaAudio: plug-in created");
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
	kernelOpen();
	os_log(gLog, "VivaAudio: initialized, %.3f host ticks per frame", gTicksPerFrame);
	return 0;
}

/* ------------------------------------------------------------------------------------------------ properties */

static Boolean
VA_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a)
{
	UInt32 size;
	return VA_GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
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
	OSStatus e = VA_GetPropertyData(d, o, pid, a, qsize, q, sizeof(scratch), &size, &scratch);
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
	static const AudioObjectID devices[] = { kObjCodec, kObjActuator };
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
		return objectIDs(devices, 2, inDataSize, outDataSize, outData);
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
	Boolean codec = o == kObjCodec;
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
		RETSTR("Actuator");
	case kAudioObjectPropertyManufacturer:
		RETSTR("viva");
	case kAudioObjectPropertyOwnedObjects: {
		static const AudioObjectID c[] = { kObjCodecOut, kObjCodecIn, kObjCodecVolume, kObjMuteReceiver,
			kObjMuteSpeaker };
		static const AudioObjectID co[] = { kObjCodecOut, kObjCodecVolume, kObjMuteReceiver, kObjMuteSpeaker };
		static const AudioObjectID ci[] = { kObjCodecIn };
		static const AudioObjectID ac[] = { kObjActuatorOut };
		if (!codec) {
			return objectIDs(ac, in ? 0 : 1, inDataSize, outDataSize, outData);
		}
		return glob ? objectIDs(c, 5, inDataSize, outDataSize, outData) :
		       (out ? objectIDs(co, 4, inDataSize, outDataSize, outData) :
		       objectIDs(ci, 1, inDataSize, outDataSize, outData));
	}
	case kAudioObjectPropertyControlList: {
		static const AudioObjectID c[] = { kObjCodecVolume, kObjMuteReceiver, kObjMuteSpeaker };
		return objectIDs(c, codec ? 3 : 0, inDataSize, outDataSize, outData);
	}
	case kAudioDevicePropertyDeviceUID:
		if (codec) {
			RETSTR("Codec");
		}
		RETSTR("Actuator");
	case kAudioDevicePropertyModelUID:
		if (codec) {
			RETSTR("it.viva.audio.codec");
		}
		RETSTR("it.viva.audio.actuator");
	case kAudioDevicePropertyTransportType:
		RET(UInt32, kAudioDeviceTransportTypeBuiltIn);
	case kAudioDevicePropertyRelatedDevices: {
		AudioObjectID me = o;
		return objectIDs(&me, 1, inDataSize, outDataSize, outData);
	}
	case kAudioDevicePropertyClockDomain:
		RET(UInt32, codec ? 1 : 2);
	case kAudioDevicePropertyDeviceIsAlive:
		RET(UInt32, 1);
	case kAudioDevicePropertyDeviceIsRunning:
		RET(UInt32, codec ? gCodecIO > 0 : gActuatorIO > 0);
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		RET(UInt32, codec);
	case kAudioDevicePropertyLatency:
		RET(UInt32, 0);
	case kAudioDevicePropertySafetyOffset:
		RET(UInt32, codec ? kSafetyOffset : 0);
	case kAudioDevicePropertyStreams: {
		static const AudioObjectID both[] = { kObjCodecOut, kObjCodecIn };
		static const AudioObjectID ao[] = { kObjActuatorOut };
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
		((UInt32 *)outData)[1] = codec ? 2 : 1;
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
	Boolean input = o == kObjCodecIn;
	UInt32 ch = o == kObjActuatorOut ? 1 : 2;
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		RET(AudioClassID, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		RET(AudioClassID, kAudioStreamClassID);
	case kAudioObjectPropertyOwner:
		RET(AudioObjectID, o == kObjActuatorOut ? kObjActuator : kObjCodec);
	case kAudioObjectPropertyName:
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
		RET(UInt32, 1);
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
controlProperty(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *q,
    UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
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
VA_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qsize, const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
	switch (o) {
	case kObjPlugIn:
		return plugInProperty(a, qsize, q, inDataSize, outDataSize, outData);
	case kObjCodec:
	case kObjActuator:
		return deviceProperty(o, a, inDataSize, outDataSize, outData);
	case kObjCodecOut:
	case kObjCodecIn:
	case kObjActuatorOut:
		return streamProperty(o, a, inDataSize, outDataSize, outData);
	case kObjCodecVolume:
	case kObjMuteReceiver:
	case kObjMuteSpeaker:
		return controlProperty(o, a, qsize, q, inDataSize, outDataSize, outData);
	default:
		return kAudioHardwareBadObjectError;
	}
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
	pthread_mutex_lock(&gLock);
	if (dev == kObjCodec) {
		if (gCodecIO++ == 0) {
			if (!kernelOpen()) {
				gCodecIO = 0;
				pthread_mutex_unlock(&gLock);
				return kAudioHardwareNotRunningError;
			}
			kernelRoute();
		}
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
	pthread_mutex_lock(&gLock);
	if (dev == kObjCodec) {
		if (gCodecIO > 0 && --gCodecIO == 0 && gConn != IO_OBJECT_NULL) {
			IOConnectCallScalarMethod(gConn, VA_UC_STOP, NULL, 0, NULL, NULL);
			os_log(gLog, "VivaAudio: codec IO off");
		}
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
		uint64_t zero = frames - frames % gRingFrames;
		*sampleTime = (Float64)zero;
		*hostTime = host - (UInt64)((Float64)(frames - zero) * gTicksPerFrame);
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
	*will = op == kAudioServerPlugInIOOperationWriteMix ||
	    (op == kAudioServerPlugInIOOperationReadInput && dev == kObjCodec);
	*inPlace = true;
	return 0;
}

static inline int32_t
toS32(Float32 v)
{
	v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
	return (int32_t)(v * 2147483392.0f);
}

/* WriteMix of the Codec: interleaved stereo float -> the kernel ring at the cycle's output sample time. */
static void
codecWrite(const Float32 *src, UInt32 frames, Float64 sampleTime)
{
	if (gRing == NULL) {
		return;
	}
	Boolean rcv;
	UInt32 o = codecOutputs(&rcv);
	Float32 g = gGain;
	UInt64 pos = (UInt64)sampleTime % gRingFrames;
	for (UInt32 i = 0; i < frames; i++) {
		Float32 l = src[2 * i] * g, r = src[2 * i + 1] * g;
		int32_t L, R;
		if (rcv || !(o & VA_OUT_SPK)) {
			L = toS32((l + r) * 0.5f);      /* receiver: mono on the left (HS) */
			R = 0;
		} else {
			L = toS32(l);
			R = toS32(r);
		}
		gRing[2 * pos] = L;
		gRing[2 * pos + 1] = R;
		if (++pos == gRingFrames) {
			pos = 0;
		}
	}
}

/* WriteMix of the Actuator: the peak per 2 ms becomes a CONT-mode drive level for the next cycle. */
static void
actuatorWrite(const Float32 *src, UInt32 frames)
{
	struct va_haptic h = { 0 };
	Boolean any = false;
	for (UInt32 i = 0; i < frames && h.count < VA_HAPTIC_STEPS; i += kEnvelopeStepFrames) {
		Float32 peak = 0.0f;
		for (UInt32 j = i; j < MIN(frames, i + kEnvelopeStepFrames); j++) {
			Float32 v = fabsf(src[j]);
			peak = v > peak ? v : peak;
		}
		uint8_t gain = peak < kEnvelopeFloor ? 0 : (uint8_t)MIN(0x80, (UInt32)(peak * 0x80) + 0x20);
		any |= gain != 0;
		struct va_haptic_step *last = h.count ? &h.steps[h.count - 1] : NULL;
		if (last != NULL && last->gain == gain) {
			last->ms += 2;
		} else {
			h.steps[h.count++] = (struct va_haptic_step){ .ms = 2, .gain = gain };
		}
	}
	if (!any && !gHapticOn) {
		return;                 /* silence, and the motor is already off */
	}
	if (h.count > 0 && h.steps[h.count - 1].gain != 0) {
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
		memset(main, 0, (size_t)frames * 2 * sizeof(Float32));
	} else if (op == kAudioServerPlugInIOOperationWriteMix) {
		if (dev == kObjCodec) {
			codecWrite((const Float32 *)main, frames, cycle->mOutputTime.mSampleTime);
		} else {
			actuatorWrite((const Float32 *)main, frames);
		}
	}
	return 0;
}
