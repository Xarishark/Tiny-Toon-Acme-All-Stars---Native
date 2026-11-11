# Tiny Toon Adventures: ACME All-Stars — Native PC Port

[![Get it on RecompLauncher](https://recomplauncher.com/assets/press/launcher/badge-dark.svg)](https://recomplauncher.com/game/tiny-toon-acme-all-stars-native)

A static recompilation of the Mega Drive / Genesis game (Konami, 1994) into C,
running on a hand-written Mega Drive runtime.

**This is not an emulator.** The game's 68000 machine code was disassembled,
translated to C ahead of time, and compiled for your CPU. There is no
instruction interpreter in the hot path. What *is* emulated is the hardware
around the CPU — VDP, Z80, YM2612, PSG, DMA and timing — because the game talks
to it directly.

There was no public disassembly of this game. The project produced its own.

**It is a recompilation, not a decompilation.** No human rewrote the game's
logic in readable form. A tool reads the original 68000 instructions and emits
equivalent C, one instruction at a time, and that C is compiled for your CPU.

So the generated code is not meant to be read. What *is* meant to be read is
`analysis/` and `docs/` — the knowledge of how the ROM is organised, and the
hand-written runtime the game needs. That is what took the work.

```
27,690 instructions traced      1,948 C files generated, one per routine
byte-identical reassembly       the emitted assembly rebuilds the ROM exactly
89.19% of the ROM classified    with evidence, not inference
```

---

## Features

- The whole game, playable start to finish, built from your own ROM
- Two players on one keyboard or with two gamepads, any controller
- Remappable keyboard keys and gamepad buttons
- Picture modes: pixel-exact, fill window, 4:3 CRT shape; windowed or fullscreen
- Original 50 Hz PAL timing and stereo FM sound from the game's own driver
- RecompLauncher support: one-step setup and settings on the game's page
- Mods: drop a DLL in `mods/`
- Fast forward and pause

---

## Status

The game is **playable start to finish**. Story mode and every stand-alone mode
have been completed without a crash.

| | |
|---|---|
| CPU | all 27,690 traced instructions translated; no unresolved indirect flow |
| Video | planes, sprites, DMA, per-line sprite limit, H40/H32 |
| Audio | Z80 running the real driver, YM2612 FM, PSG, stereo |
| Input | two players, keyboard and any gamepad |
| Timing | 153,409 68000 cycles per PAL frame, counted per instruction |

Known gaps, all **measured** rather than assumed:

- **Sprite masking** — the X=0 idiom is not implemented. Measured across 300,000
  frames: this game never uses it.
- **VDP FIFO stall** — not implemented. Measured: the game performs zero data-port
  writes during active display, so the FIFO cannot fill.
- **SSG-EG on the YM2612** — not implemented. Measured: never enabled.
- **6.35% of the ROM** is still unclassified data, most of it in the audio bank
  (a further 4.46% is inferred rather than proven, and counted separately). This
  does not affect play; it is analysis work, not a defect.

The pattern is deliberate. Three of those four were closed by *measuring whether
the game reaches them*, not by writing code. See
[docs/05-vdp.md](docs/05-vdp.md).

---

## What you need

**Your own copy of the ROM.** It is not in this repository and never will be —
it is copyrighted commercial data. You need a 1 MB raw `.bin` dump of
*Tiny Toon Adventures: ACME All-Stars* (Mega Drive/Genesis, region E).

Read [LEGAL.md](LEGAL.md) before going further. The short version: this project
distributes only its own work, and the parts derived from the ROM are generated
on your machine, from your dump, at build time.

The build verifies your file and tells you precisely what is wrong if something
is — truncated download, `.smd` interleaved dump, a different game, a different
revision, or a damaged file.

---

## Download (Windows)

No toolchain needed. Get `TinyToon-<version>-Windows-x64.zip` from the
[Releases](https://github.com/rafarvns/Tiny-Toon-Acme-All-Stars---Native/releases)
page, extract it anywhere and run `tinytoon.exe`. It asks for your ROM once,
then **builds the game from it** (about 20-30 seconds, the first time only)
with the recompiler, embedded Python and TinyCC that come in the zip. The
download itself contains no game code — see [LEGAL.md](LEGAL.md).

It is also set up for [RecompLauncher](https://recomplauncher.com): drop in your
ROM and it builds the game during setup, with the picture, fullscreen, window
size and player swap settings on the game's page. The integration is described
in [`recomplauncher.json`](recomplauncher.json).

---

## Quick start

### With Docker — nothing to install but Docker

```bash
git clone <this-repository>
cd tinytoon
cp /path/to/your/rom.bin .
./build-docker.sh                    # Linux binary
./build-docker.sh windows --release  # single-file Windows .exe
```

The first run builds the image, which compiles SDL3 from source for both
targets and takes about ten minutes. Every run after that takes seconds. Your
ROM is mounted read-only and never enters the image.

### Natively — one command

```bash
./build.sh                  # uses ./rom.bin
./build.sh /path/to/rom.bin
./build.sh --release        # windowed build: no console, log to file, icon
```

`build.sh` checks your toolchain first and stops with a specific message if
something is missing. It will not produce a half-built binary and let you find
out later.

You need Python 3.9+, the `capstone` module, a C compiler, `make`, and SDL3
(3.2 or newer). Full per-platform instructions and troubleshooting are in
[BUILDING.md](BUILDING.md).

---

## Controls

The defaults; all of them can be changed (see below).

| Mega Drive | Keyboard, player 1 | Keyboard, player 2 | Xbox pad |
|---|---|---|---|
| D-pad | `W` `A` `S` `D` | arrow keys | D-pad / left stick |
| A | `J` | numpad `1` | X |
| B | `K` | numpad `2` | A |
| C | `L` | numpad `3` | B |
| Start | `Enter` | numpad `0` | Start / Back |

| | |
|---|---|
| `F1` | pause |
| `F2` | fast-forward (~11x, muted while active) |
| `F3` | image mode: 1:1 exact / 1:1 filling the window / 4:3 |
| `F4` | swap controller ports (player 1 <-> player 2) |
| `Esc`, or L+R+Start held for a second on a pad | quit |

**Remapping.** Every key and pad button can be changed in `tinytoon.toml`, next
to the program: `p1_up` … `p2_start` take one or more SDL key names
(`p1_a = "Space, Z"`), and `pad_a` … `pad_start` take pad buttons (`a b x y`,
`leftshoulder`, `righttrigger`…). An empty value turns a key off. RecompLauncher
offers ready-made keyboard and pad layouts on the game's page, and writes the
same file. The release's `README.txt` lists every setting.

The split is physical: player 1 takes the left half of the keyboard, player 2
the right. Neither reaches into the other's region.

Player 2's face buttons and Start also accept the **top number row** (`1` `2` `3`
`0`), for keyboards without a numeric keypad.

The function keys are deliberate. `P`, `Tab`, `F` and `T` used to sit in the
middle of the keyboard, where both players have their hands — pausing or swapping
ports by accident mid-match is the kind of defect nobody reports as a bug; they
just find the program strange.

Generic controllers and original Mega Drive pads on USB adapters work too. SDL
does not recognise them as gamepads, so the port falls back to a raw joystick
mapping. `tinytoon.log` records which path each controller took — the first
thing to check if a button does not respond.

---

## How it works

```
ROM ──┬── static analysis → 68000 code → recompiler → C code
      └── extraction ─────→ game data ───────────────────┘
                                                          ↓
                       game logic (physics, AI, state, collision)
                                                          ↓
                  native MD runtime (VDP, Z80, YM2612, PSG, DMA, timing)
                                                          ↓
                                        SDL3 → Windows, Linux, Steam Deck
```

The boundary that matters: **recompiled game logic only touches hardware
through the runtime abstraction**, never SDL directly. That is what will later
allow higher internal resolution or a decoupled render rate without editing
game logic.

The project rule is *reproduce faithfully first, modify second*. Fidelity to
the original comes before any modern improvement, so that bugs introduced by
the port are never mistaken for behaviour of the original.

---

## Verification

The distinguishing feature of this project is that almost nothing is asserted.
Five gates each compare against something written by somebody else:

| gate | compares against | result |
|---|---|---|
| reassembly | vasm rebuilding the ROM from emitted assembly | **byte-identical** |
| `difftest` | Musashi, a reference 68000 core, instruction by instruction | 739,451 comparisons, 2,889 instruction forms, all matching |
| `ymdiff` | Nuked-OPN2, a cycle-accurate YM2612 | FM synthesis matching |
| `zexdoc` | published Z80 hardware CRCs | 67/67 |
| `codecdiff` | the game's own decompressors, running | 63 runs, byte-exact |

The fifth exists because of a measurement worth repeating: the LZ decompressor
"successfully decodes" **99.5% of random offsets** into the ROM. It has no
checksum and its end token occurs by chance. Without a ground truth from the
running game, a wrong decompressor would have passed unnoticed and poisoned
every extraction downstream. See [docs/25-codecs.md](docs/25-codecs.md).

---

## Repository layout

```
runtime/     the hand-written Mega Drive runtime, in C. Our work.
tools/       analysis, disassembly, the recompiler, extraction. Python.
analysis/    what the disassembly knows: functions, symbols, jump tables,
             data regions — each entry carrying the evidence for it.
docs/        the research notes: how each part was measured and decided.
build/       generated. Never committed. This is where the ROM-derived C lives.
```

`analysis/` splits into two classes, and confusing them breaks the workflow:
hand-curated **input** (`jump-tables.csv`, `symbol-names.csv`, …) where verified
knowledge is recorded with its evidence, and regenerable **output**
(`functions.csv`, `rom-map.csv`, …) that the tracer overwrites.

The operating principle throughout: **do not guess.** An address is code only if
there is an execution path to it. The tracer validates every table before
seeding targets and rejects whole tables on a single bad entry, because a guess
recorded as fact contaminates all analysis downstream.

---

## Documentation

`docs/` holds the research notes — how each part was measured and decided, and
what was found to be wrong along the way. They are the most useful thing here
after the code itself.

| | |
|---|---|
| [00-project-overview.md](docs/00-project-overview.md) | what exists and what does not |
| [02-rom-format.md](docs/02-rom-format.md) | the ROM's structure, and the jump-table problem that gates everything |
| [03-memory-map.md](docs/03-memory-map.md) | console map, and what is known about the game's RAM |
| [05-vdp.md](docs/05-vdp.md) | the video chip, and three features measured as unreachable |
| [09-input.md](docs/09-input.md) | controllers, and why the header's `J4` is false |
| [10-graphics.md](docs/10-graphics.md) | palettes and compressed graphics |
| [14-audio.md](docs/14-audio.md) | Z80, YM2612, and the errors that turned PCM into noise |
| [16-functions.md](docs/16-functions.md) | what has been identified, and the evidence for each name |
| [18-recompilation.md](docs/18-recompilation.md) | how 68000 becomes C |
| [19-native-runtime.md](docs/19-native-runtime.md) | the runtime, and five bugs only running could find |
| [21-build-system.md](docs/21-build-system.md) | the build, and the rules that preserve byte identity |
| [22-differential-testing.md](docs/22-differential-testing.md) | the gates, in detail |
| [23-timing.md](docs/23-timing.md) | why the clock counts cycles, and what changed when it did |
| [24-read-map.md](docs/24-read-map.md) | how running the game became the extraction tool |
| [25-codecs.md](docs/25-codecs.md) | the three compression formats, written out |
| [26-packaging.md](docs/26-packaging.md) | turning a diagnostic binary into a program |
| [27-mods.md](docs/27-mods.md) | the mod ABI: plugging one in, and writing one |

## Mods

Drop a shared library into `mods/` beside the executable. No rebuild, no
patching the binary. The whole contract is one header,
[`runtime/mod.h`](runtime/mod.h).

```
tinytoon.exe
mods/
  filtro-cor.dll
rom.bin
```

A mod reads game state — work RAM, VRAM, palettes, VDP registers — and rewrites
the finished frame before it reaches the screen. It cannot write game state,
which is deliberate: a modded session and a clean one still *play* identically,
so the port's fidelity stays checkable while the interesting parts get built.
Write access is the next ABI version.

`mods/filtro-cor/` is a working example — it repaints the frame in four shades
of green, which is unmistakable enough that you know it loaded from across the
room. Building and writing mods: [docs/27-mods.md](docs/27-mods.md).

## Reporting a bug

Attach **`tinytoon.log`**, written beside the executable in a `--release` build.
It records the build version, the ROM verdict, which controllers were detected
and by which path, cycle budget overruns, unimplemented hardware accesses,
sprite-per-line peaks and the audio queue counters.

Note that `funcao nao traduzida alcancada: $XXXXXX` and `despacho fora de tabela`
are not crashes. They are the port refusing to execute something the analysis has
not resolved, and they name the address that needs work. That is how the last
open dispatch site was found — somebody finished the racing stage.

## Contributing

Open to anyone. [CONTRIBUTING.md](CONTRIBUTING.md) covers the one rule that makes
the rest trustworthy — *do not guess* — plus what counts as proof for a jump
table's extent, and the gates to run before a pull request.

## Credits

This project stands on other people's work — reference implementations that made
verification possible, and documentation without which the hardware would have
had to be reverse-engineered too. See [CREDITS.md](CREDITS.md).

## License

**Public domain** ([Unlicense](LICENSE)). The runtime, tools, analysis data and
documentation here are original work, released with no strings attached — take
it, use it, sell it, fork it, no attribution required.

That does not and cannot extend to the game itself, or to the third-party work
used for verification, which keeps its own license. See [LEGAL.md](LEGAL.md) and
[CREDITS.md](CREDITS.md).

*Tiny Toon Adventures: ACME All-Stars* is © Konami and Warner Bros. This project
is not affiliated with, endorsed by, or connected to either. No game data is
distributed here.
