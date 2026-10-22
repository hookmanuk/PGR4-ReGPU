# PGR4 on ReGPU: scope

Project Gotham Racing 4 (Bizarre Creations, 2007) as a native PC port, built the same way as the
Forza Motorsport 2 port (`D:\fm2-port`): XenonRecomp for the code, FM2's runtime for the console's
kernel, and **ReGPU** (`thirdparty/ReGPU`, a submodule of `zer0ducksgiven/ReGPU`: the framework extracted from FM2's renderer) for graphics.
The second purpose is to prove ReGPU on a game it wasn't built from: every renderer problem found here
is either a PGR4 quirk (fixed in this port, through ReGPU's hooks) or a ReGPU gap (fixed in ReGPU).

## What's here

| Item | What it is |
|---|---|
| `Project Gotham Racing 4 (World) (...).iso` | Retail disc (XGD2, 7.8 GB image, 6.2 GB of files, 1,033 files). Title ID `4D5307F9`. |
| `TU_16L61VP_0000004000000.00000000000G1` | Title Update #2 (LIVE package): `default.xexp`, `gw.xexp`, `Pgr4TitleUpdate.bin` |
| `4D5307F9/00000002/638152C7...` | DLC "Vehicle Pack 1" (LIVE, content type 2, the same kind of package as FM2's DLC) |

## What the disc contains

- **`default.xex`**: the game. Loads at `0x82000000`, image 13.4 MB, **7.3 MB of PowerPC code**
  (`.text` `82270000`), with the Bink video decoder compiled in (`BINK` section).
- **`gw.xex`**: *Geometry Wars: Waves*, the arcade cabinet in the garage. A separate program (1.1 MB
  of code) that also loads at `0x82000000`; the title update patches it too.
- **Data**: `Game/` (771 files: `.dat`, `.pak`, `.pak_hrd`, `.bpr`, `.baf` sound banks, `.gm3`,
  `.stream2`), `Camera/`, `UI/`, `loc/`, `Movies/` (13 Bink videos, 180 MB intro), 115 `.wma` music
  tracks.

## The kernel: what FM2's runtime already covers

`default.xex` imports **293** functions and variables. FM2's runtime implements **236** of them.
The **57 new ones**:

| Group | Imports | Work |
|---|---|---|
| Networking | `NetDll_*` (accept, listen, recvfrom, sendto, WSA events…), `XNetDnsLookup/Release`, `XNetGetDebugXnAddr`, `XNetXnAddrToMachineId` (20) | Offline stubs: "no network" (offline only, decided) |
| Kernel I/O and objects | `IoCreateDevice`, `IoDeleteDevice`, `IoCompleteRequest`, `IoCheck/Set/RemoveShareAccess`, `IoDismountVolume`, `IoInvalidDeviceRequest`, `NtReadFileScatter`, `ObReferenceObject`, `ObIsTitleObject` | Small; `NtReadFileScatter` needs a real implementation (the counterpart of `NtWriteFileGather`) |
| Kernel misc | `ExAllocatePoolTypeWithTag`, `KeEnableFpuExceptions`, `KeSave/RestoreFloatingPointState`, `KeSetDisableBoostThread`, `KeTryToAcquireSpinLockAtRaisedIrql`, `MmQueryStatistics`, `RtlCompareStringN`, `RtlUpcaseUnicodeChar`, `_snprintf`, `XexLoadImageHeaders` | Small |
| Kernel data exports | `ExLoadedCommandLine`, `ExThreadObjectType`, `KeCertMonitorData`, `KeDebugMonitorData`, `XboxHardwareInfo`, `XboxKrnlVersion`, `XexExecutableModuleHandle` | Values in the loader's variable table (zeroed storage is the fallback) |
| **GPU data exports** | **`VdGlobalDevice`, `VdGpuClockInMHz`, `VdHSIOCalibrationLock`** | **ReGPU change**: ReGPU provides the `Vd*` functions but no data exports yet. It needs a way to hand the loader their values (`VdGpuClockInMHz` = 500; the other two zeroed) |
| XAM | `XamShowGameInviteUI`, `XamShowPlayersUI`, `XNotifyPositionUI`, `XamUserGetSigninInfo`, `XamUserWriteProfileSettings`, `XamUserCreateAchievementEnumerator`, `XamWriteGamerTile` | Small; sign-in info and profile writes matter for saves |

Audio uses the same interface as FM2 (`XAudioRegisterRenderDriverClient`, `XAudioSubmitRenderDriverFrame`,
`XMACreateContext`), so FM2's audio carries over. Bink and WMA decoding happen in the game's own
recompiled code.

## The plan, following FM2

### Phase 1: project and code (no graphics yet)
1. The FM2 layout: `src/`, `tools/`, `thirdparty/` (XenonRecomp; ReGPU as a submodule), `out/` for generated
   code (never committed).
2. Extract the disc (`tools/xiso.py`), apply Title Update #2 to `default.xex` (FM2's loader and
   `xexp` handling), XenonRecomp it with a PGR4 TOML: entry point, function boundaries,
   jump tables. This is the longest single step on FM2: expect several rounds of XenonRecomp fixes
   (missed functions, jump tables, `setjmp`/`longjmp`, unusual instructions).
3. Copy FM2's runtime (kernel, memory, threads, file system, XAM, audio, loader, settings, debug server,
   crash and hang logging) without FM2's hooks and launcher. Generate stubs for every import
   (`tools/genstubs.py`) so it links and logs each unimplemented call.

**Done when:** the game boots and runs its code to the first graphics call, logging its file reads.

### Phase 2: ReGPU bring-up
1. Link ReGPU, fill `regpu::Config` and `regpu::Host` from the runtime (memory base, allocator,
   guest threads and calls, interrupt lock). Add the three `Vd` data exports (above) to ReGPU.
2. Get to the first presented frame, then the front end.
3. Fix what breaks. Each fix is sorted: **console behaviour** goes into ReGPU (and back to FM2 when it
   applies there); **PGR4 quirks** go into this port through `rewriteShader`, `onDraw` or
   `resolveWriteback`.

**Done when:** menus and a race render correctly at 1280x720.

### Phase 3: the game
- Saves and profiles (sign-in info, profile settings, FM2's signed-save handling), the DLC
  (`stfs.py install` and FM2's DLC enumeration, same as FM2's), input, audio, music (WMA), videos.
- Then FM2's player features as wanted: launcher, resolution and widescreen, frame-rate cap, upscalers.

### Out of scope for now (decided 2026-10-22)
- **Geometry Wars** (`gw.xex`): not ported. The garage cabinet doesn't start. (It would need its own
  XenonRecomp run and the runtime swapping programs when the game launches it.)
- **Networking**: offline only. The `NetDll_*` and `XNet*` imports answer "no network".

## What this tells us about ReGPU

| Expected | Why |
|---|---|
| Data exports (`Vd*` variables) | Certain: PGR4 imports three |
| Render resolution | ReGPU assumes 1280x720 (`kWidth`/`kHeight`). **Check PGR4's real resolution first** (its EDRAM surface sizes in the first captures); if it isn't 720p this is the first ReGPU change |
| Different EDRAM use | Weather, HDR, motion blur and night scenes are PGR4 strengths: new tiling, MSAA and resolve patterns FM2 never used |
| Resolve write-backs | Photo mode and replays in PGR4 read images on the CPU differently from FM2: `resolveWriteback` gets its first real user |
| Shader count and compile time | Unknown until the first boot; the first-boot compile screen (`staticShaders`) may need PGR4's pack format |
| Lessons back to FM2 | Any console-behaviour fix found here applies to FM2 too |

## Decisions (2026-10-22)

1. **Geometry Wars**: out of scope for now.
2. **Networking**: offline only.
3. **ReGPU**: a git submodule of `zer0ducksgiven/ReGPU` at `thirdparty/ReGPU`. Every ReGPU improvement made
   here is committed and pushed to ReGPU's `main`, then this repo moves its pointer forward. PGR4-specific
   code stays in this repo (through ReGPU's hooks), never in ReGPU.
4. **Repo**: private GitHub repo. Game files (disc image, title update, DLC, anything extracted from them)
   are never committed.

## Rough effort

Phase 1 is the long one: FM2's equivalent took most of its early development. Reusing FM2's runtime
removes most of the kernel work, leaving the 57 new imports. Phase 2's length depends on how far PGR4
strays from what FM2 exercised; that is what this test is for.
