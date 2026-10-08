# Recoil Remake

A rebuild of **Recoil** (Zipper Interactive, 1999), the hover-tank combat game, that runs on modern Windows.

This is not a game "inspired by" Recoil. Every function of the original `Recoil.exe` was reverse-engineered and
rebuilt, and the result is checked against the original by machine:

- **4,678 of 4,716 ported functions are proven byte-identical** to the original's code (`tools/prove_identity.py`).
- The rest are checked by running them side by side with the original on the same inputs (oracle tests).
- Every game constant in the source names where it came from (an address in the original, or a data file).

The original game's bugs are kept, because they are part of the game. They are listed in `ORIGINAL_DEFECTS.md`.

## You need your own copy of Recoil

This repository and its downloads contain **no part of the game**. No executable, data, maps, sounds, videos or
music are included. The remake reads all of them from your copy when you install it.

The supported version is the **1999-01-29 build**, as on the "Recoil Classic" CD. The 1998 release is not supported:
its program code and 35 of its data files differ.

## Two downloads

| Download | For | Contains |
|---|---|---|
| **RecoilRemake-Player** | playing | `Setup.exe` + the remake. No Python or developer tools needed. |
| **RecoilRemake-Source** | reading, changing and building | this repository: the source code, the tools and the research ledger |

### Playing

1. Unzip the player download and run `Setup.exe`.
2. **Step 1:** Setup opens the dgVoodoo2 download page. dgVoodoo2 is free and lets old DirectDraw games run on
   modern Windows; the remake was tested with versions 2.8.7.3 and 2.8.7.5. Download the zip and select it.
3. **Step 2:** select your copy of Recoil. This can be the installed game folder, a zip of it, the CD (or a copy of
   it), or a disc image (`.iso`, `.bin`/`.cue`, `.nrg`). Only a full disc image (`.nrg` or `.bin`+`.cue`) includes
   the CD music.
4. Pick an install folder and click **Install**, then start *Recoil Remake* from the desktop shortcut.

### Building it yourself

See [BUILDING.md](BUILDING.md). To change the code and check your change, see [CONTRIBUTING.md](CONTRIBUTING.md).

### Learning how it works

- [RECOIL_EXE_GUIDE.md](RECOIL_EXE_GUIDE.md): a tour of the original program. Covers its start-up, main loop, data
  formats and subsystems, and where to start reading.
- [HOW_IT_WAS_MADE.md](HOW_IT_WAS_MADE.md): how the original was taken apart and rebuilt, the tools and checks
  used, and how to repeat or extend the work.

## How the repository is organised

| Folder | What it is |
|---|---|
| `05_remake/src/` | The remake. The folders mirror the original's own source files (`GameZRecoil/`, `Battlesport/`). Functions whose original file is not known are in `unattributed/`. `platform/` is the layer between the original code and modern Windows. |
| `05_remake/boot/` | The launcher, including the no-CD mode and the virtual CD drive that plays the music. |
| `05_remake/setup/` | The player Setup. |
| `05_remake/tests/` | Tests. Many run the original function and the remake side by side. |
| `03_re/ledger/` | The research ledger: one row for each of the original's 4,061 functions, with status, evidence and the file it belongs to. |
| `04_spec/` | Written specifications of structures, formulas and systems. |
| `tools/` | The build, verification and porting tools. |
| `ORIGINAL_DEFECTS.md` | Bugs in the original game that the remake keeps on purpose. |

## Legal

The remake's own source code is free software: you can redistribute it and/or modify it under the terms of the
**GNU General Public License, version 3** or (at your option) any later version. See [LICENSE](LICENSE).

The licence covers this project's code only, not the game. Recoil is (c) 1999 Electronic Arts / Zipper Interactive;
this is a non-commercial fan project, not affiliated with or endorsed by them. See [NOTICE](NOTICE).
