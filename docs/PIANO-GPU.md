# GPU su viva: piano (bozza 1, 2026-11-03)

Obiettivo: Metal **vero** su iOS 16.7.16, sulla GPU del Helio G96 (MT6781), una **Mali-G57 MC2** (Valhall v9,
job manager). Si parte da [metal-shim](../../../metal-shim) (driver Metal per macOS Tahoe 26 che traduce verso
OpenGL 4.1 e OpenCL 1.2 su una Radeon TeraScale) e da **Mesa** (Panfrost: compilatore Valhall e driver Gallium).
Questo documento è solo il piano: nessun codice è stato scritto.

## Filosofia (ereditata da metal-shim, adattata)
- Le app e i framework devono **trovare un device Metal e funzionare**. Quello che il Mali fa in modo efficiente
  si esegue davvero, il resto diventa un fallback economico o un oggetto valido ma inerte. Mai `nil` dove ci si
  aspetta un oggetto.
- **Interruttore generale**: senza la GPU (boot-arg `vivagpu=0`) il kernel non pubblica l'acceleratore, Metal vede
  0 device e iOS disegna sulla CPU come oggi. Tutto il lavoro resta reversibile senza riflashare `userdata`.
- I processi critici (prima di tutto `backboardd`, che compone l'intera interfaccia) passano alla GPU **per ultimi**,
  dopo app di prova, app vere e recupero dai blocchi della GPU.
- Il bersaglio è il Metal di **iOS 16 per A11** (l'identità del telefono è iPhone X, `iPhone10,3`): famiglia
  **Apple4**, non Metal 3 e tanto meno Metal 4 di Tahoe. Quello che Tahoe ha in più si toglie, ma Apple4 ha cose che
  macOS/Mac2 non aveva (vedi la tabella delle funzioni).

## Cosa cambia rispetto a metal-shim
| | metal-shim (macOS Tahoe 26, Radeon HD 6750M) | viva (iOS 16.7.16, Mali-G57 MC2) |
|---|---|---|
| Driver di kernel | quello di AMD/OCLP, già presente | **da scrivere**: alimentazione, MMU, job manager, interfaccia utente (patch XNU `01xx-viva-gpu-*`) |
| Back-end grafico | OpenGL 4.1 core (CGL) + OpenCL 1.2 | **Gallium/Panfrost di Mesa** come libreria statica dentro il bundle; niente GL né CL in mezzo |
| Shader | AIR → GLSL 4.10 (testo), AIR → OpenCL C | **AIR → NIR** → compilatore Valhall di Mesa (binari nativi, niente testo) |
| Memoria dei buffer `device`/`constant` | texture buffer R32UI + offset (GL 4.1 non ha SSBO) | **puntatori GPU a 64 bit veri** (load/store globali di Valhall): layout e aritmetica di puntatori esatti |
| Famiglia dichiarata | Mac2 + Common3 | **Apple4** (+ Common), come l'A11 |
| Registrazione | kext Lilu che aggiunge `MetalPluginName` all'acceleratore AMD | il **nostro** kernel pubblica un servizio di classe `IOAcceleratorES` con `MetalPluginName`/`MetalPluginClassName` (verificato, vedi "Fatti verificati") |
| Posizione del bundle | `/Library/GPUBundles` | `/System/Library/Extensions/VivaMTLDriver.bundle` sulla root HFS+ scrivibile (percorso fisso in Metal di iOS 16) |
| Library validation | deroga in AMFI tramite kext | AMFI è già la nostra (patch 0025/0035/0036): da verificare solo che un bundle firmato ad-hoc venga caricato |
| Presentazione | `CAMetalLayer` → IOSurface → `CGLTexImageIOSurface2D` | IOSurface del nostro `SweetSurface` importate come BO della GPU (zero copie); a schermo con `AppleCLCD` |
| Componenti tolti | — | Liquid Glass (`TSGlass*`), TeraScale Swift, interposer, spoof degli entitlement e del modello, app di controllo macOS, OpenCL |

### Cosa si riusa di metal-shim
- **Frontend Metal** (`TSMtlCommon`, `TSMtlDevice`, `TSMtlCommands`, struttura di `TSMtlResources`/`TSMtlRender`/
  `TSMtlPipeline`, `TSMtlLayer`): la logica di encoder, command buffer, coerenza CPU/GPU, IOSurface, query di
  visibilità, catch-all `forwardInvocation:`, statistiche e log. Le chiamate GL (circa 700 in quattro file) vengono
  sostituite da chiamate Gallium.
- **Lettura dell'AIR** (`TSAirTranslator.m`, `TSLLVM.h`): libLLVM via API C, pulizia con `LLVMRunPasses`, metadati
  `!air.*`, varying, sampler `constexpr`, funzioni visibili collegate, function constant, inlining. Cambia solo
  l'emissione: NIR invece di GLSL. Molti problemi risolti in GLSL (array di array, type punning, `extract_bits`,
  pack/unpack) in NIR spariscono o diventano banali.
- **Test e strumenti**: `tests/phase1-3`, `translate`, `cfg`, `uploads`, `tools/airdump`, `airtrans`, la cache su
  disco e l'idea delle `.mslbundle` (shader precompilati).
- **Esperienza di `TSAirOpenCL.m`** per il compute (barriere, threadgroup memory, atomiche), riscritta in NIR.

## Architettura
```
App / framework iOS (QuartzCore, CoreImage, VectorKit, WebKit/ANGLE, SpriteKit, ...)
   │ Metal.framework (shared cache di iOS 16)
   ▼
VivaMTLDriver.bundle                                     (userspace, Objective-C + C)
   ├─ frontend Metal (da metal-shim): device : _MTLDevice di iOS 16, code, command buffer, encoder, risorse
   ├─ shader: AIR (bitcode di _MTLFunction) → libLLVM → NIR → compilatore Valhall (Mesa) → binario + cache su disco
   ├─ back-end: Gallium (pipe_screen/pipe_context di Panfrost, Mesa statico, simboli nascosti)
   │     └─ "kmod" xnu: BO, VM, submit, syncobj ──► IOConnectCallMethod
   └─ IOSurface: import nel kernel come BO (texture lineari), CAMetalLayer
   │
   ▼ IOUserClient
XNU (patch 01xx-viva-gpu-*)                              (kernel, C++)
   ├─ VivaMFG:   MTCMOS (SPM), clock (topckgen/apmixed/mfgcfg), regolatori vgpu/vsram_gpu (MT6366 via pwrap)
   ├─ VivaMali:  reset, power di L2 e core, MMU (tabelle 4K costruite da pagine XNU da 16K), job slot, IRQ, recupero
   ├─ VivaGPUUserClient: semantica di DRM_PANFROST_* (CREATE_BO, MMAP, SUBMIT, WAIT_BO, MADVISE, GET_PARAM)
   │                    + syncobj, + import di IOSurface
   ├─ servizio "acceleratore" con le proprietà che Metal di iOS legge (MetalPluginName, ...)
   └─ SweetSurface (0046) esteso: elenco pagine, allineamenti adatti alla GPU
```
Scelta chiave: **Gallium diretto invece di GLES**. Gallium è già un'interfaccia a oggetti senza stato globale
(CSO per blend/depth/raster, sampler view, constant buffer, SSBO, `draw_vbo`, `launch_grid`), molto vicina a Metal, e
porta gratis blit, clear, mipmap, query e layout delle texture (`u_blitter`, AFBC, u-interleaved). Un passaggio per
GLES 3.1 aggiungerebbe una macchina a stati, un compilatore GLSL sul Cortex-A55 e le limitazioni di GLES (niente
puntatori, niente texture rectangle, estensioni sparse), senza riuso reale: il back-end GL di metal-shim è scritto per
il GL desktop 4.1 e andrebbe riscritto comunque.

Scelta chiave 2: **lo sviluppo del back-end si fa sul Mac**. Lo stesso codice Gallium gira con **llvmpipe** (driver
Gallium software di Mesa, compila su macOS): back-end, traduttore AIR → NIR e test di metal-shim si portano a verde sul
Mac, con le pipeline di Tahoe, prima di toccare il telefono. Sul telefono cambia solo il `pipe_screen` (Panfrost al
posto di llvmpipe). Il compilatore Valhall di Mesa compila anche sul Mac, per le statistiche degli shader e per la
cache precompilata.

## Funzioni di Metal (iOS 16, Apple4) e come vanno sul Mali-G57
| Funzione | Trattamento |
|---|---|
| Device, `supportsFamily`, limiti | Apple4 + Common; limiti presi da Panfrost (`pipe_caps`) e ridotti dove Apple4 promette meno; `threadExecutionWidth` = 16 (warp di Valhall). Il profilo/le ivar private di `_MTLDevice` di iOS 16 vanno riscoperti (G0) |
| Buffer shared/private | BO della GPU mappati write-combined nel processo (il Mali sul MT6781 probabilmente non è coerente con le cache della CPU: da verificare); `contents` = mappatura diretta, **niente copie** (diversamente da metal-shim) |
| Puntatori `device`/`constant` negli shader | indirizzi GPU a 64 bit (`load_global`/`store_global`); i `constant` piccoli e a indice costante vanno nei constant buffer (più veloci, push constant/FAU) |
| Texture 1D/2D/3D/cube/array, mip, viste | risorse Gallium; viste con `pipe_sampler_view` (swizzle e cambio di formato di pari dimensione nativi) |
| Formati | RGBA8/BGRA8 (+sRGB), 16F/32F, interi, depth32F/16/24S8, **ASTC LDR e ETC2 nativi** (iOS li usa molto). PVRTC: decodifica CPU al caricamento. **XR10** (`BGRA10_XR`, `BGR10_XR`, formati estesi del P3): emulati come RGBA16F con conversione negli shader, oppure si dichiara il display non wide-color (da decidere in G5 dopo aver visto cosa usa CoreAnimation) |
| **Programmable blending** (`[[color(n)]]` letto nel fragment) | **nativo**: lettura del tile buffer di Valhall (il percorso di Panfrost per framebuffer fetch). È la funzione più importante per CoreAnimation su iOS (le varianti `render_target` di QuartzCore, che su macOS restavano nel ripiego, qui sono il percorso principale) |
| Texture `memoryless` | Mali è tile-based come le GPU Apple: niente writeback (invalidate alla fine del pass); la memoria si alloca lo stesso, da un pool condiviso |
| Load/store action, resolve MSAA | clear di tile (gratis), `dontCare` = invalidate, resolve al writeback del tile dove Panfrost lo fa, altrimenti blit |
| Tile shader, imageblock, raster order group (Apple4) | non esistono su Valhall: tile shader e imageblock → oggetti inerti con log; i casi semplici (imageblock = attachment colore) si emulano con framebuffer fetch; ROG ignorati |
| Vertex/fragment, MRT fino a 8, dual source, MSAA 4×, depth bias, clip distance | nativi in Gallium/Panfrost |
| half/short | fp16 nativo del Mali (metà registri, doppio throughput) |
| Draw indiretti, instanced, base vertex/instance | nativi |
| Compute | `launch_grid`, threadgroup memory = shared memory, barriere, atomiche su buffer e threadgroup; quad-group ops (Apple4) con le operazioni di sottogruppo di NIR. SIMD-group oltre il quad: Apple4 non le promette, si traducono comunque dove NIR lo permette |
| Argument buffer (tier 1 di Apple4) | buffer e valori: memoria GPU letta con puntatori (gratis). Texture e sampler: l'encoder registra indici, al draw si legano a una tabella di slot e lo shader indicizza la tabella in modo dinamico (Valhall lo supporta) |
| ICB | emulati: esecuzione CPU al momento del submit (lettura del buffer quando scritto dalla GPU, con attesa) |
| Fence, MTLEvent, MTLSharedEvent | syncobj del kernel (anche timeline); ordine implicito dentro la coda |
| Query di visibilità | `PIPE_QUERY_OCCLUSION_*` di Panfrost, scritte nel buffer al completamento (come metal-shim) |
| Heap | sub-allocazione dentro un BO grande; `makeAliasable` rispettato |
| Function constant, funzioni visibili, dynamic library | specializzazione dell'AIR prima della traduzione (già in metal-shim); dynamic library → link dell'AIR |
| Ray tracing, mesh shader, MetalFX, rasterization rate map, sparse, residency set, MTL4 | non in Apple4/iOS 16: `supports*` = NO, oggetti inerti |
| MPS (MetalPerformanceShaders) | probabilmente legato alle GPU Apple (librerie per famiglia): da provare in G9; altrimenti spento come su macOS |
| Cattura, contatori, timestamp GPU | spenti (timestamp CPU dove serve) |

## Fasi
Ogni fase ha un criterio di chiusura verificabile. Le fasi kernel (G1-G2) e Mac (G3-G4) possono procedere in
parallelo dopo G0.

| Fase | Contenuto | Si chiude quando | Dove |
|---|---|---|---|
| **G0 – Ricognizione e infrastruttura** | vedi sotto | tutte le domande di G0 hanno risposta con prova; file e comandi arrivano sul telefono senza riflashare | Mac + telefono |
| **G1 – Accensione** | MTCMOS, clock, regolatori, reset, lettura di `GPU_ID`, accensione di L2 e dei 2 core | `VIVA GPU: id ... cores 2 l2 on` sulla console; IRQ di "reset completato" ricevuto via SweetGIC | kernel |
| **G2 – MMU e job** | tabelle delle pagine, address space, job slot, IRQ di job/MMU, primo job `WRITE_VALUE`, poi un job di fragment che pulisce una IOSurface | `gpu selftest` dal link USB: valore scritto dalla GPU, IOSurface riempita di un colore e visibile a schermo; un errore MMU volontario recuperato senza panic | kernel |
| **G3 – Mesa su Darwin** | Mesa statico per iOS arm64 e macOS; kmod xnu; prova Gallium in C sul telefono | triangolo disegnato da Panfrost in una IOSurface e mostrato da `AppleCLCD`; sul Mac `panfrost` compila shader offline e llvmpipe disegna | Mac + telefono |
| **G4 – Back-end Gallium e AIR → NIR** | metal-shim con back-end Gallium, sul Mac con llvmpipe | `make test` di metal-shim verde con `TSMTL_BACKEND=gallium` (phase1-3, translate, cfg, uploads) e copertura dei metallib di sistema misurata | Mac |
| **G5 – Driver Metal per iOS 16** | frontend sull'ABI privata di iOS 16, bundle nella root, servizio acceleratore, funzioni Apple4 | app di prova sul telefono: `MTLCreateSystemDefaultDevice` ≠ nil, triangolo in un `CAMetalLayer` a schermo, test phase1-3 verdi su Panfrost | telefono |
| **G6 – Completezza Apple4** | compute, argument buffer, ICB, eventi, heap, formati compressi, programmable blending | test `translate`/`compute` verdi su Panfrost; metallib di QuartzCore, CoreImage, VectorKit tradotti al 100% (o elenco motivato delle eccezioni) | Mac + telefono |
| **G7 – Sistema** | CoreAnimation sul server di render (`backboardd`), CoreImage, cache precompilata, recupero dai blocchi | SpringBoard composto dalla GPU per 8 h senza blocchi; fps e carico CPU misurati contro il rendering CPU | telefono |
| **G8 – Prestazioni** | scanout diretto delle IOSurface (niente memcpy in `AppleCLCD`), DVFS, power gating, AFBC, costo CPU per draw | 60 Hz nelle animazioni di SpringBoard; consumo a riposo misurato | kernel + bundle |
| **G9 – Compatibilità** | Mappe, Foto, Safari (WebGL), giochi, Fotocamera (anteprima, quando ci sarà) | elenco di app che funzionano, con gli stub aggiunti per ciascuna | telefono |

### G0 – Ricognizione e infrastruttura
Domande, ognuna da chiudere con una prova (comando + output) in questo file:
1. ✅ **Come Metal di iOS 16 trova il driver**: risposto offline (vedi "Fatti verificati"). Resta da provare sul
   telefono che `NSBundle` carichi un bundle **fuori dalla shared cache** (l'AGX vive solo nella cache).
   Da chiarire: cosa fa QuartzCore con `IOGPUDeviceCreate`/`IOGPUDeviceSetDisplayParams` (soft-link a IOGPU.framework,
   che parla con il user client di `IOGPUFamily`) quando trova un device: se il server di render lo chiama, il nostro
   user client dovrà rispondere a quei selettori o QuartzCore dovrà tollerarne il fallimento.
2. **ABI privata di `_MTLDevice` di iOS 16**: feature profile per Apple4, ivar `_limits`, selettori delle varianti
   (`newVertexVariantWithCompilerOutput:…` ecc.), `bitcodeData` di `_MTLFunction`, compilatore (MTLCompilerService,
   plugin). Gli strumenti `re/introspect`, `protocheck` di metal-shim si ricompilano per iOS e girano sul telefono.
3. ✅ **libLLVM su iOS**: `/usr/lib/libLLVM.dylib` (LLVM 31001.722.3.1) è nella shared cache ed esporta 958
   funzioni dell'API C (`LLVMParseBitcodeInContext2`, `LLVMGetBitcodeModule*`, ...): `TSLLVM.h` funziona com'è, con
   `dlopen` di quel percorso. Da verificare sul telefono: `LLVMRunPasses` e i pass usati da metal-shim.
4. **Cosa usa CoreAnimation**: estrarre `default.metallib` di QuartzCore, CoreImage, UIKit/UIKitCore, VectorKit,
   RenderBox dalla shared cache di iOS 16 e passarli in `tools/airdump`: formati, programmable blending, memoryless,
   tile shader, XR10, function constant. Questo decide le priorità di G5-G6.
5. **Hardware**: sequenza di accensione dal kernel Xiaomi `viva-r-oss` (`gpufreq` per mt6781, `mtk-pmic-wrap`,
   MTCMOS in `scpsys`/SPM, mux `mfg_sel`, MFGPLL in apmixed, CG in `mfgcfg@13fbf000`), tabella OPP, `GPU_ID` atteso
   (famiglia v9 "TNAX"), coerenza con la CPU (ACE-Lite o no), registri lasciati da LK (con `peek` dal link USB: la GPU
   è probabilmente spenta). Riferimento strutturale pubblico: supporto mainline di MT8192 (anche lui Mali-G57) in
   Linux, solo come documentazione del comportamento (vedi "Licenze").
6. **Versione di Mesa**: ultima stabile all'avvio di G3, con `pan_kmod` (astrazione kmod di Panfrost) e il supporto
   v9 nel driver Gallium. Si fissa un commit e le modifiche vanno in `mesa/patches/*.patch`, come per XNU.

Infrastruttura (prima di qualsiasi codice GPU, perché ogni ciclo oggi costa un flash di `userdata` da 16 GB):
- **Agente userspace `vivad`** (LaunchDaemon nella root) collegato al link USB tramite un user client del kernel:
  `push FILE`, `pull FILE`, `exec CMD` con stdout/stderr nel link, `kill`. Host: nuovi comandi in
  `tools/viva-link.py`. Così bundle, Mesa e test si aggiornano in secondi.
- **Prova del code signing**: un eseguibile e un bundle firmati ad-hoc caricati da un processo di sistema
  (`dlopen`), con la nostra AMFI.
- **Test runner sul telefono**: i test di metal-shim compilati per iOS arm64, eseguiti da `vivad`, risultati e
  immagini (`imgdiff`) riportati sul Mac.

### G1 – Accensione (kernel, patch `0100-viva-gpu-power`)
- `VivaMFG` (nub ADT `/arm-io/mfg` generato da `mkadt.py --viva`):
  - regolatori: `vgpu` (buck) e `vsram_gpu` (LDO) dell'MT6366. Serve un **percorso di scrittura** in `VivaPwrap`
    (oggi solo lettura, 0095/0096 scrivono solo i registri di interrupt): elenco bianco limitato ai registri di
    tensione/abilitazione di quei due regolatori, con vincolo `vsram ≥ vgpu` come nel `gpufreq` di MediaTek;
  - clock: mux `mfg_sel` in topckgen, MFGPLL in apmixed, gate di `mfgcfg`;
  - MTCMOS nell'ordine `mfg_async` → `mfg` → `mfg_core0` → `mfg_core1` (registri SPM a `0x10006000`, sequenza da
    `viva-r-oss`), con le protezioni del bus (infracfg) da togliere e rimettere;
  - una sola OPP fissa (media) all'inizio; nessun DVFS fino a G8.
- `VivaMali` (MMIO `0x13000000`, 16 KiB): lettura di `GPU_ID`, feature (`L2_FEATURES`, `SHADER_PRESENT`,
  `MMU_FEATURES`, `AS_PRESENT`, `JS_PRESENT`), soft reset, accensione di L2 e shader core, quirk di Panfrost per v9.
- Interrupt dal DTB: SPI 274-278 (`GPU`, `MMU`, `JOB`, `EVENT`, `PWR`), livello alto → INTID 306-310 tramite SweetGIC
  (0094). Si usano GPU, JOB e MMU; `EVENT`/`PWR` sono del driver kbase di ARM e restano mascherati.
- Comandi del link USB: `gpu id`, `gpu on`, `gpu off`, `gpu regs`.

### G2 – MMU e job manager (patch `0101-viva-gpu-mmu-jobs`, `0102-viva-gpu-userclient`)
- **MMU**: formato delle tabelle da scegliere in G0 (il G57 dovrebbe supportare sia quello LPAE di Mali sia
  AARCH64_4K). Granulo 4 KiB contro le pagine XNU da 16 KiB: ogni pagina XNU = 4 PTE consecutive; le tabelle stesse
  stanno in pagine XNU wired, non cacheabili o pulite esplicitamente. Un address space per processo, assegnato agli AS
  hardware a rotazione (LRU), flush/lock degli AS come Panfrost.
- **Job**: due slot (JS0 fragment; JS1 vertex/tiler/compute), coda FIFO per slot con dipendenze (syncobj in
  ingresso), IRQ di completamento → segnale del syncobj in uscita. **Timeout e reset** con errore riportato al
  command buffer (requisito per G7). Fault MMU → errore al processo e AS disattivato, mai panic.
- **Memoria**: BO da pagine wired (`IOBufferMemoryDescriptor`), mappati nel processo write-combined (o cacheabili con
  manutenzione esplicita, se G0 dice che la GPU è coerente); `MADVISE` per la cache di BO di Mesa; heap crescente
  per il tiler (memoria "growable" su fault, come Panfrost) oppure heap fisso dimensionato all'inizio.
- **IOSurface**: `SweetSurface` espone l'elenco delle pagine; import di una surface come BO (stessa memoria, nessuna
  copia), pitch allineato a 64 byte, contenuto lineare. Le surface restano vive finché un BO le usa.
