<a id="readme-top"></a>

<div align="center">

<img src="assets/logo.png" alt="NFSUG1VITA" width="128" height="128">

<h3 align="center">NFSUG1VITA</h3>

<p align="center">
  <b>Need for Speed Underground, on the PS Vita</b>
  <br />
  <br />
  A static recompilation of Need for Speed Underground (PC, 2003) to the PS Vita, with a native sceGxm backend.
  <br />
  <br />
  <a href="#setup-for-players">Setup</a>
  &nbsp;·&nbsp;
  <a href="#controls">Controls</a>
  &nbsp;·&nbsp;
  <a href="../../issues">Report Bug</a>
</p>

[![issues][issues-shield]][issues-url]&nbsp;[![last commit][commit-shield]][commit-url]

[![platform][platform-shield]][platform-url]&nbsp;[![resolution][resolution-shield]][resolution-url]&nbsp;[![renderer][renderer-shield]][renderer-url]&nbsp;[![license][license-shield]][license-url]

</div>

> **Status: in development.** The recompiler, the Win32 runtime and the Vita platform layer are in place; the game
> does not run yet (the Direct3D 9 renderer is not written) and there is no release.

<details>
  <summary>Table of Contents</summary>
  <ol>
    <li><a href="#setup-for-players">Setup (for players)</a></li>
    <li><a href="#controls">Controls</a></li>
    <li><a href="#settings">Settings</a></li>
    <li><a href="#known-issues">Known issues</a></li>
    <li><a href="#build-for-developers">Build (for developers)</a></li>
    <li><a href="#credits">Credits</a></li>
    <li><a href="#license">License</a></li>
  </ol>
</details>

## Setup (for players)

You need your own copy of Need for Speed Underground for PC, version 1.4: its `Speed.exe` must have the SHA-256
`c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7` (other versions are refused).

- The contents of the game folder (`Speed.exe` and the data folders) go directly to `ux0:data/NFSUG1VITA/`.
- Performance baseline: the ARM clock overclocked to 500 MHz with PSVshell.
- CapUnlocker is optional: with it, some guest threads move to core 3.

There is no VPK to install yet.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Controls

Not final yet.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Settings

Optional settings go in `ux0:data/NFSUG1VITA/nfsug1vita.cfg`, one `key=value` per line. The list of settings is not
final yet. The log is written to `ux0:data/NFSUG1VITA/nfsug1vita.log`.

Screen: `widescreen=1` (default) renders at the display's 960x544 with the 4:3 vertical view and a wider horizontal
one, and centres the 2D screens at 4:3; `widescreen=0` keeps the game's resolution option (800x600 by default)
scaled to the display. `hud_anchor=1` (default) puts the race HUD's elements and the track map against the screen
edge on their side at the same scale and stretches full-screen 2D (fades, backgrounds) to the full width during
races and track loading. `hud_anchor=0` keeps the whole HUD centred at 4:3. `menu_widescreen=on` (default) lays the
menus out for the screen width in every part of the game (front end, pause menu, results, dialogs): plain colour
fills and gradients that reach a screen edge continue to the edge of the display, legends, titles, arrows and side
panels next to a screen edge move to that edge, and menu blocks, text, pictures and movies keep their shape in the
middle; 2D placed off the 4:3 screen stays hidden. `menu_widescreen=off` keeps the menus centred at 4:3.

Race benchmark: `bench=1` skips the front end and the movies and starts a circuit race in which the AI drives your
car; a `bench: summary` line gives the fps and frame times from the start signal to the finish (the timing log stays
off unless `bench_perf=1` or `perf_log=1`). Off by default; the other keys need `bench=1`:

| Key | Default | Meaning |
|-----|---------|---------|
| `bench_track` | 1001 | track number (1001-1008 are the circuits; 1001 is Market St.) |
| `bench_laps` | 2 | laps, 1-10 |
| `bench_opponents` | 3 | AI opponents, 0-3 |
| `bench_seconds` | 0 | ends the measurement after this many seconds (0: at the finish) |
| `bench_exit` | 0 | 1 quits the game after the summary |
| `bench_perf` | 0 | 1 turns the timing log (`perf:` lines) on during the benchmark |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Known issues

- The game does not run yet: there is no Direct3D 9 renderer.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Build (for developers)

Needs [VitaSDK](https://vitasdk.org) (with `VITASDK` set, in the environment or in `tools/env.local.ps1`), Python 3
and PowerShell (Windows PowerShell 5.1 or PowerShell 7). CMake and Ninja are installed with pip when they are missing;
the recompiler's Python package (capstone) is installed automatically.

The generated recompilation (`src/nfsu/generated/`) and any game binary or data are not tracked: the build generates
the code from your own `Speed.exe`. Only the version listed in `tools/recompiler/generate.py` is accepted (its SHA-256
is checked).

```powershell
tools/build.ps1 -Game <folder with Speed.exe>   # -> build-release/NFSUG1VITA.vpk; rebuilds only what changed
```

The game folder can also come from `NFSU_GAME_DIR`; it is remembered for the next run. Without game files,
`-DNFSU_RUNTIME_CHECK=ON` compiles the runtime alone. The shaders ship prebuilt in `shaders/gxp/`; the official PSP2
SDK (`host_tools/bin/psp2cgc`, `SCE_PSP2_SDK_DIR`) is only needed to change them.

### clang build (optional)

VitaSDK's gcc is the default. With [LLVM](https://github.com/llvm/llvm-project/releases) (clang and lld; `NFSU_LLVM`
is its root) the game also builds with clang, ThinLTO and profile-guided optimisation; VitaSDK still provides the
libraries and the packaging tools.

