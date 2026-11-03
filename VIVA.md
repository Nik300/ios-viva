# ios-sweet on viva (Redmi Note 11 Pro 4G, MediaTek Helio G96 / MT6781)

Branch `viva` of this fork. Everything sweet-specific stays as is; viva support
is added next to it (`--viva` in `tools/mkadt.py`, `shim/viva.c`,
`shim/build-viva.sh`, `xnu/patches/0090-viva-*.patch`, `tools/viva-*`).

## Device facts (measured on the phone, 2026-11-02)

| What | Value |
| --- | --- |
| Boot | LK; `fastboot boot` is accepted but ignored. Flash `boot_b` (active slot) |
| Kernel format | **gzip only**. A raw Image is rejected (reboot after ~10 s) |
| boot.img | header v2, page 2048, DTB inside, cmdline `bootopt=64S3,32N2,64N2` (keep it) |
| Entry | EL1, MMU off, `x0` = DTB at 0x4bc80000; ramdisk at 0x47c80000 |
| PSCI | 1.1 via SMC; `SYSTEM_OFF` hangs instead of powering off |
| Watchdog | toprgu 0x10007000, stop with `WDT_MODE = (mode & ~1 & 0xffffff) \| 0x22000000` |
| Panel | `dsi_k6t_38_0c_0a_dsc_vdo`, 1080×2400, **video mode**, 59.8 Hz |
| Framebuffer | `/chosen/atag,videolfb`: 0x7bc10000, ARGB8888, **pitch 1088 px**, 3 buffers |
| RAM | 0x40000000–0x1c0000000 (6 GiB). All LK reservations below 0xc0000000, plus dramc-rk1 at 0x1bffff000 |
| XNU RAM | 0xc0000000–0x1be000000 (last 32 MiB block below dramc-rk1) |
| GICv3 | GICD 0x0c000000, GICR 0x0c040000; redistributor stride 0x20000 works (6-core boot) |
| Timer | 13 MHz, virtual timer PPI 11 = INTID 27 |
| CPUs | 6× Cortex-A55 (MPIDR 0x000–0x500), 2× big (0x600, 0x700). `cpus=6` boots to the lock screen; the big cores are untested |
| UFS | MediaTek ufshci at 0x11270000, left running by LK. CAP 0x1587071f (64-bit DMA, 32 slots). LUN 0/1: no GPT (preloader); **LUN 2**: GPT, `userdata` at block 0x2ff800, 109735 MiB (= fastboot's 0x1aca7f8000) |

## Safety

The LUN that holds `userdata` on MediaTek UFS also holds `nvram`, `proinfo`,
`seccfg`, etc. The `--viva` ADT has **no `/sweet-ufs` node**, so the kernel
does not touch storage at all, and `tools/viva-mkboot.sh` turns off every
sweet Qualcomm driver (keys, touch, USB, persistent log) whose fixed MMIO
addresses are other blocks on MT6781. Only `boot_b` is ever flashed.

## Build and boot

```sh
# kernel (GitHub Actions, macos-14)
gh workflow run xnu.yml -R YOU/ios-viva --ref viva
gh run download RUN_ID -R YOU/ios-viva -n xnu-kernel -D xnu/out
# boot image (stock boot.img from the viva fastboot ROM, EEA OS1.0.3.0.TGDEUXM)
tools/viva-mkboot.sh xnu/out/kernel.development.sweet STOCK/boot.img STAGE
fastboot flash boot_b shim/out/viva-boot.img
fastboot reboot
```

`STAGE` goes one step at a time:

| Stage | ADT | Kernel writes | Expected result |
| --- | --- | --- | --- |
| `nostorage` | no UFS node | none | panic `ufsroot0 unavailable: 6` (reached 2026-11-02) |
| `ufs-ro` | UFS node | refused (no `ufsrw`) | `SWEET UFS: "userdata" on LUN 2 ... read-only` (reached 2026-11-02, CI run 41529510653) |
| `ufs-rw` | UFS node | only inside `userdata` | iOS boots from the HFS+ root flashed into `userdata` |

Patch 0091 can discover the LUN by reading the GPT of LUNs 0-7; on viva it is
LUN 2, so `viva-mkboot.sh` passes `ufslun=2`.

### iOS root image (macOS, no Linux needed)

```sh
python3 tools/viva-extract-dt.py IPSW          # D22AP /product for mkadt.py (research/, not in git)
sudo tools/viva-mkiosroot-macos.sh IPSW ios16-root.img 16g
python3 tools/viva-img2simg.py ios16-root.img ios16-root.simg
fastboot flash userdata ios16-root.simg        # touches only userdata (Android data is lost)
```

IPSW: iPhone10,3 16.7.16 (20H392) from Apple. The image is built with
macOS's APFS/HFS+ and GNU rsync (`brew install rsync`), which writes the
files decompressed: the viva kernel has no AppleFSCompression kexts.

`shim/build-viva.sh STOCK/boot.img` without a payload builds a shim-only image
that stops at "no SWEETXNU payload": a quick check of the shim and of LK's
initrd properties.

## Milestones

1. ✅ Own code runs (gzip Image), framebuffer drawable, text console, live RAM map.
2. ✅ XNU boots to root mounting (2026-11-02, CI run 41525615211, 1 CPU): panics as designed with
   `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6` because the `--viva` ADT has no UFS node.
3. ✅ Storage: MediaTek UFS from LK's state, LUN 2 (2026-11-02, CI run 41529510653).
4. ✅ **iOS 16.7.16 lock screen** (2026-11-02, ufs-rw stage, ~20 min first boot): backboardd and
   SpringBoard up, black wallpaper, red date/time, static "charging" battery, gesture bar.
   No input yet (sweet's Qualcomm key/touch drivers are off).
5. ✅ **USB link** (2026-11-03, patch 0093, `vivausb=3`, `out/xnu-ufs-rw-link.img`): enumerates at
   high speed, live console from the first kernel line, `ping`/`stat` answer. Host:
   `tools/viva-link.py -o FILE`. Measured after the PHY wake: DTM0 0x02000000, DTM1 0x00003f2f,
   POWER 0x70 (HS), DEVCTL 0x99.
6. ✅ **Keys** (2026-11-03, patches 0094-0096, `out/xnu-ufs-rw-keys.img`): power, volume up (MT6366 PMIC
   PWRKEY/HOMEKEY via pwrap + EINT 144) and volume down (keypad) are HID consumer events through
   SweetGIC/VivaEINT interrupts, no polling; iOS reacts (screen on/off, volume HUD).
7. Touch (Goodix GT9896 on MTK I²C0, EINT 13), then battery/charger (PMIC BM/chrdet), SMP/perf, GPU
   (Metal driver on Mesa Panfrost, see the GPU plan).

## Handoff (2026-11-02, end of session)

State: iOS 16.7.16 reaches the lock screen on viva (6 cores, `out/xnu-ufs-rw-smp.img`
in `~/Developer/ios-viva/out/`). No input devices yet. Kernel artifacts: CI run
41537557007 (patches up to 0092).

USB probe result (patch 0092, `vivausb=2`, cable attached):
`cg 00bf8000 00000800 00000001 00000180 | phy acr0 00463c6e acr6 00100442 acr4 0000001a
dtm0 02040000 dtm1 00000000 | musb hw e503 ep 88 fa 00 pw 20 dc 80 ie 06 tx 01ff rx 01fe l1 0`.
Reading: MUSB clocked, peripheral mode, soft-disconnected (POWER.SOFTCONN=0), 8 TX + 8 RX
endpoints; T-PHY u2 forced into suspend (DTM0 FORCE_SUSPENDM). No clock needs enabling.

Patch 0093 "viva link" (`vivausb=3`, written 2026-11-02, CI run 41541439190 OK, image `out/xnu-ufs-rw-link.img`, works on the phone 2026-11-03): wakes the T-PHY (MediaTek
usb_phy_recover sequence, DTM1 forced to device: VBUSVALID/AVALID/BVALID/IDDIG, SESSEND=0),
MUSB peripheral in PIO (no DMA), interrupt line masked (L1INTM=0), a kernel thread polls
every 1 ms. Device 1209:0002 "viva iOS link", vendor interface, bulk 0x81 IN / 0x01 OUT.
IN frames `'V' type len16 payload`: `L` console ring from the start of the boot, `R` reply,
`E` end of reply. Commands: ping, stat, help, peek PA [WORDS], poke PA VALUE, rewind [BYTES],
panic [TEXT], reboot (toprgu WDT_SWRST). It starts in `sweet_ufs_rootdev()` *before* UFS, so it
also works in the `nostorage` stage and when UFS fails; it does not run during a panic yet.
Host: `tools/viva-link.py [-o FILE] [log|cmd CMD...]` (pyusb + Homebrew libusb).

Build: `VIVA_EXTRA="cpus=6 vivausb=3" tools/viva-mkboot.sh KERNEL STOCK ufs-rw`.

If it does not enumerate, things to check in order: the `VIVA LINK: phy ...` / `connected (power
.. devctl ..)` lines on screen (POWER should read 0x60 or 0x70, DEVCTL bit 0 set); then bus
reset lines; DTM0 bit 25 (unknown, left as LK set it); MediaTek-specific registers not touched
(0x80/0x84 data toggle, PHY tuning). Then keys (MT6366 PMIC via pwrap), touch (Goodix on MTK I²C).

Patch 0094 (CI run 41546086814, image `out/xnu-ufs-rw-gic.img`, untested): SweetGIC = GICv3 SPIs as an
IOInterruptController (nub /arm-io/gic, ADT `#interrupt-cells` 2 = `<INTID flags>`), handler on CPU 0, all
SPIs routed to CPU 0. First consumer VivaKeypad (kp@10010000, SPI 73 edge, IOInterruptEventSource), logs
`VIVA KP: bit N down/up`; the ADT `keymap` (`<bit usage>` pairs) is still empty.

0094 result (2026-11-03): SweetGIC works (512 INTIDs, keypad INTID 105 edge, interrupts delivered to the
work loop). Volume down = keypad bit 0 (mirrored in bits 9, 34, 43, 68, 77, 102, 111). Power and volume up
are PMIC keys (live DTB `kpd-hw-pwrkey`/`kpd-hw-rstkey`, CONFIG_KPD_PWRKEY_USE_PMIC): MT6358/MT6366
TOPSTATUS 0x28 bit 1 PWRKEY_DEB, bit 3 HOMEKEY_DEB (0 = pressed); PSC_TOP_INT_CON0 0x910 / STATUS0 0x91c
bits 0-3 = pwrkey, homekey, pwrkey_r, homekey_r; PMIC interrupt = pio EINT 144 -> GIC SPI 186.
Reference source: github.com/MiCode/Xiaomi_Kernel_OpenSource branch viva-r-oss (MTK_PLATFORM mt6785,
drivers/soc/mediatek/mtk-pmic-wrap.c pwrap_mt6781). Merged DTB from RAM: ~/Developer/ios-viva/dtb/viva-live.dtb.
Touch: Goodix GT9896 on i2c0@11007000 addr 0x5d, irq GPIO 13 (falling), reset GPIO 17, max 8640x19200.

Patch 0095 (CI run 41548885484, image `out/xnu-ufs-rw-pwrap.img`, untested): VivaPwrap, read-only WACS2
(no write path); USB link `pmic ADDR [N]`; keypad keymap <0 0xea>.

0095 result (2026-11-03): pwrap reads work (LK leaves WACS2 initialized), PMIC hwcid 0x6610 = MT6366,
TOPSTATUS 0x0e idle, 0x0c power held, 0x06 volume up held. LK leaves PSC interrupts enabled (con 0x0065)
with chrdet (bit 5) pending while USB is plugged.

Patch 0096 (CI run 41551405221, image `out/xnu-ufs-rw-keys.img`, works 2026-11-03: all keys, no stray PMIC/EINT interrupts; LK had LDO[0..1] and PSC interrupts on): VivaEINT (IOInterruptController
under SweetGIC, SPI 186), VivaPwrap as a bus (VivaDTNub children) with writePMIC() limited to interrupt
enable SET/CLR + status registers, VivaPMIC (all PMIC interrupts off at start, EINT 144 on its work loop),
VivaPMICKeys (power, volume up). ADT: /arm-io/eint, /arm-io/pwrap/pmic/keys.

Touch path (from Xiaomi viva-r-oss + live DTB, 2026-11-03): driver goodix_gtx8 (CONFIG_TOUCHSCREEN_MTK_TOUCH),
GT9896 "Normandy" protocol: 16-bit register addresses, event head at 0x824E (byte 0 bit 7 = touch event,
byte 1 low nibble = finger count), 8 bytes per finger (id, x16 LE, y16 LE, w, ...), checksum = byte sum
over 4 + 8n bytes == 0, then write 0 to 0x824E; version at 0x8240; reset = GPIO17 low 2 ms, high, 110 ms.
I2C0 0x11007000 (SPI 105 level), clock: infra1 CG bit 29 (on at boot) + imp_iic_wrap 0x11017e00 bit 0.
`tools/viva-touch-probe.py` reads gates and pin states (peek only).

Patch 0097 (CI run 41554971700, image `out/xnu-ufs-rw-touch.img`, untested): VivaGPIO (output-pins
whitelist 17/73/166), VivaI2C (I2C0 v2 FIFO, 400 kHz from 124.8 MHz / 5; probe 2026-11-03: gates on,
SCL0/SDA0 already mode 1, timing registers 0 = LK never used I2C0), VivaGT9896 (touch was unpowered:
avdd/dvdd/reset outputs at 0), dispatching through SweetHIDTouch.

0097 result (2026-11-03): I2C0 and power-on work (the IC ACKs, its INT line toggles, EINT 13 correct) but
no touches: this GT9896 is a *Yellowstone* IC, not Normandy. The register map is in the vendor's
/vendor/firmware/goodix_cfg_group.bin (super.img -> vendor_a ext4; tools/viva-super-extract.py, debugfs,
tools/viva-goodix-cfg.py): coor 0x4180, pid 0x4022, version 0x4014 (135 bytes), command 0x4160, esd 0x4166,
cfg_addr 0x96f8, cfg_send_flag 0x4034. Yellowstone events: 8-byte head (byte 0 bit 7 touch, bit 6 request,
byte 2 = contacts, BE16 sum in bytes 6-7), 8 bytes per contact (id << 4 | type, x/y/w BE16), BE16 contact sum.

Patch 0098 (CI run 41557214919, image `out/xnu-ufs-rw-ys.img`, works 2026-11-03: touch OK on the lock screen): VivaGT9896 on the Yellowstone protocol (registers from the ADT); request events logged/acked
(config requests are not answered yet: the IC keeps its flash config); USB link `i2c DEV REG N` and
`i2cw DEV REG B..` for live exploration.

Patch 0100 "viva GPU" (2026-11-03, CI run 41569069049, image `out/xnu-ufs-rw-gpu.img` with vivagpu=1, untested; kernel side of `~/Developer/ios-viva/gpu/uapi`, phases G1-G2 of
`docs/PIANO-GPU.md`): `VivaGPUAccelerator : IOAcceleratorES` on ADT `/arm-io/gpu` ("mediatek,mt6781-mali",
reg = Mali, mfgcfg, SPM, topckgen, apmixed, infracfg_ao; SPI 274-276 GPU/MMU/JOB) + `VivaGPUUserClient`
(viva_gpu_uapi.h: BOs, mmap, submit, WAIT_BO, syncobjs, IOSurface/user-memory import, SYNC_BO).
- MFG power (facts from viva-r-oss gpufreq_v1/mt6781 + clk-mt6781-pg, summary in the session's
  MT6781-GPU-POWER.md, reproduced here): MT6366 VSRAM_GPU (EN 0x1bc8.0, VOSEL 0x1bf4[6:0], DA 0x1be4/0x1be2[14:8])
  then VGPU (EN 0x1508.0, VOSEL 0x1526[6:0], DA 0x1520/0x151e), 0.5 V + 6.25 mV/step; MFGPLL apmixed
  0x250/0x254/0x25c, 300 MHz = CON1 0x831713b1; mfg_sel CLK_CFG_0[17:16] (SET 0x44 CLR 0x48, UPDATE 0x004 bit 2),
  1 = mfgpll, 3 = mainpll_d5 (park); SPM key 0x0b160001, MFG0-3 PWR_CON 0x308 + 4n, status 0x16c/0x170 bit 2+n;
  MFG1 bus protection infracfg_ao SET 0x2a0/0x2a8, CLR 0x2a4/0x2ac, STA 0x228/0x258, masks 0x18000000 (1),
  0x00600000, 0x02000000, 0x20000000 (1); mfgcfg CG CLR 0x008 bit 0 + async/global/qchannel setup; ACP off
  (infracfg_ao 0x290 bit 9). Fixed OPP: 300 MHz, VGPU 0.65 V, VSRAM 0.85 V. VivaPwrap::writeRegulator() only
  writes those four PMIC fields (VGPU <= 0.9 V, VSRAM <= 1.0 V).
- Mali: soft reset, L2/stack/tiler/shader power, TLS hash + IDVS quirks, COHERENCY_NONE; AArch64 4K page tables
  (48-bit, MEMATTR byte 0 = 0x8f), one AS per connection (LRU over AS_PRESENT); slot 0 fragment, slot 1 the rest,
  one chain per slot, flush clean+invalidate at start and end; heap BOs grow 2 MiB per translation fault; other
  faults disable the AS and fail the chain; 3 s timeout -> hard stop -> soft reset.
- Boot-arg `vivagpu`: 0 (default) off; 1 started, unpowered (USB link `gpu ...`); 2 power at boot + publish.
  Link: `gpu [status|mfg|on|off|regs|selftest [fault]|publish]` (`mfg` is read-only).
- First test (2026-11-03, run 41569069049): `gpu mfg`: the preloader leaves VGPU 0.9 V and VSRAM_GPU 1.0 V on,
  MFGPLL on at 300 MHz (con1 0x031713b1), mfg_sel = 1, MFG0-3 off. `gpu on` WORKS: rails to 0.65/0.85 V, MFG0-3 on,
  GPU_ID 0x90930010 (G57 r0p1), shader 0x5, l2 0x1, stack 0x5, AS 0xff, JS 0x7, MMU_FEATURES 0x2830 (48-bit VA),
  COHERENCY_FEATURES 0x1, L2_FEATURES 0x08130206, THREAD_FEATURES 0x48000, JS features 0x20e/0x1c9e/0x1e.
  `gpu selftest` panicked: `phystokv_range: illegal PA 0x6860c000; phys base 0xc0000000` - on this port the
  physical aperture only covers RAM from 0xc0000000 while kernel allocations also come from below: never use
  phystokv; cache maintenance and page tables go through kernel mappings (fixed in the next build).
- Not yet: DVFS, runtime power gating, IOSurface cache flush cost (whole surface per job), performance counters.

Workflow notes:
- Kernel patches are made against a local xnu-8796.141.3 tree with all
  `xnu/patches/*.patch` applied in order (`git apply`; 0031/0041 need the CI's HFS
  staging script and can be skipped locally). CI silently skips a patch that does not
  apply, so check each new patch with `git apply --check` on that tree.
- Single files compile-check locally with `tools/xnu-cc.py LOG BUILDTREE FILES` against the
  `xnu-buildtree` artifact (it holds the patched source: edit there, `git diff` = next patch)
  and the CI log (`gh run view RUN --log`); CI_DXB/CLANG in it are set for this fork.
- Build: `gh workflow run xnu.yml -R Nik300/ios-viva --ref viva` (~8-10 min), then
  `tools/viva-mkboot.sh KERNEL ~/Developer/ios-viva/stock/boot.img STAGE`; extra boot-args via
  `VIVA_CMDLINE`. Debug features stay compiled in and are switched by boot-args.
