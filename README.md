# PGR4:ReGPU

A native PC port of **Project Gotham Racing 4** (Bizarre Creations, 2007, Xbox 360). It is not an
emulator: nothing interprets the console's code or runs it in a virtual machine.

- **The game code is native x64.** [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) translates
  the game's PowerPC code ahead of time into C++, which is compiled into `pgr4.exe`. The console's
  kernel and system calls are reimplemented as ordinary Windows code.
- **The graphics are native Direct3D 12.** **ReGPU**, the renderer in `thirdparty/ReGPU`, turns the
  game's GPU commands into Direct3D 12 draws, and its shaders (Xenos microcode) into HLSL compiled for
  your GPU. Pipelines are cached, so a shader compiles once.

 No game files are included: you need your own copy of the disc
(and, optionally, Title Update #2 and DLC).

## ReGPU

ReGPU gives the port a PC renderer instead of a GPU emulator. The game draws exactly as it did on the
console, writing command packets and calling the `Vd*` video functions, and ReGPU builds Direct3D 12
work from them:

- **Shaders:** each Xenos shader is translated once to HLSL and compiled to a D3D12 pipeline, cached on
  disk between runs.
- **Render targets:** the console's 10 MB EDRAM tiles and resolves become D3D12 render targets and
  copies, with real MSAA.
- **Textures:** the console's tiled, big-endian formats are converted to D3D12 textures as the game
  uploads them.
- **PC features the console never had:** any resolution, FSR 1 upscaling and sharpening, anisotropic
  filtering, an uncapped or custom frame rate.

It started in the Forza Motorsport 2 port and was proven on this one; fixes made for PGR4 go back into
it.

## What's here

| Folder | What it is |
|---|---|
| `src/` | The port's runtime: the console kernel and XAM, memory, threads, file system, audio (XMA, WMA via XMP), saves and profiles, settings, and the game-specific hooks (`src/hooks/`) |
| `thirdparty/ReGPU/` | The renderer: command processor, Xenos shader microcode to HLSL, EDRAM, resolves, MSAA, texture formats, upscaling (FSR 1) and the debug server. Was a submodule of `zer0ducksgiven/ReGPU`; it now lives in this repository |
| `config/pgr4.toml` | XenonRecomp's function boundaries and jump tables for `default.xex` with Title Update #2 |
| `tools/` | Disc and package extraction (`xiso.py`, `stfs.py`), recompile (`recomp.sh`), import and stub generators |
| `launcher/PGR4Launcher/` | The launcher (C#, Photino): install, graphics settings, DLC and mods, profiles and 360 save import, title update import, photos, updates |
| `installer/` | The Inno Setup installer and first-boot setup |

## Building

Windows 10/11, a Direct3D 12 GPU, clang-cl, CMake 3.25+, Ninja, Python 3, and the .NET SDK for the
launcher.

```sh
py -3 tools/xiso.py extract "<PGR4 disc>.iso" game
py -3 tools/stfs.py update "<TU_16L61VP_...>" out/tu2
sh tools/recomp.sh                 # XenonRecomp: game/default.xex + TU2 -> out/ppc
cmake --build build/rel            # pgr4.exe
dotnet build launcher/PGR4Launcher -c Release
```

`play.cmd` starts the game from the repository root (logs to `out/play.log`).

## DLC

360 DLC packages install through the launcher's **Add DLC**. The Kellogg's Typhon promo, whose 360
package is lost, installs from its zip of rebuilt files (`PGR4-Kelloggs-Typhon-DLC.zip`) through the
same button.

## Contributors

- **Scott** ([zer0ducksgiven](https://github.com/zer0ducksgiven)): the port and ReGPU.

ReGPU contributors:

- **[Aiden-Piercee](https://github.com/Aiden-Piercee)**: ring buffer replacement without racing the
  command processor, depth-only alpha test and clear fixes for fixed-point targets, pixel-shader kills
  in depth-only passes, regpuctl whole-frame capture and draw counts.
- **[tankyx](https://github.com/tankyx)**: explicit-LOD sampling in vertex shaders; skipping microcode
  that cannot be a shader.
- ReGPU also carries fixes from the Forza Motorsport 2, Forza Motorsport 4 and Glue2GPU ports.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT).
- [Xenia](https://github.com/xenia-project/xenia)'s research into the Xenos GPU and the Xbox 360
  kernel informed this work. No Xenia code is included.
- AMD FidelityFX SDK (MIT); Microsoft's `d3dcompiler_47.dll` (Windows SDK redistributable);
  [Photino.NET](https://github.com/tryphotino/photino.NET) for the launcher.

Project Gotham Racing 4 is © Microsoft. This project contains none of its code or data.
