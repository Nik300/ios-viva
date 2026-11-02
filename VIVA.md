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
| GICv3 | GICD 0x0c000000, GICR 0x0c040000 (DT size 0x200000: redistributor stride still to verify) |
| Timer | 13 MHz, virtual timer PPI 11 = INTID 27 |
| CPUs | 6× Cortex-A55 (MPIDR 0x000–0x500), 2× big (0x600, 0x700) |
| UFS | MediaTek ufshci at 0x11270000 (not used yet) |

## Safety

The LUN that holds `userdata` on MediaTek UFS also holds `nvram`, `proinfo`,
`seccfg`, etc. The `--viva` ADT has **no `/sweet-ufs` node**, so the kernel
does not touch storage at all, and `tools/viva-mkboot.sh` turns off every
sweet Qualcomm driver (keys, touch, USB, persistent log) whose fixed MMIO
addresses are other blocks on MT6781. Only `boot_b` is ever flashed.

## Build and boot

```sh
# 1. kernel (GitHub Actions, macos-14)
gh workflow run xnu.yml -R YOU/ios-viva --ref viva
gh run download RUN_ID -R YOU/ios-viva -n xnu-kernel -D xnu/out
# 2. boot image (stock boot.img from the viva fastboot ROM, EEA OS1.0.3.0.TGDEUXM)
tools/viva-mkboot.sh xnu/out/kernel.development.sweet STOCK/boot.img
fastboot flash boot_b shim/out/viva-boot.img
fastboot reboot
```

`shim/build-viva.sh STOCK/boot.img` without a payload builds a shim-only image
that stops at "no SWEETXNU payload": a quick check of the shim and of LK's
initrd properties.

## Milestones

1. ✅ Own code runs (gzip Image), framebuffer drawable, text console, live RAM map.
2. ✅ XNU boots to root mounting (2026-11-02, CI run 41525615211, 1 CPU): panics as designed with
   `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6` because the `--viva` ADT has no UFS node.
3. Storage: MediaTek UFS (bring-up from LK's state), root in `userdata`.
4. Lock screen; then touch (Goodix on MTK I²C) and keys (MT6366 PMIC).
