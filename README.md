# Buckshot Roulette — 3DS Homebrew Port

A from-scratch homebrew implementation of the **Buckshot Roulette** ruleset for the
Nintendo 3DS, built with libctru + citro2d. It runs on real hardware (old & New 3DS)
via custom firmware, and installs through FBI.

> This is an **original clean-room reimplementation** of the *gameplay mechanics*.
> All code, art, audio and text here are original — no assets from the commercial
> game are used or included.

## Features

- **Turn-based shotgun duel** against an AI Dealer.
- Shells loaded with an announced mix of **LIVE / BLANK** rounds in a secret order.
- **3 escalating rounds** (2 / 4 / 6 lives) — survive all of them to win.
- Full **item set**: Magnifying Glass, Cigarettes, Beer, Handcuffs, Hand Saw,
  Adrenaline, Burner Phone, Inverter, Expired Medicine.
- **Dealer AI** that remembers revealed shells, plays the odds, and uses items
  tactically (3 difficulty levels: Calm / Standard / Ruthless).
- **Both screens used**: top screen is the table/dealer/shotgun scene with muzzle
  flash, screen-shake and particles; bottom screen is your touch-driven items,
  fire controls and event log.
- **Settings menu** (saved to the SD card):
  - *Audio*: master / SFX / music volume, mute.
  - *Graphics*: CRT scanlines, vignette, screen shake, muzzle flash, brightness.
  - *Video*: FPS counter, smooth animation, game speed.
  - *Gameplay*: Dealer AI difficulty.
- **Procedural audio** — every sound effect and the ambient drone are synthesized
  at runtime through the DSP (no audio files needed).

## Controls

| Input | Action |
|-------|--------|
| D-Pad / Touch | Move selection |
| A | Use item / fire selected |
| X | Shoot the Dealer |
| Y | Shoot yourself |
| B | Back (menus) |
| START | Quick options / restart on game over |

## Install (modded 3DS)

### Option A — QR code in FBI
1. Open **FBI** → *Remote Install* → *Scan QR Code*.
2. Scan the QR for the latest release `.cia`.
3. FBI downloads and installs it. The game appears on your HOME Menu.

The release `.cia` lives at:
`https://github.com/levibug31-max/3ds-buckshot-rullet/releases/download/v1.0/BuckshotRoulette.cia`

### Option B — manual
- Copy `BuckshotRoulette.cia` to your SD card and install it with FBI, **or**
- Copy `BuckshotRoulette.3dsx` to `/3ds/` and launch it from the Homebrew Launcher.

## Building

CI (`.github/workflows/build.yml`) builds both the `.3dsx` and `.cia` on every push
and attaches them to the `v1.0` release. To build locally you need
[devkitPro](https://devkitpro.org/) (devkitARM + libctru + citro2d):

```bash
make                # produces BuckshotRoulette.3dsx / .elf / .smdh
./build_cia.sh      # fetches makerom + bannertool, produces BuckshotRoulette.cia
```

Or entirely via Docker (no local devkitPro install):

```bash
docker run --rm -v "$PWD":/proj -w /proj devkitpro/devkitarm:latest bash -lc \
  'export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH && make'
./build_cia.sh
```

## Project layout

```
source/        game logic, dealer AI, rendering, audio, settings
assets/        original icon, banner and banner audio
app.rsf        makerom config for the CIA
build_cia.sh   CIA packaging (banner + makerom)
Makefile       devkitPro 3DS build
```
