# Freegemas for AmigaOS (RTG)

A native AmigaOS 3.x port of [Freegemas](https://github.com/JoseTomasTocino/freegemas),
the open source match-three puzzle game, for RTG graphics cards. It runs in a window
on the Workbench or on its own 800x600 screen, with sound through AHI or Paula.

Two builds are made from the same sources:

| Program        | CPU                                      | Blending        |
|----------------|------------------------------------------|-----------------|
| `Freegemas080` | Apollo 68080 (Vampire), with FPU         | AMMX kernels    |
| `Freegemas060` | 68060 with FPU (also runs on the 68080)  | 68k kernels     |

## Requirements

- AmigaOS 3.x (developed on 3.2) with Picasso96 or CyberGraphX
- an RTG mode with 15, 16, 24 or 32 bits:
  - fullscreen: an 800x600 (or larger) mode
  - window: a Workbench screen of at least about 820x640
- 68080 or 68060 with FPU (68060: `68060.library` installed)
- about 32 MB of free Fast RAM (everything is loaded at start)
- for sound: AHI 4 or newer, or the Paula audio hardware

## Installation and start

Copy the `Freegemas` drawer anywhere and double-click `Freegemas080` (68080) or
`Freegemas060`. From the Shell:

    cd Freegemas
    Freegemas080 [FULLSCREEN|WINDOW] [AUDIO AHI|PAULA|OFF]

Tool types (icon) and Shell arguments:

| Tool type / argument     | Meaning                                                        |
|--------------------------|----------------------------------------------------------------|
| `FULLSCREEN=YES` / `NO`  | own screen or a window on the Workbench (Shell: `FULLSCREEN`, `WINDOW`) |
| `AUDIO=AHI`              | sound through ahi.device (default; falls back to Paula)        |
| `AUDIO=PAULA`            | sound through audio.device directly                            |
| `AUDIO=OFF`              | no sound                                                       |
| `AHIUNIT=n`              | AHI unit (0..3)                                                |
| `LOG=YES`                | write `freegemas.log` (off by default; Shell: `LOG`)           |

The fullscreen option in the game's Options menu and the `FULLSCREEN` tool type
are kept in sync: switching in the game updates the icon the game was started from.

Controls: mouse, or cursor keys with Space / Return, `H` for a hint, `Esc` back.
A joystick in port 2 works as well.

The game keeps its settings and high scores in `freegemas.prefs` in its drawer. With
`LOG=YES` it also writes a short log to `freegemas.log` there (results of the kernel
self-tests, screen format, audio output, errors); without it nothing is written.
The game has no serial debug output.


## License

Freegemas is free software by José Tomás Tocino and contributors, released under the
GNU General Public License version 2 or later. This port is released under the same
license (see `LICENSE`). The game media come from the Freegemas repository.
The programs link FreeType, zlib and the C/C++ runtime libraries of the toolchain
statically; their licenses apply to those parts.
