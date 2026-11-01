<div align="center">

<img src="assets/logo.png" alt="NFSUG1VITA" width="128" height="128">

<h3>NFSUG1VITA</h3>

<b>Need for Speed Underground on the PS Vita</b>

A fan port of the PC version of Need for Speed Underground (2003): native widescreen, Vita controls, 30 fps.

</div>

> **Work in progress.** The game is playable. Expect bugs; please report them on the
> [issues page](../../issues). There are no official downloads yet, so you build the game yourself (see
> [Building](#building)).

## What you need

- A PS Vita with custom firmware (HENkaku / Ensō) and [VitaShell](https://github.com/TheOfficialFloW/VitaShell).
- Optional: if you like, an overclocking plugin such as [PSVshell](https://github.com/Electry/PSVshell) can help the
  game run more smoothly. Whether you use one is entirely up to you.
- Optional: CapUnlocker, which lets the game use one more CPU core.
- Your own copy of **Need for Speed Underground for PC, version 1.4**. Other versions are refused.
- About 1.5 GB of free space on the memory card.

## Installing

1. Install `NFSUG1VITA.vpk` with VitaShell.
2. On your PC, open the folder where the game is installed (the one with `Speed.exe`).
3. Copy **everything inside that folder** to `ux0:data/NFSUG1VITA/` on the Vita (VitaShell's FTP or USB mode works).
   `Speed.exe` must end up directly at `ux0:data/NFSUG1VITA/Speed.exe`.
4. Start the game from the home screen.

If the game closes right away, check `ux0:data/NFSUG1VITA/nfsug1vita.log`: it says what is missing.

## Controls

| Driving | | Menus | |
|---|---|---|---|
| Left stick / D-pad left-right | Steer | D-pad | Move |
| R | Accelerate | ✕ | Select |
| L | Brake / reverse | ○ | Back |
| ✕ | Nitrous | △ | Extra option on the screen (Customize, Stock, …) |
| □ | Handbrake | START | Start / skip |
| ○ | Look back | | |
| △ | Change camera | | |
| D-pad up / down, or rear touch right / left | Shift up / down | | |
| SELECT | Reset car to the track | | |
| START | Pause | | |

To quit, use the PS button.

## Settings

Settings are optional. Create a text file `ux0:data/NFSUG1VITA/nfsug1vita.cfg` with one setting per line, for example:

```
menu_widescreen=off
```

| Setting | Values | What it does |
|---|---|---|
| `widescreen` | `on` (default), `off` | Full 16:9 picture, or the original 4:3 one |
| `menu_widescreen` | `on` (default), `off` | Menus laid out for the wide screen, or centred at 4:3 |
| `hud_widescreen` | `on` (default), `off` | Race HUD at the screen edges, or centred at 4:3 |
| `button_prompts` | `vita` (default), `pc` | Vita buttons on screen, or the original PC keys |
| `rear_touch` | `on` (default), `off` | Gear shifting on the rear touch pad |

## Known issues

- A few frames drop below 30 fps in busy moments of a race.
- The 3D garage behind some menus can look washed out.
- Online and LAN play are not available.

## Building

You need [VitaSDK](https://vitasdk.org), [LLVM](https://github.com/llvm/llvm-project/releases) (clang and lld),
Python 3 and PowerShell, plus your own game folder (the code is generated from your `Speed.exe`). Set `VITASDK` and
`NFSU_LLVM` to their folders (in the environment, or in `tools/env.local.ps1`), then run:

```powershell
tools/build.ps1 -Game "<your game folder>"
```

The VPK is written to `build-release-clang/NFSUG1VITA.vpk`. The options are described at the top of
`tools/build.ps1`.

## Credits

- [Grarak](https://github.com/Grarak) — [psp2-tooling](https://github.com/Grarak/psp2-tooling) for profiling on the
  Vita, and [mh3-wii](https://github.com/Grarak/mh3-wii), which set the performance goal for this port.
- [GrapheneCt](https://github.com/GrapheneCt) — CapUnlocker.
- EA Black Box — the original *Need for Speed Underground*.

## License

The port's own code is MIT (see [LICENSE](LICENSE)); third-party parts keep their own licenses
([THIRD_PARTY.md](THIRD_PARTY.md)). The game itself is not included.

Unofficial, non-commercial fan port, not affiliated with or endorsed by Electronic Arts. *Need for Speed* is a
trademark of Electronic Arts. You must own the game to play.