- **User client** (`VivaGPUUserClient`, aperto da `IOServiceOpen` sul servizio acceleratore): selettori con gli stessi
  campi delle ioctl `DRM_PANFROST_*` (così il lato Mesa cambia poco) più `SYNCOBJ_CREATE/DESTROY/WAIT/RESET/SIGNAL`,
  `IMPORT_IOSURFACE`, `GET_PARAM` con i valori letti in G1. Mappatura dei BO con `clientMemoryForType`.
- **Prova**: `gpu selftest` dal link (WRITE_VALUE + fragment clear su una IOSurface) e lo stesso da userspace con
  `vivad exec gputest`.

### G3 – Mesa su Darwin (repo `mesa/` in questo fork: script di build + `mesa/patches/`)
- Build con meson, cross per iOS arm64 (SDK iphoneos, deployment target 16.0) e nativa per macOS; solo: `util`,
  `compiler/nir`, compilatore Panfrost (Bifrost/Valhall), `panfrost/lib` (GenXML, descrittori, layout), driver
  Gallium `panfrost`, `gallium/auxiliary`; su macOS anche `llvmpipe` (con LLVM di Homebrew, solo per i test).
  Niente frontend GL/EGL/Vulkan. Libreria statica, `-fvisibility=hidden`, linkata nel bundle.
