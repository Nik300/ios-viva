#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--viva [--ufs]]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# Panic log buffer, carved from the TOP of the dram range and exposed to XNU as
# /pram + /chosen/embedded-panic-log-size. Without it, pe_init.c's
# check_for_panic_log() leaves gPanicBase == 0, debug_log_init() bails out twice
# and the kernel hangs right after "kdp_core zlib memory" — reproduced on QEMU
# and independently on ios-fajita.
PANIC_LOG_SIZE = 0x100000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

# --viva: Redmi Note 11 Pro 4G (MT6781). RAM is 0x40000000-0x1c0000000; every
# LK mblock reservation is below 0xc0000000 except dramc-rk1 at 0x1bffff000.
# start.s maps kernel RAM with 32MB cacheable blocks, so the range stops at the
# last 32MB boundary below dramc-rk1. Addresses from the viva DTB and the
# on-device probe (README of ios-viva).
VIVA_PHYS_BASE, VIVA_MEM_SIZE = 0xc0000000, 0x1be000000 - 0xc0000000
VIVA_GICD, VIVA_GICD_SIZE = 0x0c000000, 0x40000
VIVA_GICR, VIVA_GICR_SIZE = 0x0c040000, 0x200000
VIVA_FB, VIVA_FB_STRIDE = 0x7bc10000, 1088 * 4
VIVA_FB_SIZE = (VIVA_FB_STRIDE * 2400 + 0x3fff) & ~0x3fff
VIVA_TIMEBASE = 13000000
VIVA_VTIMER_INTID = 27  # DT timer <GIC_PPI 11>, the standard virtual timer
VIVA_ECID = 0x5649564100000001  # 'VIVA' + 1, synthetic
# MediaTek ufshci@11270000 (DT size 0x2300, one 16K page)
VIVA_UFS, VIVA_UFS_SIZE = 0x11270000, 0x4000

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


# Linux IRQ_TYPE_* in the second interrupt cell (SweetGIC.cpp)
IRQ_EDGE_RISING, IRQ_LEVEL_HIGH = 1, 4
# HID consumer-page usages (IOHIDUsageTables.h)
HID_CSMR_POWER, HID_CSMR_VOLUME_UP, HID_CSMR_VOLUME_DOWN = 0x30, 0xe9, 0xea


def spi(n, flags):
    """DT <GIC_SPI n flags> -> the ADT's <INTID flags>."""
    return u32(32 + n, flags)


def viva_devices(ph, phandle, gic_ph):
    """viva /arm-io devices with their own IOKit drivers (addresses and SPIs from the viva DTB)."""
    io = lambda pa, size: u64(pa - SOC_BASE, size)
    eint_ph = next(phandle)
    return [
        # MediaTek keypad (kp@10010000, SPI 73 edge): iokit/Kernel/arm/hid/VivaKeypad.cpp.
        # keymap = <bit usage> pairs (HID consumer page). Volume down is kpd-hw-init-map
        # index 0 = bit 0 (measured 2026-11-03); power and volume up are PMIC keys.
        node('keypad', [
            ph(),
            ('compatible', cstr('mediatek,mt6781-keypad')),
            ('reg', io(0x10010000, 0x1000)),
            ('interrupt-parent', u32(gic_ph)),
            ('interrupts', spi(0x49, IRQ_EDGE_RISING)),
            ('keymap', u32(0, HID_CSMR_VOLUME_DOWN)),
        ]),
        # External interrupts (eint@1000b000, GIC SPI 186 level): iokit/Kernel/arm/VivaEINT.cpp,
        # an interrupt controller under the GIC; children use interrupts = <EINT flags>.
        node('eint', [
            ('AAPL,phandle', u32(eint_ph)),
            ('compatible', cstr('mediatek,mt6781-eint')),
            ('reg', io(0x1000b000, 0x1000)),
            ('interrupt-controller', b''),
            ('#interrupt-cells', u32(2)),
            ('interrupt-parent', u32(gic_ph)),
            ('interrupts', spi(0xba, IRQ_LEVEL_HIGH)),
        ]),
        # PMIC wrapper (pwrap@1000d000): iokit/Kernel/arm/VivaPwrap.cpp, a bus for the MT6366
        # (VivaPMIC.cpp, interrupt EINT 144 level high) and its key inputs (hid/VivaPMICKeys.cpp:
        # keymap <key usage>, key 0 PWRKEY = power, 1 HOMEKEY = volume up).
        node('pwrap', [
            ph(),
            ('compatible', cstr('mediatek,mt6781-pwrap')),
            ('reg', io(0x1000d000, 0x1000)),
        ], [node('pmic', [
            ph(),
            ('compatible', cstr('mediatek,mt6366-pmic')),
            ('interrupt-parent', u32(eint_ph)),
            ('interrupts', u32(144, IRQ_LEVEL_HIGH)),
        ], [node('keys', [
            ph(),
            ('compatible', cstr('mediatek,mt6366-keys')),
            ('keymap', u32(0, HID_CSMR_POWER, 1, HID_CSMR_VOLUME_UP)),
        ])])]),
    ]


def build(seed=None, qemu=False, highmem=False, viva=False, viva_ufs=False):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py).
    viva=True: Redmi Note 11 Pro 4G addresses. Without viva_ufs there is no UFS node, so the
    kernel never touches storage; with it the kernel drives the MediaTek host LK left running."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    if viva:
        phys_base, mem_size = VIVA_PHYS_BASE, VIVA_MEM_SIZE
    # Carve the panic log off the top so the kernel never allocates over it.
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    gicd_size, gicr_size, vtimer = GICD_SIZE, GICR_SIZE, (QEMU_VTIMER_INTID if qemu else VTIMER_INTID)
    fb, fb_size, fb_stride, ecid = FB, FB_SIZE, FB_STRIDE, SWEET_ECID
    if viva:
        gicd, gicr, timebase = VIVA_GICD, VIVA_GICR, VIVA_TIMEBASE
        gicd_size, gicr_size, vtimer = VIVA_GICD_SIZE, VIVA_GICR_SIZE, VIVA_VTIMER_INTID
        fb, fb_size, fb_stride, ecid = VIVA_FB, VIVA_FB_SIZE, VIVA_FB_STRIDE, VIVA_ECID
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('viva-shim' if viva else 'sweet-shim')),
        ('system-firmware-version', cstr('viva-shim' if viva else 'sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', ecid)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32((2050000000 if core >= 6 else 2000000000) if viva else (2304000000 if core >= 6 else 1804800000))),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    # SPI interrupt controller of the /arm-io nubs (iokit/Kernel/arm/SweetGIC.cpp):
    # a device node has interrupt-parent = gic_ph and interrupts = <INTID flags>.
    gic_ph = next(phandle)
    gic = node('gic', [
        ('AAPL,phandle', u32(gic_ph)),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('#interrupt-cells', u32(2)),
        ('virtual-timer-intid', u32(vtimer)),
        ('reg', u64(gicd - SOC_BASE, gicd_size, gicr - SOC_BASE, gicr_size)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])] + (viva_devices(ph, phandle, gic_ph) if viva else []))

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb, fb_size)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(fb_stride)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('mediatek,mt8183-ufshci') if viva else cstr('qcom,ufshc')),
        ('reg', u64(VIVA_UFS, VIVA_UFS_SIZE) if viva else u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Panic log region. /pram reg = <phys size>; it sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('qcom,sm7150') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([ufs] if (viva_ufs if viva else not qemu) else []))


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv, viva='--viva' in sys.argv,
                 viva_ufs='--ufs' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
