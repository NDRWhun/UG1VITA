<a id="readme-top"></a>

<div align="center">

<img src="assets/logo.png" alt="NFSUG1VITA" width="128" height="128">

<h3 align="center">NFSUG1VITA</h3>

<p align="center">
  <b>Need for Speed Underground, on the PS Vita</b>
  <br />
  <br />
  A fan port of the PC version of Need for Speed Underground (2003): native widescreen, Vita controls, 30 fps.
  <br />
  <br />
  <a href="#install">Install</a>
  &nbsp;·&nbsp;
  <a href="#controls">Controls</a>
  &nbsp;·&nbsp;
  <a href="#settings">Settings</a>
  &nbsp;·&nbsp;
  <a href="../../issues">Report Bug</a>
</p>

[![build][build-shield]][build-url]&nbsp;[![issues][issues-shield]][issues-url]&nbsp;[![last commit][commit-shield]][commit-url]

[![platform][platform-shield]][platform-url]&nbsp;[![resolution][resolution-shield]][resolution-url]&nbsp;[![renderer][renderer-shield]][renderer-url]&nbsp;[![license][license-shield]][license-url]

</div>

> **Version 0.9, playtesting.** The game is being tested before the final release. It's worth the wait!

## Requirements

- Need for Speed Underground for PC, **version 1.4** (your own copy; other versions are refused)
- The game's `Speed.exe` must have the **no-DVD patch** applied
- For now only one `Speed.exe` works. Check yours on Windows with `certutil -hashfile Speed.exe SHA256`; it must
  show `c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7`
