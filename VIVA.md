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
| CPUs | 6× Cortex-A55 (MPIDR 0x000–0x500), 2× Cortex-A76 (0x600, 0x700), all PSCI CPU_ON (live DTB). `cpus=6` boots to the lock screen; `cpus=8` (0110) untested |
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

Patch 0100 "viva GPU" (2026-11-03, CI run 41569069049, image `out/xnu-ufs-rw-gpu.img` with vivagpu=1, works; kernel side of `~/Developer/ios-viva/gpu/uapi`, phases G1-G2 of
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
- Second test (run 41570731376): `gpu selftest` PASS (WRITE_VALUE 0xc0ffee00, status 0x01, 0 ms). `gpu selftest
  fault` panicked inside XNU: IOBufferMemoryDescriptor with kIOMapWriteCombineCache -> vm_object_change_wimg_mode ->
  pmap_set_cache_attributes -> flush_dcache(phys) -> phystokv_range: illegal PA 0x6a01c000. **Port bug**: RAM below
  0xc0000000 is VM-managed (pa_valid) but outside the physical aperture (ptov_table), so changing any such page's
  cache mode (WC/uncached buffers, any driver) panics. Workaround in 0100: all BOs cacheable, the kernel flushes
  every BO of a job before it and invalidates after it. Real fix (later): cover low RAM in the aperture.
- Third test (run 41572312019, image `out/xnu-ufs-rw-gpu.img`, vivagpu=1), **G1 + G2 kernel side WORK**:
  `gpu on` twice (after `gpu off`, rails really off/on), `selftest` PASS (<1 ms), `selftest fault`: translation fault
  (write) on AS0 caught, the chain fails with 0x48 (job bus fault: the AS is disabled under it) -> TTRX_3076 reset
  (316 ms), next `selftest` PASS, counters consistent. To improve: a user MMU fault always costs a full reset.
  Next: userspace on the phone (`gpu/ios/build.sh` needs Xcode: test_vgpu with `gpu publish`, then the Metal bundle).
- Not yet: DVFS, runtime power gating, IOSurface cache flush cost (whole surface per job), performance counters.

Patches 0101-0109 (2026-11-03, built: CI run 41580171845, image `out/xnu-ufs-rw-0109.img` with
`cpus=6 vivausb=3 vivagpu=2`, vivad in the xnu-kernel artifact; NOT tested on the phone yet): the userspace requests in `~/Developer/ios-viva/gpu/docs/RICHIESTE-KERNEL.md`.
Build with `VIVA_EXTRA="cpus=6 vivausb=3 vivagpu=2"` (user request: vivagpu=2 from now on).
- 0101 (K1) unknown user-client types on VivaGPUAccelerator get a `VivaGPUProbeClient` (boot-arg `vivagpuprobe=0`
  refuses them as before): every selector/trap/memory type/notification logged (48 lines per client) and answered
  kIOReturnUnsupported; userspace property reads of the accelerator logged once per (process, key) with
  found/MISSING (hooks in IOUserClient.cpp). `gpu probe` lists both tables.
- 0102 (K5) a bus fault (0x48) on a chain whose AS we just disabled for an MMU fault no longer resets the GPU; any
  other bus fault still does (TTRX_3076). Until a fragment chain completes again, JS0 chains time out after 500 ms.
  `gpu selftest fault` now also says "no reset".
- 0103 (K8) GPU memory per process (connections of one pid summed): `vivagpumem` (MiB, default 1024) per process,
  `vivagpumemtotal` (default 2048); over the limit CREATE_BO/heap growth/page tables fail with kIOReturnNoMemory.
  `gpu status` lists processes (KiB, BOs, imported KiB, connections, MMU faults) and peak/refusals.
- 0104 (K3) boot-arg `vivagpucache`: 1 (default) normal BOs get cache maintenance only once userspace maps them
  (clientMemoryForType); IOSurface BOs are lazy: cleaned before a job only after a CPU write lock
  (IOSurfaceLock/Unlock traps set `cpuDirty`), and after a job only marked `gpuDirty` (dropped at the next
  IOSurfaceLock or before AppleCLCD copies them). **Contract: CPU access to an IOSurface the GPU uses must be inside
  IOSurfaceLock/Unlock** (or the BO mapped through VivaGPUUserClient, which makes it eager). 0 = old behaviour.
  `gpu status`: average submit time, maintenance time and KiB per submit/completion. The low-RAM aperture fix is
  NOT done (needs a diagnosis on the phone: why kernel pages come from below 0xc0000000).
- 0105 (K2) AppleCLCD converts 'w30r', 'w40a'/'w40m' (XR10, (v-384)/510, clamped) and 'l10r' to BGRA8 while copying;
  every layer's format change is logged (`SWEET FB: layer N format ...`). tools/mkadt.py --viva also sets /product
  artwork-display-gamut = "sRGB" (was the iPhone X's "P3"); MobileGestalt may keep a cached value.
- 0106 (K6) IOSurface values: sel 9 SetValue ([value,key] or [dict] merge), 10 copy (descriptor out {len, gen, binary
  dict}), 11 RemoveValue (empty key = all), 27 bulk attachments into state+0x30; generation (g+2)|1 at state+0x20,
  serialized size (>= 1) at state+0x28. ABI confirmed from 20H392 IOSurface.framework disassembly.
- 0107 (K4) vendor OPP table (gpufreq_v1/mt6781 segment 0; this phone's devinfo[69] bits 29:28 = 0): boot-arg
  `vivagpufreq=MHz` (default 300, max 950), `vivagpumv` margin on VGPU (default 50, capped at 0.9 V), VSRAM >=
  VGPU + 0.1 V; rails stepped 50 mV at a time; MFGPLL CON1 computed (300 MHz = 0x831713b1 as before). Runtime:
  `gpu freq [MHz]`.
- 0108 (K7) link: `push SIZE MODE FNV PATH` + raw data (file written to PATH.vivapush, renamed if FNV-1a matches,
  parents created), `pull PATH` ('F' frames), `run ARGS..` / `vivad ps|kill ID|tests` through /dev/vivalink to the
  userspace daemon `tools/vivad` (built by CI into the xnu-kernel artifact), its output as 'O' frames. Host:
  `tools/viva-link.py push|pull|run`. Install once: pull /System/Library/xpc/launchd.plist,
  `tools/viva-launchd-add.py` it, push it and vivad (/usr/libexec/vivad 755), reboot. vivad runs
  /private/var/root/viva-tests/autorun/* at boot.
- 0109 (K9) direct scanout, off by default (`vivascanout=1` or `disp scanout on`): display-sized BGRA surfaces are
  allocated contiguous below 4 GiB; a swap with one full-screen such surface rewrites only ADDR/PITCH of LK's OVL
  layer (found by its address), else LK's values are restored and the CPU copy runs. No vblank sync yet (tearing
  possible). `disp regs` is a read-only dump of OVL0/OVL0_2L/larb ports: run it first (larb port bit 0 set would
  mean the M4U translates: then scanout cannot work with physical addresses).

Patch 0110 "big cores" (2026-11-03, CI run 41584638951, image `out/xnu-ufs-rw-0110.img`, NOT tested on the phone yet): `cpus=8` brings up the two A76s too (the
kernel already allowed 8 since sweet's 0078; mkadt emits cpu6/cpu7 = MPIDR 0x600/0x700, GICR found by
affinity, set/way ops read CCSIDR per core). 0110 adds the Cortex-A76 erratum 1286807 / A55 2441007
workaround (sync_tlb_flush: DSB, dummy `tlbi vale1is, xzr`, DSB, like Linux REPEAT_TLBI) and logs each core's
MIDR (`SWEET CPU: cpu=6 mpidr=0x600 midr=0x414fd0b. (A76 rNpM) came alive`). Still one homogeneous cluster: the
scheduler does not prefer the A76s (AMP/P-E clusters = `__ARM_AMP__`, a much larger change).
`tools/viva-mkboot.sh` now defaults to `VIVA_EXTRA="cpus=8 vivausb=3 vivagpu=2"`. If a big core fails:
`SWEET CPU: cpu=6 PSCI CPU_ON rc=N` (firmware refused) or `NOT alive after 5s` (entry/MMU/GIC); fall back
with `VIVA_EXTRA="cpus=6 vivausb=3 vivagpu=2"`.

Patches 0111-0113 (2026-11-03, CI run 41593631050, image `out/xnu-ufs-rw-0113.img` with `cpus=8 vivausb=3 vivagpu=2` + sweetfb_noconsole=1; NOT tested on the phone yet):
- Symptom (link-gpu8.log, 0109-quiet): SpringBoard/backboardd frames with whole tiles black or holding old pixels.
  Each one is a fragment chain killed by a GPU MMU read fault (status 0x48 after `MMU fault ... translation fault,
  read access`): the tiles after the fault are never written. Faulting addresses were unmapped VAs (0x16603900,
  0x3e1f3980, ...) and plain garbage (0xffffe61e3800, outside Panfrost's 4 GiB range): job descriptors with stale
  pointers.
- Cause: after every job the kernel dropped the CPU lines of each CPU-mapped BO with `dc ivac`. Mesa keeps writing
  other parts of the same pool BOs (descriptors for the next draws) while a job runs; `ivac` throws those dirty
  lines away, so the GPU later reads whatever was in DRAM. **0111**: clean + invalidate (`dc civac`) everywhere,
  plain invalidate removed from the driver; SYNC_BO maps its types explicitly. MMU fault lines now say what the
  address is: `inside live VA [..) PTE ..` (kernel mapping bug), `in VA [..) freed N ms ago` (userspace used a
  closed BO), `outside the VA range (bad pointer)` or `not allocated`, plus the process. `vivagpucache=2`
  (diagnosis): every CPU-maintained BO of the connection is cleaned with every job, listed in bo_handles or not.
- **The "physical aperture" diagnosis of 0100 was wrong.** XNU RAM is 0xc0000000-0x1be000000 as configured;
  `illegal PA 0x6860c000` was 0x16860c000 cut to 32 bits by `pmap_sync_page_attributes_phys` (`pp << PAGE_SHIFT`
  on a 32-bit ppnum_t; Apple's coherent-IO SoCs never reach it). **0112** uses ptoa(). Write-combined / uncached
  mappings (any driver) should work now; the GPU BOs stay cacheable with kernel maintenance for the moment.
- **0113** quiet screen: the on-screen text is the `fbcon` pseudo-serial (serial=3), which also gets every
  kprintf directly, so `sweetfb_noconsole=1` (mute from AppleCLCD's first present = the boot Apple) left text
  coming. fbcon now honours the mute too (a panic still draws). viva-mkboot.sh adds `sweetfb_noconsole=1` by
  default; link `console on|off` toggles it live; the link log is unaffected.

Patches 0114-0115 (2026-11-03, NOT tested on the phone yet; details and verification in
`~/Developer/ios-viva/gpu/docs/ANALISI-KERNEL-RENDERING.md`, answers in `gpu/docs/RICHIESTE-KERNEL.md`):
- **0114** (CI run 41694721285, image `out/xnu-ufs-rw-0114.img`): K0 implicit sync in `submit` (each job waits for
  the last job on each of its BOs; IOSurfaces carry their own last fence `SweetSurface::gpuFence`, shared across
  connections; WAIT_BO on a surface waits for other processes too). K0b: BOs without VGPU_BO_WB_MMAP are
  write-combined (Linux model, `vivagpuwc=0` = cacheable as before; relies on 0112), CPU-maintained BOs are no
  longer flushed in the completion IRQ but listed and invalidated when userspace waits (WAIT_BO, syncobj wait;
  `vivagpulazy=0` = old). K0b.3 (JS_HEAD_NEXT) not done: with the shared tiler heap the dependency chain alternates
  slots. K0c: AppleCLCD queues the last 4 swaps in order and shows the newest whose surfaces have no pending GPU job
  (`viva_gpu_surface_idle`), retries at the next vsync otherwise, redoes a copy if a job touched a surface meanwhile,
  shows the newest anyway after `sweetfb_gpuwait` ticks (12; 0 = off). IOSurfaceLock waits for the GPU (AvoidSync ->
  kIOReturnCannotLock). Link: `gpu status` (lazy invalidation, implicit dependencies), `disp` (GPU sync counters).
- **0115** (CI run 41696147588, image `out/xnu-ufs-rw-0115.img`): flush reduction (`JS_FLUSH_ID_NEXT` =
  GPU_LATEST_FLUSH at submit, `vivagpuflushid=0` off); DVFS governor every 50 ms (>85% busy -> `vivagpufreqmax`,
  default 950; else down to the OPP that would be 70% busy, not within 200 ms of a step up, never below
  `vivagpufreq`; `vivagpudvfs=0` off, link `gpu dvfs [on|off]`); panel refresh adaptive by default (copies within
  `sweetfb_duty`=25% of one CPU; `sweetfb_hz=N` = fixed as before), copy time in `disp`.
- 0114/0115 on the phone (2026-11-03): boot and animate fine, then panic in backboardd after a while:
  `__memmove_chk object size check failed ... (1132 < 3396)` = AppleCLCD::swap() shifting the full swap queue with
  one memmove from fQueue[0] (fortify sees one record). **0116** (CI run 41699672332, image
  `out/xnu-ufs-rw-0116.img`, = 0114+0115+fix) shifts one record at a time.
- If the GPU's first CREATE_BO panics in pmap (WC path, 0112 not enough): boot with `vivagpuwc=0`.
- 0116 on the phone (2026-11-03): no more black tiles / mixed frames in `vctl burst`. Left (gpu/docs/RICHIESTE-KERNEL.md):
  K0d copy tearing (1-2 of 8 frames split between two whole frames), K0e ~0.28 ms per job independent of the clock.

Patch 0117 (2026-11-03, CI run 41707729706, image `out/xnu-ufs-rw-0117.img`, NOT tested on the phone yet):
- **K0e fast kick**: the job IRQ is an IOFilterInterruptEventSource. After every kick the queue head of each slot
  whose unfinished deps are all jobs on the hardware is *armed* (AS bound, `vm->running` reserved, JS_CONFIG ready);
  the filter (primary interrupt context, `fFastLock` spinlock) clears the done bits, starts armed jobs whose awaited
  slots are done, and the gated handler (`fastSync`, first thing in every gated entry that looks at fRunning/fQueue)
  completes the done jobs at the IRQ time and takes the started ones over. Failures stay with the gated handler.
  `vivagpufastkick=0` / link `gpu fastkick off` = old path. Same-slot `JS_*_NEXT` queueing not done: the filter
  restarts the slot within microseconds anyway, without the "two jobs, one done bit" ambiguity.
  Measuring: `gpu lat` (also in `gpu status`; `gpu lat reset`): % of time GPU busy / jobs queued but none running /
  nothing queued, avg time on the hardware per job, start latency split (ready at submit / after a dependency via work
  loop / via filter), IRQ -> handler latency.
- **K0d double buffer**: copies go to the panel buffer not on screen (LK's 0x7bc10000 or LK's next one,
  fPhys + rowBytes*height = 0x7c606000) and LK's layer (OVL0_2L L0) is switched at the next frame end = OVL INTSTA bit
  1 (FME_CPL) rising, polled; calibrated once at the first swap (3 frame ends; if the bit does not latch, retried with
  INTEN bit 1 set, harmless: the GIC masks an unhandled line). The vsync timer is phase-locked: it fires
  `sweetfb_lead` (1000 us) before the predicted frame end; with a switch pending it polls (interrupts off from seeing
  the bit to the register writes, skipped if the poll was held up > 20 us). Partial frames, the default surface and the
  unmuted console go to the buffer on screen as before. Scanout (`vivascanout=1`) switches at frame ends too. A panic
  points the layer back at LK's buffer (`sweet_fb_panic_hook`, pe_serial.c). `sweetfb_flip=0` / link `disp flip off`
  = old copy; `disp` shows state, frame period, switches/missed/unsure; `disp regs` shows INTEN/INTSTA/ADDCON_DBG.
  If `disp` says "no frame-end status from the OVL", the double buffer is off and nothing else changed.

Patch 0118 (2026-11-03, K0g, NOT tested on the phone yet; analysis in `~/Developer/ios-viva/gpu/docs/ANALISI-KERNEL-RENDERING.md`,
"Seconda analisi"):
- After 0117 (userspace measurements, gpu/docs/FINDINGS): GPU busy ~1%, job on the hardware ~24 us, frame 18 ms, the
  time is in the SUBMIT (~60 us each, ~100 per frame) and WAIT_BO ioctls. Kernel-side cause found in the code: every job
  completion, every submit with an out_sync and every syncobj change woke **every** waiter of every process
  (`commandWakeup(&fFenceEvent)`), each woken thread retook the gate to find nothing, and submits queued behind them.
- **Targeted wakeups**: a wait for one fence (WAIT_BO, one-handle or WAIT_ALL syncobj waits, IOSurfaceLock /
  viva_gpu_surface_wait, selftest) sleeps on the fence (`sleepOn`, `VivaGPUFence::waiters`); the shared event only
  for ANY waits on several syncobjs or a syncobj without a fence, and woken only when such waiters exist. Replacing
  a syncobj's fence wakes the old fence's waiters, so they re-check as before (same semantics).
- **Trap fast path**: `IOConnectTrap2(conn, VGPU_TRAP_CALL=0, selector, &args)` = the struct method without the Mach
  message (copyin/copyout of the structure). UAPI minor 1. Userspace must adopt it (libdrm-xnu/vgpu).
- **Call counters**: link `gpu calls [reset]`: per selector calls (trap/method), avg/max time in the kernel, avg/max
  wait for the gate; sleeps, spurious wakeups, fence/generic wakeups. `gpu clocks`: ARMPLL_LL (A55) / BL (A76) / BB,
  CCIPLL, MFGPLL from apmixedsys (read only): nothing in this port sets CPU frequencies, they stay where LK left them.
- **Early present** (`sweetfb_early=1` default, link `disp early on|off`): a queued swap is tried when it arrives and
  whenever a GPU job that used IOSurfaces completes (`sweet_fb_gpu_done` -> AppleCLCD timer on its work loop), not
  only at the vsync tick: the copy goes off screen right away and is switched at the next frame end (one frame of
  latency less). `disp` shows tries / frames shown early.

0118 built: CI run 41731434461, image `~/Developer/ios-viva/out/xnu-ufs-rw-0118.img` (cpus=8 vivausb=3 vivagpu=2).

Patch 0119 (2026-11-04, CI run 41733230639, image `out/xnu-ufs-rw-0119.img` = 0118+0119; userspace requests K0h/K0i in gpu/docs/RICHIESTE-KERNEL.md, NOT tested on the phone yet; UAPI minor 2):
- **K0h.1 SUBMIT_BATCH** (selector 21): up to 64 `vgpu_submit` in one call, queued in order under one gate acquisition,
  exactly as N SUBMITs; `done` = how many were queued (stops at the first error); each element's `seq` written back.
- **K0h.2 completion page**: `vgpu_submit.seq` (was `pad`) = the job's number in its connection; IOConnectMapMemory64
  type `VGPU_MAP_COMPLETION` (0x10000) maps a read-only page `struct vgpu_completion`: every job with seq <= completed
  has finished (a watermark: the connection's unfinished jobs are kept in submit order, slots finish out of order).
  Not valid for other processes' jobs on a shared IOSurface.
- **K0i SURFACE_PROMISE** (selector 22): an unsignalled "promise" fence put into a syncobj and made the last fence of up
  to 16 IOSurfaces (IOSurfaceLock, K0c, WAIT_BO and other processes' jobs wait for it). The owner connection's jobs and
  WAIT_BOs on those surfaces use what came before it (no self-deadlock). Resolved when the syncobj's fence is replaced:
  TRANSFER into it / SUBMIT out_sync = it follows that fence (fence `followers`); RESET/SIGNAL/DESTROY/close = signals
  now; timeout (default 1 s, max 5 s, job timer) = signals with an error and logs. Not resolved by the first SUBMIT
  automatically: Panfrost uses one out_sync for all submits, so the driver must TRANSFER the final fence in.
  `gpu calls` shows promises made/pending/timed out.

Patch 0120 (2026-11-04, CI run 41738470270, image `out/xnu-ufs-rw-0120.img` = 0118-0120, NOT tested on the phone yet): absolute timeouts (WAIT_BO, SYNCOBJ_WAIT) were compared with
mach_continuous_time, but Mesa computes them with clock_gettime(CLOCK_MONOTONIC), which Darwin's Libc derives as
gettimeofday - kern.boottime (follows the calendar: timed/NTP set and slew it). Reported by the userspace agent: a
deadline 1 s ahead came back expired at once (driver workaround: completion waits without a finite timeout, gpu
7bfdc01). Now the kernel uses the same formula (`viva_gpu_monotonic_ns`: clock_get_calendar_microtime -
clock_get_boottime_microtime). `gpu clocks` prints both clocks and their difference.

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

Patch 0121 (2026-11-04, CI run 41742051361 OK, NOT tested on the phone yet): **battery, charger, Low Power Mode**.
- Hardware (Xiaomi viva-r-oss drivers/power/supply/mediatek/{battery/bq27z561.c,charger/syv690.c} + live DT): I2C3
  @0x1100f000 (SPI 108, 400 kHz, IMP_IIC_WRAP gate bit 3): **TI bq27z561** fuel gauge at 0x55 (LE16 standard commands:
  0x06 temp 0.1 K, 0x08 mV, 0x0a status, 0x0c/0x14 mA (+ = charging), 0x10/0x12 RM/FCC mAh, 0x16/0x18 TTE/TTF, 0x2a cycles,
  0x2c SOC %, 0x2e SOH %, 0x30/0x32 the gauge's charge voltage/current request, 0x3c design) and **Silergy SYV690**
  charger at 0x6a (BQ25890 map; REG14 PN 1 = SYV690 64 mA/step, 4 = SC89890H 60 mA/step), INT = EINT 11 falling.
  Charge pumps ln8000/sc8551 and the Type-C/PD controller are left alone (5 V charging only).
- VivaI2C is multi-bus now (`bus-id`, optional IMP gate in reg 1 + `imp-cg-bit`); ADT `/arm-io/i2c3/battery`.
- **VivaBattery** (`ti,bq27z561` on VivaDTNub) is the IOPMPowerSource (SweetBattery steps aside when it is in the ADT):
  AppleARMPMUPowerSource conventions (CurrentCapacity % / MaxCapacity 100, AppleRaw* mAh, ExternalConnected,
  IsCharging, FullyCharged, Voltage, Amperage, InstantAmperage, Temperature centi-C, CycleCount, TimeRemaining,
  NominalChargeCapacity, DesignCapacity, MaximumCapacityPercent = SOH, AdapterDetails from the charger's VBUS type and
  input limit, legacy battery dictionary). Poll every 15 s and 300 ms after each charger interrupt.
- Charger (boot-arg `vivacharge`, default 1): watchdog 160 s kicked at every poll, VREG = min(gauge request, 4448 mV),
  ICHG = min(gauge request, 3000 mA) (0 only if the gauge says full or the battery is outside 0..50 C); writes only
  REG03/04/06/07, read back. `vivacharge=0` = never write (then the charger falls back to 4.208 V once its watchdog
  expires: the battery stops at ~85 %). Charge limit: `vivachargelimit=N` / sysctl `kern.viva_chargelimit`.
- **Low Power Mode**: powerd posts `com.apple.system.lowpowermode` (state = mode); vivad (now started also without the link
  device) copies it into sysctl `kern.viva_lowpower`: GPU DVFS ceiling `vivalpmgpu` (550 MHz), panel copy duty
  `vivalpmduty` (10 %), "LowPowerMode" on the battery service. vivad must be rebuilt/re-pushed (xnu-kernel artifact).
- Link: `bat` (gauge + charger state and registers), `bat poll`, `bat lpm on|off`; `i2c8 BUS DEV REG N`,
  `i2c8w BUS DEV REG B..` (8-bit registers on any bus).

Patch 0122 (2026-11-04, K0j in gpu/docs/RICHIESTE-KERNEL.md; its own CI run was cancelled by the 0123 one, NOT tested on the phone yet): unsignalled fences keep their
dependencies; a connection with a pending promise never waits (WAIT_BO, implicit deps) for a fence that depends on that
promise, it waits for what that fence waited for; slot queues are FIFO per connection (a job blocked on a promise no
longer holds back other connections' jobs, the fast kick arms the same candidate); promise timeouts and promises
resolved with an already signalled fence kick the queue; closing a connection drops its queued jobs (fences aborted)
and waits up to 1 s for its running ones. `gpu calls` shows the K0j counters.

Patch 0123 (2026-11-04, CI run 41743977521 = 0121+0122+0123, image `out/xnu-ufs-rw-0123.img` with cpus=8 vivausb=3 vivagpu=2,
vivad in the xnu-kernel artifact; NOT tested on the phone yet): **WiFi step 1** (plan: ~/Developer/ios-viva/wifi/docs/PIANO-WIFI.md;
hardware: MT6781-WIFI-HW.md; iOS ABI: IOS16-WIFI-ABI.md). Link `wifi` / `wifi status`: CONNSYS MTCMOS, bus protection, EMI
remap, CONN reset, PMIC rails (read only; CONN registers only if powered and unprotected). `wifi detect`: Linux's A-die
detection with the 6631 rails (VCN18 only; its enable bits are now in the PMIC whitelist, no voltage writes): CONN on,
chip id (want 0x100B0000), HW ver (0x8A00), A-die SPI id (0x6631 or 0x6635), CONN off, pinmux and VCN18 restored.
Refused if CONN is already on. Worst case a bus hang (reboot by hand).

0123 on the phone (2026-11-04): `bat` OK (gauge 89 %, 358 cycles, health 88 %, charger PN 1, 8 interrupts, 46 writes, 0
errors; vbus_stat 5 "unknown adapter" -> 500 mA input with the Mac attached). `wifi`: CONN off after LK (PWR_CON 0x12,
protected, CONN reset bit set, VCN18/33/28 off, VCN33 vosel 3 = 3.5 V). `wifi detect`: conn version 0x100b0000, cfg id 2,
hw/fw 0x8a00, **A-die MT6631** (SPI 0xb024 -> 0x66318b01), MTCMOS back off.

Patches 0124-0126 (2026-11-04, CI run 41750485668, image `out/xnu-ufs-rw-0126.img`, NOT tested on the phone yet):
- **0124 WiFi step 2**: `wifi on` = full CONNSYS power-on for the MT6631: 5 MiB contiguous EMI window below 4 GiB
  (VivaWiFi.cpp; 16 MiB aligned, 64 KiB fallback) remapped at 0xF0000000, RAM patches read by the kernel from
  `/usr/share/firmware/wifi/viva/soc2_2_ram_{mcu,bt,wifi}_2_1_hdr.bin` (push them first) to +0x0/+0x170000/+0x2A0000
  with their dates at +0x88320/300/310, rails VCN18 + VCN33 at 3.3 V + VCN28 (co-clock efuse PALDO; PMIC whitelist
  extended to those enable bits and VCN33 vosel = 1 only), then every step of mt6781_pos_gen.c and the CPU reset release;
  success = MCU idle loop (0x18002600 == 0x1D1E). No WMT/STP and no EMI MPU SMC yet. `wifi off` undoes it. `wifi
  status` now also shows the MCU PC.
- **0125**: wall charger input limit: with no USB host configuring the link and BC1.2 saying unknown/non-standard/DCP,
  REG00 IINLIM = `vivachargein` (2000 mA default, 0 = off); a host brings it back to 500 mA.
- **0126**: K0j cost fix. 0122 walked the dependency graph (up to 128 fences, no memory, repeated per level) for every BO
  of every SUBMIT and every WAIT_BO while the connection had a pending promise (backboardd: nearly always), under the
  GPU gate, plus an OSArray per BO and a scan of every promise not yet pruned. Now: answers cached per fence until a
  promise is made/resolved/signalled or a job is dropped (epoch), the common "no promise involved" case allocates
  nothing, fPromises holds pending promises only. `gpu calls` shows "dependency walk N nodes + M cached".

0126 on the phone (2026-11-04): `wifi on` WORKS: EMI window PA 0xe7000000 (16 MiB aligned), the three MT6631 RAM patches
loaded, rails on (VCN33 now 3.3 V), **MCU idle loop after ~78 ms** (com_reg0 0x1d1e, PC moving: 0x2966 -> 0x21f66),
host csr hclk/osc on; `wifi off` clean. (viva-link.py segfaulted at exit after the reply: libusb torn down under the
daemon pump thread; fixed with os._exit.)

Patch 0127 (2026-11-04, CI run 41752917837, image `out/xnu-ufs-rw-0127.img`, NOT tested on the phone yet): **WiFi step 3**,
`wifi fw` (after `wifi on`): gen4m wlanDownloadFW without WMT. Driver own (LPCTL BIT1, wait BIT0 = 0), WFDMA rings
TX 0/1/3/15 and RX 0/1 (16 entries, contiguous memory below 4 GiB, VivaWiFi.cpp), GLO_CFG as asicPdmaConfig + the TX ring 3
DMASHDL bypass; `/usr/share/firmware/wifi/viva/WIFI_RAM_CODE_soc2_2_2_1.bin` (CONNAC tailer): regions 3/4 (DL_TO_EMI) copied
to EMI +0x2F0000/+0x386000, regions 0-2 (encrypted, 0x77000/0x02013400/0x40100000) as INIT_CMD DOWNLOAD_CONFIG (mode
0x8000000F) + 2 KiB PDA packets on TX ring 3 (32-byte TXD + 32-byte INIT header), result events on RX ring 1 (20-byte RXD),
then WIFI_START (override, 0x77000) and the ready bits 0x180C1140 & 0x6. If the firmware does not come up: WMT over
STP/BTIF (set chip id, RF calibration) is the next suspect.

Patch 0128 (2026-11-04, from the GPU side, NOT tested on the phone yet):
- **Panic fix** (photo of the panic: "Invalid kernel stack pointer (probable overflow)", the same lr all down the
  backtrace, frames 0x40 apart = `dependsOnPromiseOf`, whose frame is 0x40): 0122/0126 walked the K0j dependency graph
  recursively with a budget of 512 nodes = up to 512 nested calls (32 KiB) on a 16 KiB kernel stack, and with the
  asynchronous replay the chains of unfinished jobs passed ~250. Now depth-first with an explicit heap stack (512
  frames, allocated once), each node entered once per walk (stamp `k0jWalk`: cycles and shared dependencies no
  longer re-walked), answers cached as before ("no" only when not leaning on a node still on the path or a cut).
  collectWait skips nodes already reached in the same call. Host test: 180k random graph queries (cycles, diamonds,
  epoch changes) = plain reachability, a 100k-fence chain without crash. `gpu calls`: "... in N walks (M cut short)".
- **K0d double buffer off on some boots**: `disp` said "another OVL layer reads LK's next buffer": LK had left OVL0 L1
  enabled on 0x7c606000 (1080x2400, pitch 4320), so every copy went to the buffer on screen (tearing bands while
  scrolling). LK reserves 3 buffers (mblock-18-framebuffer 0x7bc10000 + 0x1df0000): flipSetup now takes the first of
  buffers 1-2 that no layer reads (0x7cffc000 that boot).

0127 on the phone (2026-11-04): `wifi fw` without WMT: the three DOWNLOAD_CONFIG got result 0 (the ROM's PDA path works),
every PDA chunk taken (TX ring 3 cidx = didx = 12), but no WIFI_START result (RX ring 1 didx 3, RX ring 0 empty) and the ready
bits went 0x2 -> 0x4 (not 0x6): the RAM code started and never finished its init.

Patches 0129-0130 (CI run 41757521920 = 0128 from the GPU session + 0129 + 0130, image `out/xnu-ufs-rw-0130.img`, NOT tested):
- 0129: `wifi fw` waits 3 s on both RX rings for WIFI_START and checks the ready bits anyway; `wifi` shows the WFDMA side.
- 0130 **WiFi step 2b**, `wifi wmt` (between `wifi on` and `wifi fw`): WMT over STP/BTIF as wmt_core_stp_init +
  mtk_wcn_soc_sw_init: BTIF @0x1100c000 in PIO (infracfg_ao CG0 bit 31, wake pulse), STP mand mode (4-byte header, 2 zero CRC
  bytes), register read 0x80000008, query STP (11 00 00 00) / set STP (DB 0E 68 01) -> full mode (0x80|seq<<3|ack, header sum,
  CRC-16 IBM, every data frame acknowledged with the received seq), query STP again, set chip id 0x6781, query A-die, RF
  calibration (01 14 01 00 01), VCN28 off.

0130 on the phone (2026-11-04): `wifi wmt`: BTIF clock on, the MCU ANSWERS over STP/BTIF (read 0x80000008 -> 02 08 0c 00 | 00 |
00 00 01 | 08 00 00 80 | value) but the check expected another layout and stopped there; `wifi fw` then as 0127 (ready 0x4).
Scanout test (runtime `disp scanout on`): 0 frames scanned out, every frame "surface not contiguous" (CA allocates its
framebuffers at start) and, with scanout on, copies skipped the duty limit: slower.

Patches 0131-0133 (CI runs 41759869436, 41761117709; image `out/xnu-ufs-rw-0133.img`, now built with vivascanout=1 by default;
NOT tested):
- 0131: WMT register read answer format; unexpected WMT answers are shown but no longer stop the sequence.
- 0132: a scanout fallback keeps the copy duty limit (fScanLive = the last frame was really scanned out).
- 0133: **SwapWait** (sel 6, [swap, options, timeout ms]) as Apple's swap_wait_gated: op 0 retired (not queued, not waiting
  for the frame end, not the surface the layer reads), 1 displayed, 2 idle; bit 31 non-blocking -> kIOReturnBusy; blocking
  re-checks every ms (5 s cap). CoreAnimation (IOMFBDisplay::can_update_) reuses a framebuffer page only when
  SwapWait(page swap, 0x80000000) returns 0 (gpu/docs/ANALISI-SCANOUT-RE.md): before, it rendered into the surface the
  panel was scanning out (tearing). Vsync notification args 4/5 = refresh period. **IOSurface use counts** (traps 0/1)
  kept per client and published at state page +0x18/+0x1c (IOSurfaceIsInUse; CABackingStore reuse), released when the
  client goes away. `disp` shows SwapWait busy/blocking counts and the swaps the layer reads / will read.
  `tools/viva-mkboot.sh` default VIVA_EXTRA now "cpus=8 vivausb=3 vivagpu=2 vivascanout=1".

0133 on the phone (2026-11-04): **WiFi firmware RUNNING.** `wifi wmt`: register read OK, STP mand -> full (DB 0E 68 01), set chip
id 0x6781 OK, A-die 0x6631 (02 02 05 00 00 01 8b 31 66), RF calibration answered with status 0 + 0x22a bytes of calibration
data; then `wifi fw`: WIFI_START OK, ready bits 0x6, MCU PC in the EMI RAM code (0xf031052a). WMT was the missing piece.

Patch 0134 (NOT tested): `wifi cap` = CMD_ID_GET_NIC_CAPABILITY_V2 (0x8A) on TX ring 15 (first normal command), event 0xEC
parsed (HW/SW version, efuse MAC, PHY caps, A-die version); calibration answer check relaxed.
On 0140 `wifi on` failed: "no contiguous 5 MiB below 4 GiB for the EMI window" (vivascanout=1 makes the IOSurfaces
physically contiguous below 4 GiB and fragments that memory). Patch 0141 (NOT tested): VivaPwrap::start calls
viva_wifi_reserve() and takes the EMI window (5 MiB) and the WFDMA memory (0x48000) at boot, log "VIVA WIFI: reserved ...";
boot-arg vivawifi=0 skips it. Also 0020-stage-bearssl.sh falls back to GitHub mirrors (bearssl.org answered 502), SHA pinned.
0141 on the phone: `wifi on/wmt/fw` OK again, `wifi cap` answered (17 TLVs, efuse MAC zero, A-die 0x6631 eco 1, HT/VHT/5 GHz);
EID 0x07 = SLEEPY_INFO (ignored). Spec for init/NVRAM/scan: ~/Developer/ios-viva/wifi/docs/MT6781-WIFI-CMDS.md.
Patch 0142 (NOT tested): `pull part:NAME` (read-only GPT partition of the UFS root LUN, e.g. nvdata), `wifi init` (basic
config, NVRAM from /usr/share/firmware/wifi/viva/WIFI = nvdata APCFG/APRDEB/WIFI, else stWifiCfgDefault_6631.bin + random
local MAC, domain x2, DBDC off, RX filter, BSS_ACTIVATE), `wifi scan [2g] [SSID]`; bsd/dev/viva_wpa.c (WPA2-PSK crypto +
4-way, host-tested on IEEE/RFC vectors, not used yet). Join/association not written yet.

Patch 0135 (2026-11-04, from the GPU side, NOT tested on the phone yet): **frame rate with scanout + SwapWait (0133)**.
Measured on 0133 (Settings scrolls): ~12 fps shown, GPU idle 84-91%, the same with the GPU pinned at 950 MHz;
K0c "vsyncs waited for the GPU" ~1.6 per frame shown, SwapWait ~110 busy answers/s. Since 0133 CoreAnimation draws
frame N+1 only after the page of N-1 retired (= frame N switched at a frame end), so every frame of latency between
SwapEnd and the switch costs frame rate directly. Two latencies removed:
- a promise resolved with a fence that had already signalled (replay's last job done before its TRANSFER) woke
  nobody: signalFence of a promise now calls sweet_fb_gpu_done (early present);
- with scanout live, the vsync tick looks for a finished frame before vblankTick, so a frame chosen at the tick is
  switched at this frame end, not the next one (copies still after the switch).
`disp` shows "latency (0135, us)": vsync -> SwapEnd (CA render), SwapEnd -> chosen (GPU done, histogram in frames),
chosen -> on screen, SwapEnd interval, frames chosen at the tick; `disp lat reset`.

0135 on the phone (2026-11-04, Settings scrolls, 6 swipes): ~11 fps. `disp` latency: vsync -> SwapEnd 8.5 ms, SwapEnd -> GPU
done 27.5 ms (18.8 ms with the GPU pinned at 950 MHz), chosen -> on screen 27-32 ms with ~2 ticks deferred ("not near
a frame end") per switch. Patch 0136 (NOT tested): the vsync timer is armed without leeway (kIOTimeOptionsWithLeeway, 0:
the default thread-call coalescing slop let ticks land after the frame end), a tick up to 3 ms early polls instead of
waiting a whole frame, `disp` counts late/early ticks with a switch pending ("ticks with a switch pending (0136)").
Patch 0137 (NOT tested): **DVFS display boost**. The governor aims at 70% busy over 50 ms windows; frames come in bursts
(~15-20 ms of GPU work every ~36 ms), so the average stayed low, the clock went down, each frame took longer to finish,
and with SwapWait CA drew fewer frames: a loop toward low fps. Now a vsync tick at which the display had a frame queued
that the GPU had not finished (K0c wait, counter `sweet_fb_gpu_waits`) sends the next DVFS sample to the fastest OPP
(Low Power Mode ceiling still applies). `vivagpuboost=0` = off; `gpu status` shows "N display boosts".

Patch 0138 (CI 41771551139, image `out/xnu-ufs-rw-0138.img`, NOT tested on the phone yet): **no IOKit daemon**.
lsd hung on every plugin registration (LS DB kept 1 extension, PosterBoard had no poster providers). Chain
(spindump + probes `vmg`/`vbusy` in viva-tests): lsd -> InstalledContentLibrary `__GenerateBuiltInAppInstallSessionID`
-> MGCopyAnswer("re6Zb+zwFKJNlkQTUeT+/w") -> `_MGWaitForDevices` ("IOKit is busy, waiting 5s") forever; from root
`MGCopyAnswer(WifiAddress)` and `UniqueDeviceID` also hang. IOKit busy state was 1: only `IOService:/`
(IOPlatformExpertDevice) busy, no busy child = the +1 that `StartIOKitMatching` adds until the IOKit daemon
(kernelmanagerd) checks in, which never happens on iOS. 0138: `OSKext::iokitDaemonAvailable()` returns false
(like `-restore`): no root busy hold, no deferred-match list. Boot-arg `sweetiokitd=1` restores the old wait.
Check after boot: `viva-tests/vbusy` must print `IOKit busy state 0`, `viva-tests/vmg WifiAddress` must not hang.
0138 on the phone: `vbusy` = IOKit busy state 0 (fix works), but `MGCopyAnswer(WifiAddress/UniqueDeviceID)` still
hung and lsd with it. Second wait: MobileGestalt's WifiAddress reads `IODeviceTree:/chosen mac-address-wifi0`
and without it waits for an IOService named sdio/wlan/marconi-wifi. mkadt (commit 8a985d5) now adds synthetic
`mac-address-wifi0` 02:41:00:00:00:01 and `mac-address-bluetooth0` ..:02. Image `out/xnu-ufs-rw-0138-mac.img`
(kernel 0138 + that ADT), NOT tested yet.

### 0139: vsync tick without timer coalescing

0136 passed `kIOTimeOptionsWithLeeway` with leeway 0, but XNU computes `slop = MAX(coalescing slop, leeway)`, so a
zero leeway changes nothing. On the phone (0138): every tick with a switch pending came avg 35 us *after* the frame
end (6492 deferred for 2682 switches, chosen -> on screen avg 38 ms, SwapEnd every 34 ms ≈ 29 fps). Only urgency
`TIMEOUT_URGENCY_SYS_CRITICAL` has no slop, and IOTimerEventSource cannot pass it: new option
`kIOTimeOptionsSysCritical` (0x200) maps to `THREAD_CALL_DELAY_SYS_CRITICAL`; the vsync rearm uses it.
0138-mac on the phone: IOKit quiet, MobileGestalt answers (WifiAddress 02:41:00:00:00:01, UDID 79ac8bfd...), lsd no
longer hangs and a full LS rebuild completes (30-90 s), but still 1 extension in the DB. lsd's os_log (new probe
`viva-tests/vlog SECS [PROC]`, live os_activity stream) shows why: ExtensionFoundation
`_EX_dictionaryWithSignedURL:` refuses the extension point cache ("Unable to find AMFI": it checks the cache's cdhash via
IOServiceMatching("AppleMobileFileIntegrity") + user client selector 6), so installd rejects 83 appexes "specifies a value
(com.apple.widgetkit-extension) for NSExtensionPointIdentifier ... that does not correspond to a known extension point".
Patch 0140 (CI 41776680999, includes the other agent's 0139 vsync; image `out/xnu-ufs-rw-0140.img` with the MAC ADT,
NOT tested): built-in AppleMobileFileIntegrity IOService on IOResources, sel 6 -> success, other selectors unsupported
(logged once as `SWEET AMFI:`). After boot: full rebuild (`viva-tests/vlsr`), then `vwall lsext` / `vwall ext`.
0140 on the phone (+ MAC ADT): full LS rebuild 143 s, LS DB 413 extension records (was 1), 9 com.apple.posterkit.provider
appexes, PosterBoard `fetchExtensionIdentifiers` lists all 9 (Collections, Gradient, Emoji, Pride, Unity, Photos,
Extragalactic, Aegir, LegacyPoster). Next (userland): create/select a poster configuration.

### 0143-0144: WiFi for iOS (wifid / Settings), join WPA2-PSK

0142 on the phone: `wifi init` + `wifi scan` work (4 networks, RSSI/channel/security right).
Patch 0143: AppleKeyStore `aks_wrap_key` / `aks_unwrap_key` (sel 10/11), ported from ios-sapphire 0160 + 0293 (class =
second scalar): without them securityd cannot write keychain items and wifid never keeps a WiFi password.
Patch 0144 (NOT tested), the iOS pipeline as hakios-xnu-kernel / ios-sapphire do it on the same 20H392 userland
(~/Developer/ios-viva/wifi/docs/IOS16-WIFI-HAKIOS.md), chip side from gen4m (wifi/docs/MT6781-WIFI-JOIN.md):
- ADT `/arm-io/wlan` (device_type wlan, compatible mediatek,mt6781-wlan, local-mac-address = /chosen mac-address-wifi0):
  launchd now starts wifid (`_LimitLoadToDeviceTree`), MobileGestalt reads the MAC there.
- iokit/Kernel/arm/VivaWLAN.cpp: properties-only classes with Apple's names (IONetworkController <- IOEthernetController
  <- IO80211Controller <- VivaWLAN on the wlan nub; IONetworkInterface <- VivaWLANInterface "en0" with
  IO80211InterfaceRole Infrastructure; newUserClient refused -> IO80211.framework uses the ioctls), IONetworkStack
  stand-in on IOResources. Boot-arg `vivawlan=0` = none of it (en0 not created).
- bsd/dev/viva_wlan.c: en0 (IFT_ETHER, SUBFAMILY_WIFI, legacy + NX_NOAUTO, BPF, KEV_DL_LINK_ON/OFF, SIOCGIFMEDIA
  IFM_IEEE80211), SIOCGA80211/SIOCSA80211 (hakios answers + RSSI_BOUNDS {1,-95,-20,-85} and NOISE -95 for auto-join,
  CHANNELS_INFO), events without payload (POWER_CHANGED, SCAN_DONE, ASSOC_DONE; join: LINK_ON, ASSOC_DONE, SSID, BSSID,
  LINK_CHANGED). Unknown requests logged once: `VIVA WLAN: GET|SET type N len L not supported`.
- bsd/dev/viva_wifi.c: a worker thread + locks (vw_mtx hardware, vs_mtx snapshots), radio up on demand (on, wmt, fw,
  init; GET POWER says on from boot like hakios, the first scan/join brings it up, ~2 s), async scan, host MLME join:
  CH_PRIVILEGE (0x1C, grant 0x10) -> STA record WTBL 5 state 0 -> Auth on TX ring 15 (TXD + frame) -> Assoc (SSID, AP
  rates, RSN PSK/CCMP with the AP's group cipher, HT cap if HT, WMM info if WMM) -> WMM params, BSS_INFO connected (auth 7
  enc 6, BMC 4), STA record state 2 (event 0x0C), PM connected + CAM -> 4-way (viva_wpa.c, PMK from the passphrase in
  the kernel) -> PTK (WTBL 5, event 0x24), GTK (WTBL 4) -> channel released -> link up. Data: TX ring 0, TXD + append
  pointing at the frame in the same buffer, token = slot, freed by MSDU reports (RX type 6; a slot whose report never
  came is reused after 1 s, counted as "lost"). Wrong password -> ASSOCIATE_RESULT 21. RX block ack refused in the STA
  record (no reorder buffer).
- Link: `wifi up`, `wifi join SSID [PASSPHRASE]` (SSID without spaces), `wifi leave`, `wifi sta` (counters + what
  wifid asked), `wifi off`.

### 0180: boot-args of the next boot in userdata (VIVABOOT), read by the shim
Found on the phone (2026-11-06): writing boot_b from iOS fails, `SWEET UFS: command 0x2a failed: ocs 0x0 upiu 0x21
response 0x1 status 0x2` (CHECK CONDITION) on every Viva Manager save, while root writes on the same LUN 2 work: LK
write-protects the boot partitions in a normal boot (fastboot can still flash them).
- Kernel (0180, bsd/dev/sweet_ufs.c): a CHECK CONDITION prints the sense data (`sense key 0x7 ... DATA PROTECT`) and
  returns EROFS. `sweet_ufs_bootfile()` reads/writes the VIVABOOT record: the first 4 KiB block of the last MiB of the
  root partition (`userdata`), written only if the HFS+ volume header says the volume ends before it (16 GiB image vs
  109735 MiB partition), through `ufs_write_blocks()` (ufsrw=1, root partition bounds), read back and compared.
- `kern.viva_nextbootargs` (viva_sysctl.c): write tries the payload in boot_b first; on EROFS (remembered for the boot)
  or EIO it writes the record instead; the payload's own line disables the record. Read = the line the next boot uses.
  `kern.viva_nextbootargs_src` = `vivaboot|payload boot-partition-write-protected|writable|untested`.
- Shim (shim/vivaboot.c, .h = layout and validation shared with the kernel): before copying XNU it takes the UFS
  request list from LK (as sweet_ufs.c, READ(10) only, 1 s timeouts, LK's list given back), finds `ufsroot=`
  (default userdata) on `ufslun=` (default 2) in the GPT, reads the record and uses its line if it is enabled, intact
  (FNV-1a 64) and saved against this payload's line (base_hash). Anything else = the payload's line, as before; the
  screen says `BOOTFILE ...`. Escape hatches: flashing boot_b with another command line drops the record (other
  base); `vivabootfile=0` in the payload ignores it. Host test: `cc shim/vivaboot_test.c -o shim/out/vt && shim/out/vt`.
- Edit the block from the Mac: `tools/viva-link.py bootrec` opens it in `$EDITOR` (default vim, `EDITOR=nano`) as
  key=value fields + `cmdline=`; saved changes are written (line_hash recomputed, seq + 1, `base_hash=payload` = hash of
  the boot partition's line). `bootrec hex` = the raw 4096 bytes as a hex dump, written exactly as saved; `bootrec show`
  prints it. Phone side: `vbootrec` (tools/vivad/vbootrec.c, built into the xnu-kernel artifact next to vivad; push it
  to /private/var/root/viva-tests/vbootrec 755) through sysctl `kern.viva_bootrec` (the raw 4 KiB block: read by
  everybody, written by root with the HFS+ overlap check, read back). On a kernel without 0180 it only reports
  ENOENT.
- Panic found on the way (2026-11-06, kernel 0170): the first read of the raw device /dev/rufsroot0 from userspace
  (vbootrec's first version) -> "Kernel data abort", far = the user buffer: physio() + buf_map() gave sweet_ufs's
  strategy the user address and its memcpy faulted under PAN. 0180: the raw device goes through a kernel buffer and
  uiomove() (the UFS lock is not held across the copy to/from the user), never physio.
- An empty line through kern.viva_nextbootargs disables the record and is never written into the boot partition.

### 0146: Viva Manager sysctls (KA1 kern.viva_link, KA2 kern.viva_nextbootargs)
Spec and behaviour: gpu/docs/RICHIESTE-KERNEL.md (KA1, KA2, "Stato KA1/KA2"). New bsd/dev/viva_sysctl.c; `gpu kv` /
`disp kv` in VivaGPU.cpp / SweetFramebuffer.cpp (also on the USB link); `stat` moved to viva_link_stat() in
viva_usb.c; sweet_ufs.c sweet_ufs_bootargs(): the only UFS write outside the root partition, limited to the one 4 KiB
block of boot_a/boot_b holding the SWEETXNU cmdline (checked header/offset/magic/version, read back). NOT tested.
Check on the phone: `sysctl kern.viva_link` style calls from the app, `sysctl kern.viva_nextbootargs` (read), then a
root write and `fastboot`-free reboot to see the new kern.bootargs. If boot_b is not on the root's LUN: ENOENT.

### 0145 / 0147: CPU DVFS, A76 preference, LVTS thermal (CI 41849632117, image `out/xnu-ufs-rw-0147.img` = up to 0147, NOT tested on the phone yet)
Before: nobody set CPU clocks (LK's A55 1866 MHz, **A76 1085 MHz**, CCI 612 MHz), A76s slower than the A55s
(cpubench2: ALU chunk 1270 us on cpu 6-7 vs 1080 us on cpu 0-5) and XNU picked the lowest idle cpu (UI on the A55s).
- 0145 `osfmk/arm64/viva_cpufreq.c`: AP-side DVFS as Linux cpufreq_v1 mt6781 (no MCUPM): Vproc11 (A76) / Vproc12
  (A55 + CCI) via VivaPwrap whitelist (ELR0 0x13a6/0x1426, max 1.11875/1.03125 V), Vsram tracks in hardware (checked),
  ARMPLL_LL/BL/CCIPLL CON1 PCW + posdiv (FHCTL off, checked), OPP tables = this phone's live DTB dvfsp tables.
  Governor every 16 ms (100 ms idle): busiest CPU >= 80 % -> ceiling, else <= 70 % target, down after 3 samples;
  CCI follows the faster cluster. Low Power Mode caps (big 1530, LL 1500, CCI 1050; `vivalpmcpu`). Scheduler
  (sched_prim.c choose_processor): with the A76s fast, threads >= user-initiated priority take an idle A76 first,
  lower ones an idle A55 first (`sched_viva_fast_cpus`). Boot-args `vivacpufreq=0|1|2` (off / governor / manual),
  `vivacpubig|vivacpull|vivacpucci=MHz`, `vivacpufast=0`. Link: `cpu [status] | gov on|off | set|max ll|big|cci MHz |
  fast on|off | reset`.
- 0147: LVTS sensors (9 points, devinfo calibration of this phone) read every 256 ms; hottest >= `vivathermtrip`
  (85 C) one OPP down per sample, < trip-5 one up per second, >= 105 C floor; no valid reading = cap opp 6 (LL 1618,
  big 1670); `vivatherm=0` = no LVTS. TOPRGU hardware thermal reset NOT armed. Temperatures in `cpu`.
- If the phone hangs at boot with 0147: boot with `VIVA_EXTRA="... vivacpufreq=0"` (clocks untouched) or
  `vivatherm=0`.

### 0149: WiFi bring-up stack overflow (0148 panic)
0148 on the phone: `wifi up` (and WiFi from Settings) panicked "Kernel stack memory corruption detected" in
`wifi_init` (backtrace vl_thread -> viva_wifi_link -> wifi_up -> wifi_init -> __stack_chk_fail, symbolized with
xnu/out-0148). Cause (since 0142, latent until the frame layout changed): `uint8_t p[64]` but SET_RX_FILTER does
`bzero(p, 68)` and sends 68 bytes. Now p[80].
RAM: the live DT says 6 GiB (`reg <0x40000000 0x1_80000000>`, rank0 2 GiB at 0x40000000, rank1 4 GiB at 0xc0000000);
XNU gets only 0xc0000000-0x1be000000 (~3.97 GiB) because rank0 holds LK's no-map reservations (TEE, ATF, modem SMEM,
SCP/SSPM, framebuffer: ~472 MiB in 24 regions); ~1.5 GiB of rank0 is free but in holes. Using it needs hole support
in arm_vm_init/pmap (not done).

### 0150: received frames as one cluster mbuf
0149 on the phone: joining from Settings panicked after the password, "after some loading" (no photo of the text yet;
the link was gone, panic screen). Suspect: data_rx built the mbuf with mbuf_allocpacket (m_len 0) + mbuf_copyback, and
viva_wlan_input strips 14 bytes with mbuf_len(m) - 14: if copyback left m_len short, the first frame after the link
came up (DHCP) went to IP with a huge length. Now gethdr + 2 KiB cluster + memcpy + explicit lengths (as sweet_usb.c),
and viva_wlan_input drops a frame whose first mbuf is shorter than the Ethernet header. NOT tested.

### 0151: calendar from the PMIC RTC
0150 on the phone: WiFi joins from Settings and gets an address (Settings no longer panics), but the clock is 1970,
so TLS fails everywhere. Nothing published IORTC, so IOKitInitializeTime waited 30 s and the calendar started at 0.
The MT6366 RTC (base 0x588, rtc-mt6397 layout, year = 1968 + reg) holds UTC kept by Android/LK: read on the link
2026-11-04 21:00:07 = the Mac's UTC. VivaPwrap::start reads it (`VIVA RTC: N s since 1970`), publishes IORTC;
SweetPlatformExpert::getUTCTimeOfDay returns it. setUTCTimeOfDay is a no-op (no RTC writes; NTP/timed only moves
the kernel calendar). Settings > General > Date & Time crashing is a separate (userland) issue, not looked at yet.
0151 on the phone: `VIVA RTC: 1793827603 s since 1970` (right UTC; awdd session ids agree). The "old clock" was the time
zone: /private/var/db/timezone/localtime -> zoneinfo/US/Pacific (the iPhone image's default), 9 h behind Italy.
New tools/vivad/vln.c (built by CI next to vivad; iOS has no ln), pushed to /private/var/root/viva-tests/vln:
`viva-link.py run -- /private/var/root/viva-tests/vln /var/db/timezone/zoneinfo/Europe/Rome /private/var/db/timezone/localtime`
(set 2026-11-04, survives reboot). Settings > Date & Time hangs (no CRASH line in the console): not looked at yet.

### 0152: data TX ring 0 without MSDU reports
0151 on the phone (joined TIM-43248969 from Settings): pages stop at the first progress tick, Sileo "533 Time Out";
`wifi sta`: data tx 1634 (76 drops), rx 1279 = traffic flows but crawls. Suspect: a TX ring 0 slot was only reused
on an MSDU report (token) or after 1 s; with 16 slots and reports missing (or unparsed) that caps TX near 16 frames/s.
Now: ring 0 has 32 entries (descriptors at DMA +0x41000, 2 KiB buffers in ring 3's area), a slot without a report is
reused after 100 ms, the ring-full case (DIDX not past the previous entry) is checked, and `wifi sta` prints the
tx ring 0 counters (reports, reused, waits, ring full, cidx/didx, the first report's raw bytes). NOT tested.

### 0153: WFDMA descriptors through an uncached mapping
Measured on 0151 with traffic stalled (`peek`): TX ring 0 CIDX 0xe / DIDX 5, then CIDX 0 (the driver wrapped over
entries the DMA had not taken: fixed by 0152's ring-full check); RX ring 0 CIDX 2 / DIDX 0xe = 11 frames written by
the hardware and never read, `rx` frozen. Cause: descriptors are 16 bytes, four per cache line, in cached memory;
giving RX entry i back (write + clean of its line) wrote the CPU's stale copy of entries i+1..i+3 over the DMA's
"done + length", so the driver waited forever on an entry that looked empty. Now viva_wifi_dma_uncached()
(VivaWiFi.cpp: the same memory mapped write-combined) is the only way descriptors are touched; wr() issues a DSB
before every register write (CIDX after the descriptor writes). NOT tested.

### 0154: no TX A-MPDU, RX ring resync, `wifi ring`
0153 on the phone: same stall. `wifi sta`: 127 MSDU reports came back (tokens work), then all 32 ring-0 tokens stayed
out and the DMA stopped (CIDX 17 / DIDX 18, 479 "ring full"): the MAC stopped sending data, so the DMA scheduler ran
out of room. RX ring 0: entries 7..15, 0 done by the hardware, entry 6 empty, driver waiting on 6 (CIDX 5).
Guess for TX: the firmware's TX block-ack session (UPDATE_STA_RECORD ucTxAmpdu = 1, as gen4m) jams under load; now 0.
RX: when the entry the driver waits on reads empty but the DMA is past it and the next one is done, give it back and
move on (counted). `wifi ring`: both RX rings (hardware/driver indexes, every descriptor's d1), TX 0/15, GLO_CFG,
INT_STA. NOT tested.

### 0182: standard USB persona (iPhone-like PTP + usbmux) and Viva Link heartbeat
User request 2026-11-06: the link must count only when the host tool is really there; a computer without the tool
must see a normal phone; charging at full input unless the tool is active; the island announces the link.
Contract (kernel / vivausbd / viva-link.py / SpringBoard tweak): `docs/VIVA-USB-PERSONA.md`.
- Personas: `link` (1209:0002, as before) from boot; `std` (05ac:12a8 "iPhone", serial = UDID) once vivausbd opens
  `/dev/vivaptp` + `/dev/vivamux` and issues VIVAUSB_ATTACH: soft disconnect 300 ms, new enumeration with PTP (if0,
  ep2 bulk + ep3 interrupt), usbmux (if1, ep4), link (if2, ep1). FIFO RAM: ep0..ep4 use 3200 bytes (size printed at
  start: `VIVA LINK: FIFO RAM N bytes`). `vivausbstd=0` = never switch.
- ep0 now has OUT data stages (PTP CANCEL_REQUEST), PTP class requests (GET_DEVICE_STATUS OK/busy, DEVICE_RESET),
  Microsoft OS descriptor (string 0xEE, vendor 0x50 compat ID "MTP" for interface 0), Apple vendor OUT requests acked.
- Functions: per direction a 128 KiB ring + transfer-end marks; OUT packets that do not fit stay in the FIFO (NAK).
  /dev/vivaptp writes: the kernel ends the IN transfer only at the end of the PTP container (length from its header),
  so a data phase may span several ≤ 64 KiB writes. Bus reset / SET_CONFIGURATION / disconnect: read() returns
  ECONNRESET once; PTP cancel: ECANCELED.
- Viva Link active = vendor request 0x41/0x56 wValue 1 (tool heartbeat, 1 s; 4 s timeout; wValue 0 = bye).
  viva_usb_host_configured() (VivaBattery input limit) is now "tool active": a computer without the tool gets
  `vivachargein` (2000 mA) like a wall charger; the battery polls at once when the state changes.
  `sysctl kern.viva_linkstate`: 0 no host, 1 standard, 2 Viva Link. `stat` shows persona, heartbeats, function counters.
- Risk: a computer port advertising less than 2 A (USB-C 1.5 A, USB-A 500/900 mA) may cut power instead of sagging
  into VINDPM: then boot with `vivachargein=500` or a lower value.
NOT tested on the phone. Compile-checked locally (xnu-cc.py, bt0154 + 0180's viva_sysctl.c).

### 0183: CPU UI boost (P1 of gpu/docs/ANALISI-PRESTAZIONI-2026-11-06.md)
Measured 2026-11-06 on 0180: during a Settings scroll LL/big never left 774/835 MHz (a UI thread 25 % busy never
reached the governor's thresholds); 42.9 ms per frame, 32.7 with the clocks fixed at max.
- `viva_cpufreq_ui_activity()` from every SwapEnd (SweetFramebuffer.cpp) and every touch report (VivaGT9896.cpp):
  for `vivacpuboostms` (250; 0 = off) LL and big run at least at `vivacpuboost=MHz` (default: fastest OPP), under the
  thermal (0147), Low Power Mode and link ceilings. A signal while the governor polls every 100 ms samples at once.
- `need` targets 50 % busy of the busiest core (was 70 %).
- A76 preference: big at least LL MHz x 1.4 / 1.46; if the big ceiling does not allow it, the preference is suspended.
- Link `cpu boost on|off`; `cpu` shows boosts, ms boosted, signals, suspensions; `gpu kv` cpu_boost* keys.

### 0184: display page pool, SwapWait reasons, 64-byte client pitches (P2, P3, P6)
- P2: with `vivascanout=1`, AppleCLCD::start takes `vivafbpool` (4; 0 = none; max 8) physically contiguous buffers
  below 4 GiB for 1125x2436 BGRA (11.2 MB each) before memory fragments. Display-sized surfaces take a pool buffer
  first, then a fresh contiguous allocation, then sparse memory; freed surfaces give the buffer back. On 0180 60-70 %
  of CA's frames were CPU-copied ("surface not contiguous"). `disp`: pool / contiguous / sparse counts.
- P3: SwapWait answers that were not done, per reason (queued for the GPU or copy, switch pending, on screen),
  per 100 swaps, and first-busy -> done latency per reason; `disp lat reset` clears them. `disp kv`: swapwait_busy_*.
- P6: a client IOSurfaceBytesPerRow that is not a multiple of 64 is rounded up for single-plane surfaces (no offset,
  no parent, and only if an explicit IOSurfaceAllocSize still holds the rows); the first 16 cases are logged
  (`SWEET SURF: WxH fmt pitch A -> B`). Boot-arg `vivasurfpitch=0` = as the client asked. Fixes the GPU copying
  the 1125x2436 pitch-4512 snapshot in and out (23 ms stalls).
NOT tested on the phone. Compile-checked locally (xnu-cc.py, bt0154 tree with 0182).

0182 on the phone (2026-11-06 night): `FIFO RAM 8192 bytes`; heartbeat seen (`Viva Link active`), without it `inactive`
after 4 s. vivausbd (launchd com.viva.vivausbd, /usr/libexec/vivausbd) switches to 05ac:12a8: the Mac asked "allow
accessory" (until allowed the device stays `!registered`), then PTP / Apple USB Multiplexor / viva iOS link interfaces
matched, macOS usbmuxd lists the device with the UDID, usbmux Connect to 62078 succeeds; viva-link.py reattaches on if2.
Open: **lockdownd never answers** (also locally on 127.0.0.1:62078 and /var/run/lockdown.sock; no crash): Finder/Image
Capture need it. VivaIsland tweak works (system island, "Viva Link · USB"). Charger: a Mac port reads vbus_stat 2 (CDP),
so 0182 left 500 mA -> **0186**: full input on any detected port while the tool is not active.

### 0187: A76 rule only while boosted; vivasurfpitch=2
0186 on the phone (2026-11-07): Settings scroll 19.9 ms per frame with the UI boost (42.9 on 0180), but outside a boost
0183's A76 rule held big at ~1920 MHz whenever LL ran fast (84 C). Now only during a boost. P6 left CA's 1125x2436
snapshot (pitch 4512) and 787x1705 surfaces unaligned (explicit IOSurfaceAllocSize = pitch x height): boot-arg
`vivasurfpitch=2` rounds those too and grows the allocation (to try: correct only if the writers read the pitch back).

### Hang: `vctl open` / swipes never return, vivad "not reading" (2026-11-07, analysis)
Reproduced with `sweetsample=SpringBoard,backboardd,vivad,vctl` and a script of one-shot viva-link commands with
pauses (each command = heartbeat + bye = link state 2 -> 1 -> 2, and the 0182/0186 charger input limit flips 500 <->
2000 mA). Without the VivaIsland tweak: 0 hangs in 104 commands. With it: SpringBoard's main thread never idles again
(UIKit in-process animations, its own `_checkForHang`, `SBSystemApertureProvidedContentElement` descriptions,
`SBFluidSwitcher...` layout): an endless System Aperture layout/animation loop when the tweak's element (alert ->
activity, `-[VIElementPresenter noticeEnded:]`) changes during an app transition; every client of SpringBoard (vctl)
waits forever. Wired memory meanwhile climbs from ~53k to ~237k pages (16 KiB) with repeated failed 11 MiB contiguous
allocations; vivad then sits at the same user pc in every sample (a page fault that never completes), so the kernel
sees no reader and refuses commands. Not a loop in vivad's code.
Fixes: viva-link.py one-shot modes no longer send the bye (a script is one link session; the phone drops "active"
4 s after the last heartbeat); VivaIsland debounces 2 s up / 15 s down; vivad no longer spins on EINTR and reports odd
input. Still hangs in the worst case (a fresh notice during an app transition, 20 s gaps): the tweak's system-element
path is OFF on the phone (`vi-sb.sh off`) until fixed; the overlay path did not hang but drew nothing.
Open: why wired memory grows that much (IOSurfaces are wired kernel memory, cap 1.5 GiB; GPU 142 MiB at the time).

### 0189: panel refresh 60 / 120 Hz / auto
The panel (Xiaomi viva-r-oss `drivers/gpu/drm/panel/dsi_k6t_38_0c_0a_dsc_vdo.c`) has a 120 Hz mode with the same
pixel clock (369.469 MHz) and MIPI rate: VFP 2452 -> 20 lines, plus four DCS packets sent in the video-mode blanking
through DSI VM_CMD (vendor LCM_SEND_IN_VDO / DYNFPS_DSI_VFP): F0 5A 5A, 60 01 (120) / 60 21 (60), F7 0B, F0 A5 A5,
then two frames, then DSI_VFP_NL (0x14013028). Tried by hand with poke on 0187 (2026-11-07): frame end calibrated at
8348 us, picture fine. AppleCLCD now: `panelSetHz()` (fScanLock, ~3 frames), vsync tick follows the panel (8333 us
target at 120 Hz) and the period sent to CoreAnimation is per tick (was per panel frame), frame end calibrated again.
Boot-arg `vivapanelhz=60|120|1` (1 = auto: 120 Hz from a swap, 60 Hz after 500 ms idle or display off), default 60;
link `disp hz [60|120|auto]`; `disp kv` panel_hz, hz_mode, hz_switches, hz_failures. Viva Manager: SpringBoard ->
Display. With 120 or auto at boot the service publishes IOMFBDisplayRefresh {displayMinRefreshInterval = 8.333 ms in
0.32 fixed point, displayRefreshStepMachTime}: QuartzCore's AppleCLCDDisplay::heartbeat_rate() takes its vsync interval
from it (iOS 16.7, read once when backboardd starts; CA_HEARTBEAT_RATE overrides).
On the phone (2026-11-07, vivapanelhz=1): 46 switches in auto mode, 0 failures, frame end 8348 / 16696 us; user
scrolling Settings by hand: SwapEnd every 14.7 ms on average (4086 swaps), chosen -> on screen 4.9 ms (8.7 at 60 Hz).
Update: the user saw a dark flash at idle switches. The switch is now locked to frame ends like the vendor's GCE
sequence (commands after a start of frame, two frame ends, the porch written after the next start of frame, in the
active lines) instead of arbitrary times; `vivapanelidle=ms` / `disp hz idle MS` sets the auto drop-back delay (500).
Update 2: still a flash. By hand over the link (each poke ~0.5 s) the panel stayed black for seconds between the mode
command and the porch write, both ways: panel mode and DSI timing out of step = black. Going to 60 the first two
commands now go in VSA/VBP as the vendor does; `disp hz seq 0|1|2` (vivapanelseq) picks where the porch goes: 0 vendor
(two frames after the commands), 1 in the blanking that carried F7 0B, 2 at the start of the frame before the commands.

### 0191: brightness and Night Shift
CoreBrightness (iOS 16.7, SetIOMFBBrightness) sets IOMFBBrightnessLevel on the IOMFB service: CFNumber SInt32, nits in
16.16 fixed point. AppleCLCD::setProperties converts it to the panel's DCS 51 HH LL (0..2047 normal range, Xiaomi
lcm_setbacklight_cmdq), level 2047 = `vivapanelnits` (500), sent through DSI VM_CMD at the next vsync tick. Tried by
hand: 51 03 00 brighter than LK's 51 02 19. QuartzCore's color matrices come through selector 55
(IOMobileFramebufferSetMatrix: scalar = matrix type, 72 bytes = 3x3 doubles row-major): converted to fixed point
without the FPU, multiplied in type order, written to the display pipe's CCORR (0x1400b000; coefficients 1.0 = 0x800
on MT6781, two 13-bit per register; CFG bit 0 relay / bit 1 engine; identity = relay). Tried by hand: a warm matrix
turned the picture yellow. First 16 matrices logged (`SWEET FB: matrix T: ...`). Link `disp bl [N]`; `disp kv`
brightness=, brightness_max=, matrix_calls=. NOT tested as a kernel.
Update: on the phone neither control reached the kernel (trace: no selector, no property). CoreBrightness only builds its
display module for a service matching IOPropertyMatch {backlight-control: true} that conforms to AppleARMBacklight
(CBDisplayContaineriOS findBacklight); DisplayCreateWithID reads IODisplayParameters.brightness {min, max, value}.
AppleCLCD now attaches such a nub (AppleARMBacklight, max 2047); its setProperties takes brightness / IODisplayParameters
to the panel. `disp seltrace on` logs every user client call, swap record change and property set.
Update 2 (phone): brightness works through the nub (1476 levels sent) but the slider stuttered: each DCS write waited up
to a frame for its blanking inside the vsync tick, holding the frames; now non-blocking (checked at the next write).
Night Shift arrives as selector 55 type 0 during the ramp, but the 72 bytes are 9 x int64 signed 32.32 (0xD5FE7E00 =
0.836), not doubles; converted with >> 21. CCORR mapped through dispMap.

### 0192: Shut Down / restart, panel sleep, CPU heat, WiFi RX block ack, force-restart sequence (NOT tested on the phone)
User report 2026-11-13: VolUp, VolDown, Power shows no power-off slider; Settings > General > Shut Down panics; the
phone runs hot (WiFi frame by frame, CPU clocks, panel never really off in standby).
- **Shut Down / restart**: nothing set PE_halt_restart, so halt_all_cpus spun and PEHaltRestart's 30 s timer
  panicked ("Halt/Restart Timed Out"). SweetPlatformExpert::haltRestart now (viva, not during a panic) puts the panel
  to sleep (sweet_fb_panel_off_for_halt) and calls viva_halt_restart (VivaPwrap.cpp): restart = TOPRGU SWRST (as the
  link's reboot, WDT_MODE IRQ/DUAL cleared); power off = vendor mt_power_off (viva-r-oss drivers/mfd/mt6358-misc.c):
  RTC_BBPU 0x588 <- 0x4303, AL_MASK 0x590 <- 0x10, WRTGR 0x5c2, CBUSY/CLR polls, BBPU | RELOAD, TOP_TMA_KEY 0x3a8
  0x9CA7, PSEQ_ELR11 0xa62 bit 10 = 0, PPCCTL0 0xa08 bit 0 (PWRHOLD) = 0; still alive after 1 s -> TOPRGU reset.
  Separate whitelist (only while halting); the pwrap lock is taken and kept. `vivapoweroff=0` = old behaviour.
- **Force-restart sequence** (VivaPMICKeys/VivaKeypad): SpringBoard maps keyboard event 0xFF01/0x40 (AppleVendorKeyboard
  WillReset, sent by Apple's button firmware) to -[SBLockHardwareButton forceResetSequenceDidBegin] -> power-off slider
  0.35 s later if the side button is still down. The kernel now recognizes VolUp click, VolDown click, side down (1 s
  windows) and sends it; side still held `vivaforcerestart` ms (10000, 0 = off) later -> watchdog restart.
  Side + volume held ("claw") goes to sosd (Emergency SOS remote alert) on D22, not to SpringBoard's own slider; if
  sosd fails, com.apple.SOS SOSDeviceSupportedKey = false (mobile) re-enables SpringBoard's LockVolumeButtons-Shutdown.
- **Panel sleep** (SweetFramebuffer.cpp): before, display off only faded to black with the panel awake at 60/120 Hz.
  Now (vivapaneloff=1) 51 00 00, 28, 10 ms, 10, 120 ms (vendor lcm_unprepare, sent in video mode via VM_CMD), DSI VFP
  -> vivapaneloffvfp (32767, ~8 fps), slow software vsync tick vivapaneltick (200 ms) that retires queued swaps;
  wake = VFP back, 11, 120 ms, 51 level, 29, vblankCalibrate. Link `disp panel [off|on|tick MS|vfp N|mode 0|1]`,
  `disp kv` panel_*. viva_cpufreq_display_power() is told about display on/off.
- **CPU** (viva_cpufreq.c): boost split touch (full, 250 ms, ignores a still finger after 1 s) / frames (1670 MHz, only
  near a touch or a first frame after 2 s); A76 preference only while boosted (was always on); CCI follows LL; floor =
  lowest OPP (LL 500 / big 774); up-ramp needs 2 samples, target 65 %; timer leeway; display off = no boost + LPM caps;
  battery "skin" ceiling (vivaskintrip 42 C); die trip 75 C (was 85); time-in-state + per-CPU busy in `cpu`; `cpu tune`.
  Old 0183 behaviour: vivacpuboostwin=0 vivacpuboostswap=0 vivacpufast=1 vivacpufloor=14 vivacputarget=500 vivacpuup=1
  vivacpuccilag=15 vivathermtrip=85 vivaskintrip=0.
- **WiFi** (viva_wifi.c): STA record had ucRxAmpdu = 0 -> AP sent single MPDUs. Now rxba=1 with host reorder (gen4m
  qmProcessPktWithReordering: per-TID 64-slot window, A-MSDU, 100 ms hole timeout, ADDBA/DELBA/bubble events), RX ring
  0 = 128 entries (was 16; DMA area 0x148000), WFDMA interrupt SPI 0x11d (INTID 317) instead of 2 ms polling with
  delay(50) looks (~1500 wakeups/s), one copy per frame, batch ifnet_input. Toggles `vivawifi<name>` / `wifi opt`:
  rxba txba(0) reorder reorderms irq pollus idlems batch rxn. If trouble: vivawifiirq=0, vivawifirxn=16, vivawifirxba=0.

### 0193: WiFi RX descriptors, panel brightness 10 bit, screen console on wake (NOT tested on the phone)
Phone report on 0192 (2026-11-13): force-restart slider OK, panel really sleeps; but no WiFi network found, the
brightness slider reached full at half way and went dark just after (two cycles), kernel lines on screen at wake.
- **WiFi**: "wifi fw" failed at region 1 (`cmd 0x01 seq 4: no event in 500 ms (rx1 didx 2 ...)`, `radio FAILED to come
  up`): the firmware had answered, the driver never saw it. 0192 dropped the clean + invalidate of the descriptor's
  line of the cached mapping before reading it through the uncached one; with the two aliases the stale cached line
  still answered. rx_peek / rx_pop / rx_resync flush the descriptor again (as up to 0191).
- **Brightness**: PANEL_LEVEL_MAX 1023. Normal mode (53 20) takes 10 bits; 51 07 FF is HBM (vendor panel_hbm_set
  sends 53 E8 first; lcm_aod_to_normal "normal mode backlight" 51 03 FF). With 2047 the level wrapped at 1024.
- **Console at wake**: sweetfb_noconsole muted the screen console only on the first *copied* frame; with every frame
  scanned out it never did, and kernel lines went into LK's buffer. Now muted on the first presented frame.
Heat seen at the same time (not a kernel fix): `aggregated` (PowerlogCore, SQLite) thread at ~92 % of a core since
boot, priority 4, so the governor held the LITTLE cluster at 2 GHz (cpu status: ll busy 93 %, peak 81 C).

### 0194: low-priority load manager in the CPU governor (NOT tested on the phone)
Heat on 0192/0193: PowerLog (`aggregated`) ran one SQLite thread at priority 4 at ~92 % of a core for hours; the
governor saw a busy A55 and held LL at 2 GHz / CCI at 1.4 GHz (93 % busy, SoC peak 81 C). Root cause of that load:
on 2026-11-07 PowerLog queued 14414 daily archive jobs (PLStorageOperator_EventPoint_Archive, Stage -1) from 1987 to
2026, because boots before 0151 (no RTC, calendar at 1970) left rows at 1970-01-01; each job copies and gzips the
16 MB database (~1.1 s). Not a missing service (the RTC is set before launchd since 0151, no newer 1970 rows); the
database was reset on the phone (aggregated booted out, BatteryLife/CurrentPowerlog.PLSQL* and Archives deleted,
bootstrapped again).
- **Scheduler** (osfmk/kern/processor.c): per CPU, time on threads with sched_pri <= viva_lowpri_max (4,
  MAXPRI_THROTTLE: background / maintenance QoS), accumulated at every processor state change (context switch, idle).
- **Governor** (viva_cpufreq.c): busy = foreground busy (total minus that time) for the target, the ceiling jump, hot
  CPUs and parking. Low-priority work alone still raises a cluster by the same rule on the total busy, at most to
  `vivacpubg` MHz (1075; 0 = off, 0192 behaviour). A boost, foreground threads and promoted threads get the full
  clock. Tune keys `bg`, `bgpri`; `cpu status` shows busy with/without low priority, low % per CPU since reset and
  the samples the cap held lower.

### 0195: the touch IC sleeps with the display (NOT tested on the phone)
Phone on 0194, display off on the Mac's charger for ~1.5 h, warm again: CPU at the floor (LL 500 MHz, 9 % busy, low
priority ~1 %), GPU idle, but `cpu status` counted 1,163,233 touch reports ignored with the display off (~230/s for
5077 s; "still" contact = the charger's noise read as a resting finger). Each one: EINT, an I2C read, a HID event to
backboardd. VivaGT9896 now does the vendor's suspend/resume (goodix_ts_i2c.c): display off -> interrupt disabled,
contacts lifted, COMMAND_SLEEP {05 00 00 00 05} to the command register 0x4160 (ADT reg-command); display on ->
hardware reset (2 ms low, 110 ms) and interrupt enabled. Called from AppleCLCD::requestPower through a thread call on
the touch work loop. `vivatouchsleep=0` = off. Tap-to-wake is not possible while it sleeps.

### 0196: LK's buffers cleared when the screen console goes quiet (NOT tested on the phone)
Phone on 0195: WiFi works again, Shut Down from the slider powers off, touch sleeps with the display (0 reports with
the display off, IC sleep sent), but kernel lines still showed at wake although the screen console was muted
(`console` -> off). They were the boot's lines left in LK's buffer 0, shown whenever the OVL went back to it before
CoreAnimation's next frame. When sweetfb_noconsole mutes the console (first presented frame), buffers 0 and 1 are now
filled with black (64-bit stores: bzero's DC ZVA faults on the non-cacheable mapping). A panic still draws.

### 0197: CCORR coefficient B<-R 13 bits (Night Shift purple) (NOT tested on the phone)
Phone on 0195: brightness slider OK; Night Shift turned the screen purple. Its type-9 matrices have small negative
off-diagonal terms (e.g. 1973 80 15 / -4 1914 2 / -12 -44 1806, 1.0 = 2048). CCORR_COEF(3) masked c[6] (B from R)
to 12 bits as mtk_disp_ccorr.c's CPU path does; the vendor's CMDQ path (the one it runs) uses 13 bits for all nine.
-12 & 0xfff = 4084 = about +2.0 in 13-bit signed: blue gained twice the red. Now 0x1fff.

### Viva Link server, journal and `watch` (2026-11-13, tools only)
Only one process can claim the link interface, so a long `log`, the interactive interface or two agents kept each
other out. `tools/viva-link.py` now always goes through a local server (`viva-link.py serve`, started in the
background by the first command, replaced when the tool's code is newer; `tools/viva_link_server.py`) that owns the
USB link and listens on `~/.viva-link/link.sock`. Every mode works as before; `VIVA_LINK_DIRECT=1` = old direct
access. The server journals everything with times to `~/.viva-link/journal/YYYY-MM-DD.log` (`L` kernel line, `O`
vivad output, `E` phone event, `C` command + reply, `S` link state), and beats only while a client is connected.
- Phone events: `tools/vivad/vevents.c` (built by tools/vivad/build.sh, ad-hoc signed with vevents.entitlements,
  copy to /private/var/root/viva-tests/vevents). The server starts it through vivad when a client attaches; it prints
  `EV app BUNDLE PID` (frontmost app from the Darwin role UI_FOCAL: SpringBoard's SBSCopyFrontmostApplication...
  answers only platform binaries), `EV touch down|up N X Y [MS DIST]` (HID monitor client), `EV key down|up NAME`,
  `EV lock 0|1`, `EV display 0|1`, `EV screenshot PATH` (DCIM), `EV crash PATH` (CrashReporter); it replaces an
  older instance and leaves 30 s after the link stops being active.
- `viva-link.py watch [-k REGEX] [--until REGEX [--count N] [--timeout S]]`: events as they happen; with `--until`
  a test waits for the user's action (e.g. `--until 'E app com.apple.mobilesafari'`), exit 0 / 2 on timeout.
- Tested with the simulator (`tools/tests/test_viva_link_server.py`, `test_viva_link_tui.py`) and on the phone
  (server, journal, one-shot commands and a watch at the same time; vevents saw touches, home, lock/display).
- Open: on 2026-11-13 vivad hung in posix_spawn twice when starting the latest vevents build (`vivad: not reading`,
  no "started" line), while the earlier builds ran; during the same boot wired memory was ~225k pages (3.4 GiB, the
  2026-11-07 hang). 0201 `ps vivad` should say where it waits.

### 0200: IOSurface pitch rounding off by default (Sileo icons, NOT tested as a kernel)
Phone 2026-11-13: Sileo's package icons were diagonal stripes, web pages in Safari had no text. Kernel log: 0184 P6
rounded client pitches to 64 (`SWEET SURF: 246x246 fmt 0x42475241 pitch 992 -> 1024`, LA08 glyph atlases 480 ->
512, 11 surfaces), but some writers keep their own pitch. With boot-arg `vivasurfpitch=0` (VIVABOOT record, same
kernel) the icons are right. 0200 makes 0 the default (1 / 2 = 0184 / 0187 behaviour). **Safari is a separate
problem**: still no text and black triangles with vivasurfpitch=0 (example.com, no WebContent crash, no GPU fault in
the log); WebKit's GPU process is the only user of IOSurface selector 44 (IOSurfaceClientSetOwnershipIdentity,
harmless). Next: Safari's feature flag "GPU Process: DOM Rendering", vmg errors with VMG_STDERR=auto.

### 0201: link `ps [NAME|PID]` and `kill PID [SIG]` (NOT tested on the phone)
`ps` alone: one line per process (`PS pid name threads N`) in the log. `ps NAME|PID`: per thread `PS name tN tid T
state W/R/U/S/T hint H event E upc ulr kbt ...`: block hint (kern/block_hint.h), wait event, user pc/lr and, for a
blocked thread, the kernel frame-pointer backtrace from the context saved at the switch (kernel collection addresses
unslid, heap addresses raw; symbolicate against the kernelcache). `kill PID [SIG]` (default 9, not pid 0/1): psignal
from the kernel, for a stuck vivad (launchd starts it again). Build tree for 0200+: `research/bt0200` (own git).

### 0202 / 0203: screenshot surface, native display switch (F1 of gpu/docs/PIANO-DISPLAY-NATIVO.md, NOT tested yet)
- 0202: `disp kv` gains `shown_surface` (IOSurface id the panel scans out), `offset_x`, `offset_y`; `viva-link.py shot`
  uses them.
- 0203: with `sweetfb_native=1` AppleCLCD::start also writes, before userland starts, `/arm-io/disp0` canvas-width/height
  (1080x2400), `/product artwork-device-subtype` (2556, `sweetfb_subtype`) and `display-corner-radius` <110 3>
  (`sweetfb_corner`, px; measured from photos 2026-11-13) and reports dot pitch 394; the touch surface is 69.55 x
  154.56 mm. The ADT stays the iPhone X one, so the same boot.img without the boot-arg is the old presentation.
- SpringBoard side: `gpu/app/tweak/VivaDisplay.m` inside VivaIsland.dylib (Viva Manager CI build), active only when
  disp0 canvas-width is 1080: island 100 x 29.33 pt at 2 pt, exclusion rect (390, 6, 300, 88) px, safe area top 53.67
  / bottom 34, storyboard apps native (classic mode 0). Preferences `it.viva.display` (DisplayHooks, IslandWidth, ...).
- Images (CI xnu 43524004489, app 43523979522): `out/xnu-ufs-rw-0203-native.img` (with sweetfb_native=1) and
  `out/xnu-ufs-rw-0203.img` (without). MobileGestalt cache backed up to `~/Developer/ios-viva/backup/` (not in git) and
  deleted right before the first native boot so that it is rebuilt.
- Back: flash `out/xnu-ufs-rw-0203.img` (or 0201), push the backed-up plist back to
  /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist
  (the island on the iPhone X canvas needs its hand-edited 2556), or turn the hooks off with DisplayHooks = NO.

### Native display on the phone (2026-11-14, images `out/xnu-ufs-rw-0203-native.img`, then `-0204-native.img`)
- 0203 + VivaDisplay: lock screen, home, Settings, Sileo (storyboard app: native) at 360x800 pt; island on the camera
  hole; status bar right once the exclusion rect went in points (px read as points put the time under the island).
- Flicker during animations (user): CoreAnimation used AppleCLCD's default surface (LK's buffer, surface 1, now the
  display's size) as a page and drew into it while the panel showed it ("copies" of 2 us, swaps of surface 1).
  **0204**: in native mode the default surface is a contiguous surface of its own (`SWEET FB: default surface 3 (own,
  contiguous)`); then 567 frames scanned out, 0 copied, a 12-frame burst of an app opening clean. Image with
  `vivafbpool=6` (the default surface takes a pool buffer).
- Status bar avoidance 150 pt (`it.viva.display` StatusBarAvoidWidth): time centred at ~58 pt (was 70).
- Open: expanded island title clipped at the top (Apple's content insets assume 11.33 pt), Control Center laid out for
  375 pt (modules left, ~56 pt free on the right), home screen grid of SBH type 4.

### 0205: dyld policy + VivaUIKit injection; keyboard fixed (2026-11-14, `out/xnu-ufs-rw-0205-native.img`)
- The keyboard on the 360 pt screen was 20 pt to the right (keys of the 320 layout stretched to 360 inside a 320 pt view
  centred by UIInputSetHostView): UIKit derives the keyboard view's width only for six exact reference sizes
  (_UIKeyboardGetDerivedKeyboardForSpecificOrientation: 320x693, 360x780, 390x844, 393x852, 428x926, 430x932). Analysis:
  gpu/docs/PIANO-DISPLAY-NATIVO.md section 9. Needs code in every app.
- 0205: MAC policy "AMFI" answers dyld (op 90, flags 0x6b: path variables, missing inserted library not fatal,
  interposing; `vivadyldpolicy`); exec_extract_strings adds DYLD_INSERT_LIBRARIES=/var/jb/usr/lib/VivaUIKit.dylib to
  executables inside an .app (not SpringBoard; `VIVA INJECT:` lines, the first 32). `vivainject=0`: neither.
- `gpu/app/tweak/VivaUIKit.m` (Viva Manager CI, installed by app/install.sh): Foundation + IOKit only; in native mode
  hooks -[UIKBScreenTraits keyboardScreenReferenceSize] 360x800 -> 360x780 once UIKitCore is loaded. Off:
  /var/jb/usr/lib/VivaUIKit.off or delete the dylib.
- Phone: boot without the dylib fine (apps start, insert failure ignored); with it Safari's keyboard is 360 pt wide at
  x 0, all ten keys and return visible.
- VivaDisplay (same day): home screen / dock / folders / App Library laid out for SBH screen type 3 (iPhone X) through
  +[SBHDefaultIconListLayoutProvider currentDeviceScreenType] (was 4, "X zoomed": small icons, rows far apart;
  preference HomeScreenType); the keyboard fix also inside SpringBoard (App Library search: SpringBoard is not
  injected). Phone: 60 pt icons, X spacing; App Library keyboard 360 pt at x 0. Spotlight and Sileo keyboards fine
  through VivaUIKit.dylib (user).
- Home grid nearer the island (VivaDisplay HomeTopShift 16 pt: the root lists' portrait top inset; first row at ~73 pt,
  was 90). The clipped "Viva Link · USB" title was VivaIsland's own overlay notice: its 72 pt pill centred the text
  across the system island (drawn above the overlay); the content now goes below the island and the pill grows to
  island height + 52 pt. VivaDisplay's ExpandedTopExtra (system expanded insets) stays 0 (untested).
- Freeze 01:48 (user forced a restart): again the wired-memory exhaustion (wired ~197k pages, 112 failed 10 MB
  vm_page_find_contiguous) after ~15 min with many SpringBoard restarts, screenshots and an app reinstall (uicache).
  Open: find what keeps that memory wired (IOSurfaces of killed SpringBoards? display pages?).
