# 00 — Project overview

A native PC port of *Tiny Toon Adventures: ACME All-Stars* (Mega Drive, Konami
1994), by reverse-engineering the ROM and statically recompiling the 68000 code
to C.

This document covers what **exists today** and how to operate it.

## The golden rule

> Reproduce faithfully first. Modify second.

The corollary that shapes the tooling: **do not guess.** An address is
classified as code only when there is a proven execution path to it. Anything
static analysis cannot resolve on its own becomes an explicit item in
[`analysis/unresolved-flow.csv`](../analysis/unresolved-flow.csv) — never a
silent guess recorded as fact.

## Current state

**The game is playable start to finish.** Story mode and every stand-alone mode
have been completed without a crash.

| Layer | State |
|---|---|
| ROM | header, vectors, checksum and hashes locked in a regression baseline |
| Disassembly | 27,690 instructions; **byte-identical reassembly passes** |
| Recompilation | 100% of instructions translated, 1,948 `.c` files, one per routine |
| Indirect flow | **0 unresolved** — every indirect jump and call lands on a validated target |
| 68000 | verified against Musashi: 2,889 distinct forms, 739,451 comparisons, all matching |
| VDP | VRAM/CRAM/VSRAM/DMA, planes, sprites, window, per-line scroll, per-line sprite limit, status derived from the clock |
| Input | two players, keyboard and any gamepad, 3-button controller |
| Audio | Z80 (zexdoc 67/67) running the real driver, YM2612 FM verified against Nuked-OPN2, PSG, stereo |
| Timing | clock in 68000 cycles, 153,409 per frame |
| Unimplemented hardware | **7 accesses in 20,000 frames**, all during boot |

### The five regression gates

Each answers a different question, and none replaces another:

| Gate | Question |
|---|---|
| reassembly (`vasm`) | does the disassembly reproduce the ROM byte for byte? |
| `tools/difftest.py` | does the generated C do what a 68000 does? (against Musashi) |
| `runtime/testes/fmdiff.c` | does the FM synthesis sound like the chip? (against Nuked-OPN2) |
| `zexdoc` / `zexall` | is the Z80 correct? (CRCs against real hardware) |
| `tools/codecdiff.py` | do the Python decompressors produce what the game's do? |

The repeating pattern is **independent reference**. Comparing against a model I
wrote myself would test my reading of the manual against itself — and that is
exactly how a `cmpa.w` missing its sign extension survived across 196
instructions until somebody played the game.

### What is missing, and how we know

Three of the four gaps below were closed not by writing code but by **measuring
whether the game reaches them**:

| | |
|---|---|
| Sprite masking | measured over 300,000 frames: this game never uses it |
| VDP FIFO stall | measured: zero data-port writes during active display, so the FIFO cannot fill |
| SSG-EG on the YM2612 | measured: never enabled |
| Bit-exact PM (vibrato) | computed from documented cents, not from the chip's table |
| MEMPTR on the Z80 | undocumented flags of `BIT n,(HL)` |
| Modern extras | native resolution, widescreen and a decoupled frame rate are not started; mods already work |

## ROM analysis, measured

`tools/rom_coverage.py` walks the whole megabyte and assigns every byte a
category, with the evidence behind it:

```
CLASSIFIED (evidence):  89.19%   935,239 bytes
  + inferred:            4.46%    46,783 bytes
  MISSING:               6.35%    66,554 bytes
```

Evidence and inference are never added together. A number that mixes what is
known with what is supposed stops measuring progress and starts measuring
optimism.

Two things carried that number from 34.61% to where it is:

**The read map.** The game reaches its data through pointer tables indexed at
run time, which static analysis cannot follow. The running port can: it records,
byte by byte, *how* each ROM byte was read (CPU, VDP DMA, Z80 bank window) and
*who* read it. Playing the game became the extraction tool. See
[24-read-map.md](24-read-map.md).

**The three codecs.** `LzDecompressA`, `LzDecompressB` and `RleUnpack`
reimplemented in Python and verified against the game itself. See
[25-codecs.md](25-codecs.md).

