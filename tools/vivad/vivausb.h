/*
 * vivausb.h: ioctls of the viva USB function nodes /dev/vivaptp and /dev/vivamux
 * (bsd/dev/viva_usb.c, patch 0182; contract in docs/VIVA-USB-PERSONA.md).
 * Shared by the kernel and tools/vivad/vivausbd.c: keep both copies identical.
 */
#ifndef VIVAUSB_H
#define VIVAUSB_H

#include <stdint.h>
#include <sys/ioccom.h>

struct vivausb_serial {
	char s[64];             /* NUL-terminated serial string of the std persona (the UDID) */
};

struct vivausb_event {
	uint32_t len;           /* bytes used in data */
	uint8_t  data[64];      /* one PTP event container (interrupt IN, endpoint 0x83) */
};

#define VIVAUSB_SET_SERIAL  _IOW('V', 1, struct vivausb_serial)  /* before ATTACH */
#define VIVAUSB_ATTACH      _IO('V', 2)                          /* switch to the std persona; EPERM with vivausbstd=0 */
#define VIVAUSB_STATE       _IOR('V', 3, int)                    /* 0 no host, 1 configured, 2 configured + Viva Link active */
#define VIVAUSB_PTP_EVENT   _IOW('V', 4, struct vivausb_event)   /* queue one PTP event (optional) */

#define VIVAUSB_STATE_NOHOST     0
#define VIVAUSB_STATE_CONFIGURED 1
#define VIVAUSB_STATE_LINK       2

#endif /* VIVAUSB_H */