- **kmod xnu**: backend di `pan_kmod` che parla con `VivaGPUUserClient` (BO, VM, submit). Le poche chiamate libdrm
  rimaste nel driver Gallium (syncobj, versione, prime) vanno in un `libdrm-xnu` minimo; `mmap` del BO sostituito da
  `IOConnectMapMemory64`; i dmabuf diventano IOSurface. Patch piccole e numerate.
- Rischi da verificare subito: pagine da 16 KiB (`os_get_page_size`) dove Panfrost assume 4 KiB; dipendenze Linux
  (`libsync`, `sync_file`, `memfd`, futex) da disattivare o sostituire con equivalenti Darwin.
- **Prova sul telefono**: programma C `pantri` (crea lo screen, una IOSurface come render target lineare, clear e
  triangolo, presenta con `IOMobileFramebuffer`). Sul Mac: compilazione offline degli shader con il compilatore
  Valhall (statistiche: registri, istruzioni, spill).

### G4 – Back-end Gallium e traduttore AIR → NIR (in metal-shim, sul Mac)
- Back-end selezionabile (`TSMTL_BACKEND=gl|gallium`) dietro un'interfaccia interna, così la versione macOS esistente
  non si rompe.
- **Risorse**: `pipe_resource` per buffer e texture; buffer `shared` = mappatura persistente (niente upload a blocchi:
  non servono più); IOSurface importate.