What remains unclassified is mostly the audio bank — tracks and effects no
session has played. It does not affect play; it is analysis work, not a defect.

## Tooling

Requirements: Python 3.9+ and `capstone` (`python -m pip install capstone`).

The ROM is not versioned (commercial data, supplied locally by the user). Put
the `.bin` dump in the repository root or in `bin/`; the tools locate any file
with a `SEGA` signature at `$100` on their own.

```bash
# Header and vector table report
python tools/rom_info.py --vectors

# Regression gate: compares the ROM against analysis/rom-baseline.json
python tools/rom_info.py --check

# Recursive code scan; writes analysis/*.csv
python tools/trace_code.py

# Report only, writing nothing
python tools/trace_code.py --dry-run

# Scan from the vectors only, ignoring accumulated knowledge
python tools/trace_code.py --no-tables

# Inspection bench — used to investigate each pending site
python tools/dump.py 0x9c8 -n 12        # disassembly
python tools/dump.py 0x9e4 --hex 64     # hex
python tools/dump.py 0x000b34 --table 9 # read words as offsets relative to a base
python tools/dump.py 0x0004b6 --ptr 16  # read longs as pointers

# How much of the ROM is classified, and what is left
python tools/rom_coverage.py
```

`rom_info.py --check` runs both on the original ROM (guarding against an
accidental dump swap: different region, different revision, corrupted file) and
on the reconstructed ROM, where it is the final step of the reassembly gate.

### Input vs. output files in `analysis/`

This distinction is the heart of the workflow:

| File | Role | Edited by |
|---|---|---|
| `jump-tables.csv` | **input** — hand-verified jump tables | human |
| `dispatchers.csv` | **input** — subroutines that consume an inline table | human |
| `symbol-names.csv` | **input** — function names with the evidence behind them | human |
| `ram-names.csv` | **input** — RAM variable names, with evidence | human |
| `data-regions.csv` | **input** — identified data ranges (palettes, etc.) | human |
| `emit-fallbacks.csv` | **input** — instructions emitted as raw `dc.w` (empty today) | human |
| `rom-baseline.json` | **input** — identity of the original ROM | generated once |
| `functions.csv` | output | `trace_code.py` |
| `symbols.csv` | output | `trace_code.py` |
| `rom-map.csv` | output | `trace_code.py` |
| `ram-map.csv` | output | `trace_code.py` |
| `jump-tables-detected.csv` | output — automatically recognised tables, for review | `trace_code.py` |
| `unresolved-flow.csv` | output — the work queue | `trace_code.py` |
| `rom-leituras.csv` | **execution output** — what the running game read, and who read it | `tools/rom_leituras.py` |

The work cycle is: run the tracer → open `unresolved-flow.csv` → investigate an
indirect target → record what was **confirmed** in `jump-tables.csv` or
`dispatchers.csv` → run again and watch coverage rise. Verified knowledge goes in
as versioned input; output is always regenerable from the ROM.

`rom-leituras.csv` is a third class: it comes from the game *running*, not from
static analysis. Its `confianca` column (`execucao`/`constante`/`inferido`)
cannot be collapsed into a single number — see [24-read-map.md](24-read-map.md).

## What is worth doing next

**From the port:**

1. **Play with recording on.** `TINYTOON_GRAVAR=session.txt` makes a session a
   byte-reproducible replay; `TINYTOON_ROMLIDO=romlido.bin` makes it raise ROM
   coverage. A full playthrough with both would become a permanent regression
   test.
2. **Linux and Steam Deck packaging.** The runtime already compiles; the
   single-file equivalent (an AppImage) does not exist yet.

**From the analysis**, which runs independently and is what enables mods:

3. `$FFA600`–`$FFA6FF` is the hottest struct in the ROM and has no named field.
   `$FFA622` alone has 200 references.
4. Review `jump-tables-detected.csv` by sampling — tables recognised
   automatically that nobody has checked one by one.
5. Keep naming functions in `symbol-names.csv` and addresses in `ram-map.csv`.
6. Translate the remaining research notes, and keep extending data extraction.