```powershell
tools/build.ps1 -Compiler clang                 # -> build-release-clang/NFSUG1VITA.vpk (ThinLTO; -NoLto without)
```

Profile-guided optimisation, with a profile from your own runs:

1. `tools/build.ps1 -Compiler clang -PgoGenerate` builds the instrumented game (`build-release-pgogen/`; slower, the
   profile runtime is fetched for the LLVM version unless `NFSU_COMPILER_RT` names a compiler-rt source tree).
2. Install it and play the scenes that matter (menus, races on several tracks, replays). The game writes its
   counters to `ux0:data/NFSUG1VITA/nfsu.profraw` every 30 seconds and at exit; the file holds the whole session.
   Copy it off after each session (FTP, USB) under its own name, e.g. `run1.profraw`, and start the next session.
3. Merge the runs (each weighs the same) and build with the profile:
   `& "$env:NFSU_LLVM/bin/llvm-profdata" merge -o nfsu.profdata run*.profraw`, then
   `tools/build.ps1 -Compiler clang -PgoUse nfsu.profdata`.

### Profile-guided optimisation with gcc

1. `tools/build.ps1 -PgoGenerate` builds the instrumented game (`build-release-gcov/`; edge counters only, slower).
2. Install it and play the scenes that matter. The game writes its counters to `ux0:data/NFSUG1VITA/nfsu.gcov` every
   30 seconds and at exit; the file holds the whole session. Copy it off after each session under its own name, e.g.
   `run1.gcov`.
3. `tools/build.ps1 -PgoUse run1.gcov,run2.gcov` merges the runs with `gcov-tool merge-stream` into
   `build-release-pgo/` and builds the optimised game there. The profile matches only the sources and options of
   the instrumented build; functions changed since then are built without it.

| Path | Contents |
|------|----------|
| `include/` | virtual x86 CPU (cpu/fpu/mmx), `lib/` runtime headers, `winapi/` shim headers, `platform/` |
| `src/lib/` | runtime: guest memory, threads, Win32 shims, DirectDraw presentation |
| `src/platform/` | Vita backend: gxm device and render thread, audio, input, sync, logging |
| `src/nfsu/` | entry point (+ generated `generated/`) |
| `shaders/` | Cg programs; `gxp/` holds their compiled binaries |
| `tools/` | recompiler, generate, build and environment scripts |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Credits

- [Grarak](https://github.com/Grarak) — [psp2-tooling](https://github.com/Grarak/psp2-tooling), for profiling on
  hardware, and [mh3-wii](https://github.com/Grarak/mh3-wii), a statically recompiled game running at full speed that
  set the performance goal for this port.
- [GrapheneCt](https://github.com/GrapheneCt) — CapUnlocker, which opens the fourth CPU core.
- EA Black Box — the original *Need for Speed Underground*.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## License

The port's own code is MIT (see [LICENSE](LICENSE)); third-party parts keep their own licenses, see
[THIRD_PARTY.md](THIRD_PARTY.md). The game is not part of this repository.

Unofficial, non-commercial fan port — not affiliated with or endorsed by Electronic Arts. *Need for Speed* is a
trademark of Electronic Arts; you must own a legal copy to play.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- BADGES -->

[issues-shield]: https://img.shields.io/github/issues/NDRWhun/NFSUG1VITA?label=issues&style=for-the-badge&labelColor=1a1f3d&color=2f7bff&logo=github&logoColor=white
[issues-url]: ../../issues
[commit-shield]: https://img.shields.io/github/last-commit/NDRWhun/NFSUG1VITA?label=updated&style=for-the-badge&labelColor=1a1f3d&color=ff3b5c
[commit-url]: ../../commits/main
[platform-shield]: https://img.shields.io/badge/PS%20Vita-native-2f7bff?style=for-the-badge&labelColor=1a1f3d&logo=playstation&logoColor=white
[platform-url]: https://vitasdk.org
[resolution-shield]: https://img.shields.io/badge/960x544-widescreen%20target-b44dff?style=for-the-badge&labelColor=1a1f3d
[resolution-url]: #known-issues
[renderer-shield]: https://img.shields.io/badge/renderer-sceGxm-ff3b5c?style=for-the-badge&labelColor=1a1f3d
[renderer-url]: src/platform/gxm
[license-shield]: https://img.shields.io/badge/license-MIT-ff3b5c?style=for-the-badge&labelColor=1a1f3d
[license-url]: LICENSE