- **Encoder**: render pass → `pipe_framebuffer_state` + clear/invalidate; stato precalcolato come CSO (blend, depth-
  stencil, rasterizer, vertex elements) creati con la pipeline Metal; draw → `draw_vbo`; blit → `resource_copy_region`/
  `blit`/`u_blitter`; compute → `launch_grid`.
- **AIR → NIR**: stessa lettura e pulizia dell'AIR di oggi; emissione con `nir_builder` in forma non strutturata e
  poi `nir_lower_goto_ifs` (lo stesso percorso di `spirv_to_nir` per OpenCL), così non serve più la macchina a stati né
  `structurizecfg`. Memoria `device`/`constant` = `nir_var_mem_global` a 64 bit; `threadgroup` = `mem_shared`;
  privata = variabili locali; texture/sampler da `!air.*`; intrinseci `air.*` mappati sugli intrinseci NIR (la tabella
  di `TSAirTranslator.m` è il punto di partenza). Poi i pass standard di Mesa (`nir_lower_io`, `nir_opt_*`) e il
  compilatore Valhall/llvmpipe.
- **Chiusura**: test di metal-shim verdi su llvmpipe, copertura dei metallib di Tahoe (VectorKit, QuartzCore,
  RenderBox) come base di confronto con il traduttore GLSL.

