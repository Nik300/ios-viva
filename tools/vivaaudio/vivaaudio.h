/*
 * The VivaAudio kernel interface (iokit/Kernel/arm/VivaAudio.cpp, patch 0223), shared by the HAL plug-in.
 * Keep in sync with the kernel's copy.
 *
 * IOServiceOpen(IOServiceMatching("VivaAudio"), type 0):
 *   IOConnectMapMemory64 type VA_MAP_STATUS: struct va_status (read-only); VA_MAP_RING: the playback ring,
 *   ringFrames frames of frameBytes (0228: 8 = 2 x int32, sampleFormat VA_FORMAT_S32 = 24 bits MSB-aligned;
 *   0224-0227: 4 = 2 x int16; the 0222/0223 kernels said 8 but read 16-bit too),
 *   left = receiver / top speaker, right = loudspeaker, write-combined.
 *   VA_UC_START (2 scalars: outputs VA_OUT_*, receiver scene 0/1), VA_UC_STOP, VA_UC_HAPTIC (struct va_haptic).
 */
#ifndef VIVAAUDIO_H
#define VIVAAUDIO_H

#include <stdint.h>

struct va_status {
	uint64_t seq;           /* odd while the interrupt updates frames/hostTime */
	uint64_t frames;        /* frames played at the last period interrupt (multiple of periodFrames) */
	uint64_t hostTime;      /* mach_absolute_time of that interrupt */
	uint64_t startSeed;     /* +1 at each start */
	uint32_t rate, channels, frameBytes, periodFrames, ringFrames, running, outputs, irqs;
	uint32_t sampleFormat;  /* 0228: VA_FORMAT_S32 = 24 bits MSB-aligned in S32_LE; 0 = older kernels (16-bit) */
};
#define VA_FORMAT_S32           1

/* Vibration envelope: `count` steps of `ms` milliseconds at drive `gain` (0 = off, 0x80 = full); replaces what
 * is playing without stopping the motor between two non-zero steps; the last step ends the envelope. */
#define VA_HAPTIC_STEPS         32
struct va_haptic_step {
	uint16_t ms;
	uint8_t  gain;
	uint8_t  reserved;
};
struct va_haptic {
	uint32_t count;
	struct va_haptic_step steps[VA_HAPTIC_STEPS];
};

#define VA_UC_START             0
#define VA_UC_STOP              1
#define VA_UC_HAPTIC            2
#define VA_MAP_STATUS           0
#define VA_MAP_RING             1
#define VA_OUT_RCV              1
#define VA_OUT_SPK              2

#endif
