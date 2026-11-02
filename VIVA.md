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
6. Keys (MT6366 PMIC via pwrap), touch (Goodix on MTK I²C), then SMP/perf, battery, GPU.

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

Workflow notes:
- Kernel patches are made against a local xnu-8796.141.3 tree with all
  `xnu/patches/*.patch` applied in order (`git apply`; 0031/0041 need the CI's HFS
  staging script and can be skipped locally). CI silently skips a patch that does not
  apply, so check each new patch with `git apply --check` on that tree.
- Single files compile-check locally with `tools/xnu-cc.py LOG BUILDTREE FILES` against the
  `xnu-buildtree` artifact (it holds the patched source: edit there, `git diff` = next patch)
  and the CI log (`gh run view RUN --log`); CI_DXB/CLANG in it are set for this fork.
- Build: `gh workflow run xnu.yml -R Nik300/ios-viva --ref viva` (~1 h), then
  `tools/viva-mkboot.sh KERNEL ~/Developer/ios-viva/stock/boot.img STAGE`; extra boot-args via
  `VIVA_CMDLINE`. Debug features stay compiled in and are switched by boot-args.