### G5 – Driver Metal per iOS 16 (bundle + kernel)
- **Kernel**: la classe `IOAcceleratorES` (sottoclasse di `IOService`, definita nella patch perché il nostro kernel non
  ha `IOGPUFamily`) e sotto di essa `VivaGPUAccelerator`, nub `/arm-io/gpu`, con `MetalPluginName = VivaMTLDriver` e
  `MetalPluginClassName = VivaMtlDevice` (obbligatoria: senza, Metal usa `NSPrincipalClass`). Metal fa
  `IOServiceMatching("IOAcceleratorES")` e poi `[[cls alloc] initWithAcceleratorPort:service]`. Con `vivagpu=0` il
  servizio non viene pubblicato.
- **Classe del device**: `VivaMtlDevice : _MTLDevice` direttamente, come `TSMtlDevice` su macOS, **non**
  `IOGPUMetalDevice`: quella catena (`IOGPUMetalCommandQueue`, `IOGPUMetalBuffer`, ...) presuppone il user client
  closed source di `IOGPUFamily` (`s_new_resource`, `s_submit_command_buffers`, memoria condivisa per i comandi).
  Se G5 mostra che qualche framework controlla `isKindOfClass:IOGPUMetalDevice`, si valuta un IOGPU minimo.
- **Compilatore**: come su macOS, la traduzione avviene nel processo leggendo `-[_MTLFunction bitcodeData]` e
  sovrascrivendo la creazione delle pipeline; nessun plugin per `MTLCompilerService`. Un plugin
  (`MTLCompilerCreate`/`BuildRequestWithOptions`/`ReleaseReply`/`Delete` e gli `AIRNT*`, percorso sotto
  `/System/Library/Extensions/*.bundle` o `/System/Library/PrivateFrameworks/*.framework`) resta un'opzione per
  spostare la compilazione fuori dalle app, se servirà.
