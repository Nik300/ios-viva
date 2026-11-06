# Viva USB: standard persona + Viva Link (patch 0182)

Contract between the kernel (`bsd/dev/viva_usb.c`), the phone daemon `vivausbd` (tools/vivad/vivausbd.c),
the host tool (`tools/viva-link.py`) and the SpringBoard tweak (Viva Manager, gpu/app/tweak).

## Personas

| Persona | When | Device | Interfaces |
| --- | --- | --- | --- |
| `link` | from boot until vivausbd attaches, or always with `vivausbstd=0` | 1209:0002 "viva iOS link" | if0 = Viva Link (as before) |
| `std` | after vivausbd's `VIVAUSB_ATTACH` ioctl | 05ac:12a8 "iPhone", serial = UDID given by vivausbd | if0 PTP, if1 usbmux, if2 Viva Link |

`std` configuration (bConfigurationValue 1, one configuration, 500 mA):

| Interface | Class/Sub/Proto | Endpoints | String |
| --- | --- | --- | --- |
| 0 PTP | 0x06 / 0x01 / 0x01 | 0x82 bulk IN, 0x02 bulk OUT, 0x83 interrupt IN (64, interval 8) | "PTP" |
| 1 usbmux | 0xff / 0xfe / 0x02 | 0x84 bulk IN, 0x04 bulk OUT | "Apple USB Multiplexor" |
| 2 Viva Link | 0xff / 0x56 / 0x01 | 0x81 bulk IN, 0x01 bulk OUT | "viva iOS link" |

Bulk max packet 512 (high speed) / 64 (full speed). The switch link -> std is a soft disconnect (~300 ms) and a
new enumeration: the host tool reconnects by itself.

## Viva Link "active" (tool present)

The link interface always exists, but the link counts as **active** only while the host tool says so:
vendor control request `bmRequestType 0x41` (host->device, vendor, interface), `bRequest 0x56`,
`wValue 1` = hello/heartbeat, `wValue 0` = bye, `wIndex` = the link interface number (0 in `link`, 2 in `std`),
no data stage. The tool sends a heartbeat every 1 s; the kernel drops "active" 4 s after the last one, on bye,
on bus reset and on disconnect.

Effects of "active":
- charger (VivaBattery.cpp): input limit 500 mA only while active; otherwise (wall charger, or a computer without
  the tool) REG00 IINLIM = `vivachargein` (2000 mA default), VINDPM protects weak sources.
- `sysctl kern.viva_linkstate` (int, read only, everybody): 0 = no USB host, 1 = host configured the device
  (standard), 2 = Viva Link active. The SpringBoard tweak polls it (1 s) and shows the island on 1/2 -> 2.
- the console log is still streamed to whoever reads the link IN endpoint (no change).

## Function nodes for vivausbd (`std` persona)

`/dev/vivaptp` and `/dev/vivamux` (root 0600, created once the root is mounted). One opener each.

- `read()` returns the bytes of **one** OUT transfer (a transfer ends with a short packet or a ZLP); if the buffer
  is smaller, the rest of that transfer comes with the next read(s). Blocks until data. Returns -1/`ECONNRESET` once
  after a bus reset, disconnect, or SET_CONFIGURATION 0 (the daemon drops all session state: PTP session, usbmux
  connections), then blocks again.
- `write()` = **one** IN transfer (the kernel adds a ZLP when the length is a multiple of the max packet size);
  at most 64 KiB per write; blocks while the kernel queue (128 KiB per function) is full. Returns `ENXIO` while the
  host has not configured the device.
- ioctls (on either node):
  - `VIVAUSB_SET_SERIAL` = `_IOW('V', 1, struct vivausb_serial { char s[64]; })`: serial number string of the `std`
    persona (the UDID; usbmuxd keys devices on it). Before ATTACH.
  - `VIVAUSB_ATTACH` = `_IO('V', 2)`: switch to the `std` persona (no-op if already there). `EPERM` with `vivausbstd=0`.
  - `VIVAUSB_STATE` = `_IOR('V', 3, int)`: 0 no host, 1 configured, 2 configured + Viva Link active.
  - `VIVAUSB_PTP_EVENT` = `_IOW('V', 4, struct vivausb_event { uint32_t len; uint8_t data[64]; })`: queue one PTP
    event on the interrupt endpoint (ObjectAdded etc.; optional).
- PTP class requests on ep0 are answered by the kernel: GET_DEVICE_STATUS (0x67) = `04 00 01 20` (OK) or
  `04 00 19 20` (busy) while a cancel is pending; CANCEL_REQUEST (0x64, 6 data bytes) flushes the PTP queues and makes
  `read()` on /dev/vivaptp return `ECANCELED` once; DEVICE_RESET (0x66) the same. GET_EXTENDED_EVENT_DATA stalls.
- Microsoft OS descriptor: string 0xEE = "MSFT100" with vendor code 0x50; vendor request 0xC0/0x50 wIndex 4
  returns the compat ID "MTP" for interface 0 (Windows binds its MTP driver).
- Apple vendor requests the Mac sends to iPhones (0x40 bRequest 0x40 "extra current", 0x40/0x41..) are acked
  without effect; 0xC0 0x45 (get mode) stalls.

## Boot-args

- `vivausb=3` link (unchanged); `vivausbstd=0` never leave the `link` persona (old behaviour); default 1.
- `vivachargein` unchanged (0 = never raise the input limit).