- PS Vita with HENkaku / Ensō and [VitaShell](https://github.com/TheOfficialFloW/VitaShell)
- About 1.5 GB free on the memory card
- Overclocking is mandatory, for example with [PSVshell](https://github.com/Electry/PSVshell)
- [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) is highly recommended

## Install

1. Install `NFSUG1VITA.vpk` with VitaShell.
2. Create the folder `ux0:data/NFSUG1VITA/` in VitaShell (or start the game once; it creates the folder and closes).
3. On your PC, open the game folder (the one with `Speed.exe`) and copy **everything inside it** to
   `ux0:data/NFSUG1VITA/` (VitaShell's FTP or USB mode works). `Speed.exe` must end up at
   `ux0:data/NFSUG1VITA/Speed.exe`.
4. Start the game from the home screen.

If the game closes right away, `ux0:data/NFSUG1VITA/nfsug1vita.log` says what is missing.

## Controls

### Menus

| Button | Action |
|--------|--------|
| <img src="assets/icons/dpad-up.svg" width="20" alt="D-pad"> | Move |
| <img src="assets/icons/cross.svg" width="20" alt="Cross"> | Select |
| <img src="assets/icons/circle.svg" width="20" alt="Circle"> | Back |
| <img src="assets/icons/triangle.svg" width="20" alt="Triangle"> | Extra option on the screen (Customize, Stock, …) |
| <img src="assets/icons/start.svg" width="20" alt="Start"> | Start, skip movie |

### Driving

| Button | Action |
|--------|--------|
| <img src="assets/icons/r.svg" width="20" alt="R"> | Accelerate |
| <img src="assets/icons/l.svg" width="20" alt="L"> | Brake / reverse |
| <img src="assets/icons/lstick.svg" width="20" alt="Left stick"> <img src="assets/icons/dpad-left.svg" width="20" alt="D-pad left"> <img src="assets/icons/dpad-right.svg" width="20" alt="D-pad right"> | Steer |
| <img src="assets/icons/cross.svg" width="20" alt="Cross"> | Nitrous |
| <img src="assets/icons/square.svg" width="20" alt="Square"> | Handbrake |
| <img src="assets/icons/circle.svg" width="20" alt="Circle"> | Look back |
| <img src="assets/icons/triangle.svg" width="20" alt="Triangle"> | Change camera |
| <img src="assets/icons/dpad-up.svg" width="20" alt="D-pad up"> / <img src="assets/icons/dpad-down.svg" width="20" alt="D-pad down"> | Shift up / down |
| Rear touch, right / left | Shift up / down |
| <img src="assets/icons/select.svg" width="20" alt="Select"> | Reset car to the track |
| <img src="assets/icons/start.svg" width="20" alt="Start"> | Pause |

Text fields open the Vita keyboard (or <img src="assets/icons/select.svg" width="20" alt="Select"> +
<img src="assets/icons/triangle.svg" width="20" alt="Triangle">). To quit, use the PS button.

## Settings

Settings are optional. Create a text file `ux0:data/NFSUG1VITA/nfsug1vita.cfg` (not `nfsug1vita.cfg.txt`) with one
setting per line, for example:

```
menu_widescreen=off
```

| Setting | Values | What it does |
|---------|--------|--------------|
| `widescreen` | `on` (default), `off` | Full 16:9 picture, or the original 4:3 one |
| `fit_4_3` | `stretch` (default), `pillarbox` | With `widescreen=off`: stretch the 4:3 picture, or keep it with black bars |
| `menu_widescreen` | `on` (default), `off` | Menus laid out for the wide screen, or centred at 4:3 |
| `hud_widescreen` | `on` (default), `off` | Race HUD at the screen edges, or centred at 4:3 |
| `button_prompts` | `vita` (default), `pc` | Vita buttons on screen, or the original PC keys |
| `rear_touch` | `on` (default), `off` | Gear shifting on the rear touch pad |
| `car_reflection_interval` | `1` (default) to `6` | Higher values update the car reflections less often, for more speed |

## Known issues

- A few frames drop below 30 fps in busy moments of a race.
- The 3D garage behind some menus can look washed out.
- No online or LAN play.

## Build

Requirements: [Python 3](https://www.python.org/downloads/) and PowerShell (part of Windows). Everything else is
downloaded on the first build: [VitaSDK](https://vitasdk.org) and [LLVM](https://llvm.org) into `.deps/`, CMake and
Ninja with pip. To use your own VitaSDK or LLVM, set `VITASDK` or `NFSU_LLVM` (or put them in `tools/env.local.ps1`).

```powershell
tools/build.ps1 -Game "<folder with Speed.exe>"   # -> build-release-clang/NFSUG1VITA.vpk
```

- The recompiled code is generated from your own `Speed.exe`; no game code or data is in this repository.
- Shaders ship prebuilt in `shaders/gxp/`.
- The build options are described at the top of `tools/build.ps1`.

| Path | Contents |
|------|----------|
| `include/` | virtual x86 CPU, runtime, Win32 and platform headers |
| `src/lib/` | runtime: guest memory, threads, Win32, C runtime, Direct3D 9 and D3DX |
| `src/platform/` | Vita backend: GXM renderer, audio, input, sync, logging |
| `src/nfsu/` | entry point, native replacements of game functions |
| `shaders/` | Cg shaders and compiled binaries |
| `tools/` | recompiler and build scripts |

## Credits

- [Grarak](https://github.com/Grarak): [psp2-tooling](https://github.com/Grarak/psp2-tooling) and
  [mh3-wii](https://github.com/Grarak/mh3-wii)
- [GrapheneCt](https://github.com/GrapheneCt): CapUnlocker
- EA Black Box: *Need for Speed Underground*

## License

MIT for the port's own code ([LICENSE](LICENSE)); third-party parts: [THIRD_PARTY.md](THIRD_PARTY.md).

Unofficial, non-commercial fan port, not affiliated with or endorsed by Electronic Arts. *Need for Speed* is a
trademark of Electronic Arts. You must own the game.

<!-- BADGES -->

[build-shield]: https://img.shields.io/github/actions/workflow/status/NDRWhun/UG1VITA/build.yml?branch=main&label=build&style=for-the-badge&labelColor=1a1f3d&logo=githubactions&logoColor=white
[build-url]: ../../actions/workflows/build.yml
[issues-shield]: https://img.shields.io/github/issues/NDRWhun/UG1VITA?label=issues&style=for-the-badge&labelColor=1a1f3d&color=2f7bff&logo=github&logoColor=white
[issues-url]: ../../issues
[commit-shield]: https://img.shields.io/github/last-commit/NDRWhun/UG1VITA?label=updated&style=for-the-badge&labelColor=1a1f3d&color=ff3b5c
[commit-url]: ../../commits/main
[platform-shield]: https://img.shields.io/badge/PS%20Vita-native-2f7bff?style=for-the-badge&labelColor=1a1f3d&logo=playstation&logoColor=white
[platform-url]: https://vitasdk.org
[resolution-shield]: https://img.shields.io/badge/960x544-native-b44dff?style=for-the-badge&labelColor=1a1f3d
[resolution-url]: #settings
[renderer-shield]: https://img.shields.io/badge/renderer-sceGxm-ff3b5c?style=for-the-badge&labelColor=1a1f3d
[renderer-url]: #build
[license-shield]: https://img.shields.io/badge/license-MIT-ff3b5c?style=for-the-badge&labelColor=1a1f3d
[license-url]: LICENSE