- **Bundle** `VivaMTLDriver.bundle` nella root (percorso da G0), compilato per arm64 iOS 16, firmato ad-hoc.
  Frontend portato sull'ABI di iOS 16: profilo Apple4, limiti, classi base delle code/encoder di iOS 16, catch-all.
- **Policy** (`/private/var/db/vivagpu/policy.plist` + boot-arg): all'inizio solo un elenco di processi abilitati
  (le app di prova); i processi critici (`backboardd`, `SpringBoard`, demoni) restano a 0 device finché G7 non li
  accende uno per volta. Stesso schema di decisione di metal-shim (`critical` / `list` / `default`), log su
  `os_log` e una riga per processo nel log del kernel tramite il link.
- **Presentazione**: `CAMetalLayer` crea le IOSurface con il framework IOSurface (che parla con `SweetSurface`); il
  bundle le importa (`IMPORT_IOSURFACE`) e ci disegna direttamente. Almeno 3 drawable, come su macOS.
- **App di prova** `MetalTest.app` installata nella root (triangolo, texture, MRT, programmable blending, compute)
  più i test di metal-shim come eseguibili.

### G6 – Completezza Apple4
Compute completo (atomiche, barriere, quad-group), argument buffer, ICB emulati, MTLEvent/MTLSharedEvent su syncobj,
heap, ASTC/ETC2/PVRTC, XR10, programmable blending in tutte le combinazioni, memoryless, `.mslbundle` per iOS:
i metallib di sistema di G0 tradotti offline sul Mac in binari Valhall e messi nella root (QuartzCore e CoreImage
compilati all'avvio costerebbero secondi sul Cortex-A55).

### G7 – Sistema
1. App vere che usano Metal in-process (Mappe/VectorKit).
2. **`backboardd`**: il server di render di CoreAnimation passa da software a Metal quando trova un device. Prima di
   accenderlo: recupero dai blocchi della GPU provato (un job infinito volontario non deve bloccare l'interfaccia),
   memoria della GPU limitata e contata, `vivagpu=0` sempre disponibile da fastboot (immagine di boot alternativa
   già pronta).
3. CoreImage (sfocature, filtri delle foto) e il resto dei framework.

### G8 – Prestazioni
- **Scanout diretto**: oggi `AppleCLCD` copia ogni fotogramma (10 MB) nel framebuffer di LK con la CPU; con la GPU
  questa copia diventa il collo di bottiglia. Si riprogramma il layer dell'OVL del display MediaTek (registro
  indirizzo/pitch/offset del layer, applicato al vsync tramite il mutex del display a `0x14001000`) per leggere
  direttamente l'IOSurface presentata, con il ritaglio 1125×2436 → 1080×2400 fatto dall'OVL. Ripiego: copia fatta
  dalla GPU.
- DVFS con la tabella OPP di `viva-r-oss` (`gpufreq`), spegnimento dei core a riposo, AFBC per le risorse private
  (Panfrost lo fa da solo), costo CPU per draw (metal-shim è a 2,8 µs/draw sull'iMac: da rimisurare sull'A55).

### G9 – Compatibilità
Come la Fase 5 di metal-shim: un'app per volta, stub e correzioni per ciascuna, elenco in questo file.

## Licenze
- Mesa (MIT) si può includere e modificare nel bundle.
- Il driver `panfrost` di Linux è **GPL-2**: non si copia codice nel kernel XNU (APSL) né in questo repo (MIT).
  Il driver di kernel si scrive da zero; come documentazione dei registri si usano le descrizioni MIT di Mesa
  (GenXML, `pan_*`), la documentazione pubblica di ARM e il comportamento osservato. Lo stesso vale per il kernel
  Xiaomi/MediaTek: si leggono sequenze e valori, non si copia codice.

## Rischi aperti
| Rischio | Effetto | Mitigazione |
|---|---|---|
| `NSBundle` rifiuta un bundle fuori dalla shared cache, o un framework richiede `IOGPUMetalDevice`/IOGPU (QuartzCore usa `IOGPUDeviceCreate`) | il bundle non viene caricato, o `backboardd` non usa il device | prova in G0; user client che risponde ai selettori IOGPU usati da QuartzCore, o IOGPU minimo nel kernel |
| Sequenza di accensione MFG sbagliata (tensioni) | GPU instabile o danni | OPP fissa e conservativa presa da `viva-r-oss`, vincoli `vsram`/`vgpu`, elenco bianco dei registri PMIC, letture di verifica prima di ogni scrittura |
| GPU non coerente con la CPU | dati vecchi tra CPU e GPU | mappature write-combined; manutenzione esplicita delle cache dove serve |
| Pagine da 16 KiB in Mesa | allocazioni sbagliate | patch in G3, test sul Mac con pagine da 16 KiB (Apple Silicon: già 16 KiB) |
| CoreAnimation usa funzioni AGX non pubbliche | `backboardd` non parte sul nostro device | G0 domanda 4; il ripiego software resta con la policy |
| Blocchi della GPU in `backboardd` | interfaccia ferma | timeout + reset in G2, policy, `vivagpu=0` |
| Memoria: 6 GiB, ma XNU usa ~3,9 GiB e la GPU prende dalla stessa RAM | pressione di memoria | contabilità dei BO per processo, MADVISE, limiti per processo |
| ABI privata di Metal di iOS 16 diversa da Tahoe | frontend da adattare | introspezione sul telefono in G0, catch-all come rete di sicurezza |

## Fatti verificati
### 2026-11-03: come iOS 16.7.16 carica il driver Metal (offline, root `ios16-root.img` montata in sola lettura)
Strumenti: `ipsw` 3.1.741 (`dyld extract`, `class-dump`, `extract --kernel`), `dyld_info`, `otool -tV`.
- **Ricerca del device** (`_MTLRegisterDevices`, disassemblato): `IOServiceMatching("IOAcceleratorES")` →
  per ogni servizio legge `MetalPluginName` (CFString), carica `/System/Library/Extensions/<nome>.bundle`, prende
  `[bundle classNamed:MetalPluginClassName]` (oppure `principalClass` se la proprietà manca), richiede
  `isSubclassOfClass:_MTLDevice`, poi `[[cls alloc] initWithAcceleratorPort:service]` e `_MTLAddDevice`.
- **Kernel di Apple**: kext `AGXG10P`, personalità con `IOProviderClass=AppleARMIODevice`, `IONameMatch=gpu,t8015`,
  `IOMatchCategory=IOAcceleratorES`, `MetalPluginName=AGXMetalA11`; il kext aggiunge a runtime
  `MetalPluginClassName=AGXA11Device` e `MetalStatisticsName=AGXMetalStatisticsExternalA11`. Le classi
  `IOAcceleratorES`, `IOGPU`, `IOGPUDevice`, `IOGPUDeviceUserClient` stanno in `IOGPUFamily` (closed source;
  l'ereditarietà esatta tra `IOGPU` e `IOAcceleratorES` non è stata ricostruita).
- **Userspace di Apple**: `AGXMetalA11.bundle` su disco ha solo `Info.plist` (nessuna chiave Metal) e metallib di
  runtime; il codice è nella shared cache. Catena: `AGXA11Device : AGXA11FamilyDevice : IOGPUMetalDevice : _MTLDevice`.
  Code, buffer, texture, heap, encoder derivano da `IOGPUMetal*`, che derivano dalle classi `_MTL*` (le stesse basi
  usate da metal-shim). Metal ha anche la vecchia famiglia `MTLIOAccel*`, non usata dall'AGX.
- **Compilatore**: plugin `AGXCompilerCore.framework` (esporta `MTLCompilerCreate`, `…BuildRequest`,
  `…BuildRequestWithOptions`, `…BuildRequestWithSerializedBitcode`, `…ReleaseReply`, `…Delete`, `AIRNT*`); il
  percorso è scritto nel driver e passato a `MTLCompilerService` come `pluginPath`. `MTLCompiler.framework` ha
  probabilmente una lista di percorsi ammessi (`/System/Library/Extensions/`, `/System/Library/PrivateFrameworks/`;
  non dimostrato).
- **libLLVM**: `/usr/lib/libLLVM.dylib` nella cache, 958 simboli `LLVM*` dell'API C.
- **QuartzCore**: `CA::OGL::MetalContext`, `GLESContext`, `SWContext` (renderer software, quello di oggi);
  importa `MTLCreateSystemDefaultDevice`, soft-link a `IOGPUDeviceCreate`/`IOGPUDeviceSetDisplayParams`, cita
  `IOAcceleratorES`. Variabili d'ambiente: `CA_NO_ACCEL`, `CA_ENABLE_OGL`, `CA_FORCE_GENERIC_SHADERS` (utile se gli
  shader specifici per AGX danno problemi), `CADynamicBackendSwitching`.
- **IOSurface nell'AGX**: `newTextureWithDescriptor:iosurface:plane:`, `newBufferWithIOSurface:`; usa
  `IOSurfaceGetBaseAddressOfPlane`, `GetBytesPerRowOfPlane`, `IsTiled`, `AlignProperty`, `CopyValue`, `Lock`.
  Serve quindi `IOSurfaceCopyValue` funzionante in `SweetSurface` (oggi restituisce sempre NULL, TODO della 0046).
