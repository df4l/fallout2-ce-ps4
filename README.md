# Fallout 2 CE for PS4

A homebrew port of [Fallout 2 Community Edition](https://github.com/fallout2-ce/fallout2-ce) to the PlayStation 4, built with the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain). The output is a `.pkg` for a jailbroken console running GoldHEN.

**No game data is included.** You need your own copy of Fallout 2 (GOG or Steam, Windows version).

## Status

Tested on real hardware: intro movies, main menu, new game, character creation (on-screen keyboard), gameplay, save and load, music, clean exit to the home screen.

## Repository layout

```
.
├── build.sh                 Build entry point (wraps make with the right environment)
├── Makefile                 PS4 build: compiles fallout2-ce (+ its vendored fpattern, lodepng, stb_vorbis) + zlib + glue, packages the .pkg
├── sources.txt              fallout2-ce source list (from its CMakeLists.txt)
├── src/ps4_platform.{h,cc}  PS4 glue: filesystem, gamepad-to-mouse, IME keyboard, clock, logging
├── patches/
│   └── fallout2-ce-ps4.patch   Engine changes (`#ifdef __PS4__` blocks), applied by setup.sh
├── scripts/
│   ├── setup.sh             Submodules, patch, toolchain binaries, OpenSSL 1.1
│   ├── stage_assets.sh      Copies your game files into gamedata/
│   └── fix_gp4.py           Fixes the <rootdir> section emitted by create-gp4
└── external/                Git submodules
    ├── fallout2-ce          fallout2-ce/fallout2-ce @ 85134d3 (+ patch)
    ├── PS4Toolchain         OpenOrbis/OpenOrbis-PS4-Toolchain @ v0.5.4 (+ release binaries)
    └── zlib                 madler/zlib @ v1.3.1
```

## Host requirements

- Linux x86_64 (tested on Linux Mint 22.3, Ubuntu 24.04 base).
- `git`, `curl`, `make`, `python3`, `clang`/`clang++` and `ld.lld` (tested with LLVM 18, matching the toolchain release).
- About 2 GB of free disk space (toolchain, objects, staged data and the ~590 MB package).

### Why OpenSSL 1.1?

The toolchain's `PkgTool.Core` (used to build `param.sfo` and the `.pkg`) is a .NET program that needs OpenSSL **1.1**, which recent distributions no longer ship. `setup.sh` checks the host and, if `libssl.so.1.1` is missing, downloads Ubuntu's `libssl1.1_1.1.1f-1ubuntu2_amd64.deb` (checksum verified) and extracts it into `deps/openssl11/`. `build.sh` then adds it to `LD_LIBRARY_PATH` for the build only. Nothing is installed system-wide and the toolchain submodule is not modified for this.

If you already extracted that package by hand into the toolchain as `compat/openssl11/` (`external/PS4Toolchain/compat/openssl11/usr/lib/x86_64-linux-gnu/libssl.so.1.1`), `build.sh` picks it up as well.

## Setup

```bash
git clone --recursive https://github.com/df4l/fallout2-ce-ps4
cd fallout2-ce-ps4
./scripts/setup.sh
```

`setup.sh` is idempotent. It:

1. initializes the submodules;
2. applies `patches/fallout2-ce-ps4.patch` to `external/fallout2-ce`;
3. downloads the OpenOrbis v0.5.4 release (`toolchain-llvm-18.tar.gz`, checksum verified) and extracts it over `external/PS4Toolchain`. The git repository only holds the toolchain sources; headers, libraries (`libSDL2.a`, libc, libc++ ...) and tools (`create-fself`, `PkgTool.Core` ...) only come with the release;
4. fetches OpenSSL 1.1 if needed (see above).

Both modified submodules are marked `ignore = dirty` in `.gitmodules`, so `git status` stays clean.

## Game data

Point the staging script at your Windows Fallout 2 install directory:

```bash
./scripts/stage_assets.sh "/path/to/Fallout 2"
```

It needs `master.dat`, `critter.dat`, `patch000.dat` and `fallout2.cfg` (any case), and also takes `data/` (without saves), the music (`sound/music/*.acm`) and, if present, the High Resolution Patch files `f2_res.ini` / `f2_res.dat`. Files are hardlinked into `gamedata/` when possible (the packager mis-sizes symlinks). `gamedata/` is ignored by git: **never commit or redistribute it.**

## Build

```bash
./build.sh            # compile only
./build.sh eboot.bin  # compile + link + fself
./build.sh all        # full package: IV0000-FALL00002_00-FALLOUT2CE000000.pkg
./build.sh clean
```

Always go through `build.sh`: it sets `OO_PS4_TOOLCHAIN`, the OpenSSL path and `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` that a bare `make` would miss. The package icon is `icon0.png` at the repository root (512x512 PNG); use another one with `ICON=/path/to/icon.png ./build.sh all`.

## Install and run

1. Copy the `.pkg` to the console (USB or GoldHEN's FTP server, for example into `/data/pkg/`).
2. Install it with GoldHEN (Debug Settings → Package Installer) and launch **Fallout 2**.

On the console, the game data is read from the package (`/app0/gamedata`, read-only). On first launch the game creates `/data/fallout2/`, its writable working directory: `fallout2.cfg` (paths rewritten to `/app0/gamedata`), saves (`SAVEGAME/`), automap data and a log, `ps4.log`. Delete `/data/fallout2` to reset the configuration (this also deletes your saves).

## Controls

| Input | Action |
|---|---|
| Left stick | Move cursor (R3 toggles slow mode) |
| Touchpad (swipe) | Move cursor precisely; tap = left click |
| Cross / Circle | Left click / right click |
| Options | Esc |
| Square | Enter |
| Triangle | Tab (automap) |
| L1 / R1 | Inventory (I) / Character (C) |
| L2 / R2 | Pip-Boy (P) / End turn (Space) |
| Touchpad click | Quick save (F6); click again to close (Esc) |
| L3 | Quick load (F7) |
| D-pad | Arrow keys |

Text fields (character name, save description) open the PS4 on-screen keyboard.

## Debugging

The game writes `/data/fallout2/ps4.log` (startup steps, engine debug messages, audio, crash handler, watchdog). Each run starts with `---- start ----`. To resolve a `CRASH:` address, use `obj/fallout2.elf` from the same build with `nm` / `llvm-symbolizer`.

## Working on the engine patch

Edit the files in `external/fallout2-ce/src` directly, then regenerate the patch:

```bash
git -C external/fallout2-ce diff HEAD > patches/fallout2-ce-ps4.patch
```

Engine changes stay inside `#ifdef __PS4__` blocks; platform code belongs in `src/`. To move to a newer fallout2-ce, update the submodule commit, re-apply the patch and fix any conflicts.

## Licenses

- **fallout2-ce** is under the [Sustainable Use License](https://github.com/fallout2-ce/fallout2-ce/blob/main/LICENSE.md). The patch in `patches/` modifies it and is distributed under the same terms.
- **OpenOrbis PS4 Toolchain** is GPL-3.0.
- **zlib** (zlib license) is used unmodified; fpattern, lodepng and stb_vorbis come vendored in fallout2-ce under their own licenses.
- Fallout 2 is © Bethesda Softworks / Interplay. This project contains no game assets and is not affiliated with them.
