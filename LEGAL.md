# Legal

Plain language, no hedging. This page exists so that nobody has to guess what
this project distributes and what it does not.

## What is here

Original work, all of it:

- `runtime/` — a Mega Drive runtime written from scratch in C: the 68000
  translation layer, the VDP, the Z80, the YM2612 and PSG, DMA, timing, and the
  SDL3 presentation.
- `tools/` — the disassembler, tracer, recompiler, extraction tools and
  verification harnesses, written in Python.
- `analysis/` — what the disassembly knows: function boundaries, symbol names,
  jump tables, data regions. This is *metadata about* the ROM — addresses,
  sizes, names and the evidence for each. It contains no game data.
- `docs/` — research notes.

## What is not here, and never will be

**The ROM.** *Tiny Toon Adventures: ACME All-Stars* is © Konami and Warner Bros.
Its code and data are copyrighted commercial works. This repository contains no
part of them, and no build artifact derived from them is committed.

The `.gitignore` enforces this, and `tools/preparar_publico.py` — the script
that assembles this repository — refuses to publish any file with a binary
extension, any file over 1 MB, and anything matching a list of forbidden
strings. That barrier has caught real mistakes.

**The recompiled C.** The ~1,950 generated C files are a mechanical translation
of the game's machine code. They are a derivative work of the ROM, so they are
generated on your machine, from your dump, at build time, and never shipped.

This is not a workaround. It is the honest structure of the thing: what this
project actually produced is the *knowledge of how the ROM is organised* and the
*runtime it needs*. Neither of those is Konami's. The game itself remains
Konami's, and you have to bring your own.

## The Windows download

The release on GitHub (and on RecompStore / RecompLauncher) follows the same
rule. It does **not** contain the game. It contains:

- `tinytoon.exe` — the runtime only: this project's code plus SDL3. It holds
  no translated game code; it loads it from `jogo.dll`.
- `montador/` — the recompiler and the `analysis/` metadata, the same files as
  in this repository.
- `python/` and `tcc/` — an embedded Python 3.12 and TinyCC, unmodified, under
  their own licenses (in `LICENSES/`), used only to run the recompiler and
  compile its output.

`jogo.dll`, the game itself, is produced **on the player's machine, from the
player's ROM**, the first time it runs or during RecompLauncher's setup. It is
the same translation a developer build makes; it just happens on the PC of the
person who owns the dump instead of on ours. The release packaging script
(`tools/montar_pacote.py`) refuses to build a zip that contains a `jogo.dll`,
any generated C, or any file the size of the ROM.

## What you need to bring

A 1 MB raw `.bin` dump of the game, obtained lawfully.

What "lawfully" means depends on where you live, and this file is not legal
advice. In many jurisdictions making a personal backup of software you own is
permitted; in others it is not; and downloading a copy from the internet is
generally not the same thing as making one yourself, regardless of whether you
own a cartridge. Find out what applies to you.

The one thing this project asks is that you do not ask it to help you skip that
step. No build script here will fetch a ROM, and no issue asking where to find
one will be answered.

## Why this shape

Static recompilation projects live or die on this distinction. The value is in
the analysis and the runtime — the part that took the work. The ROM is an input,
like a document is an input to a word processor. Shipping the input would be
both illegal and beside the point.

## Trademarks

*Tiny Toon Adventures* and related characters are trademarks of Warner Bros.
Entertainment Inc. *ACME All-Stars* was developed and published by Konami.
Sega, Mega Drive and Genesis are trademarks of Sega Corporation. This project is
not affiliated with, endorsed by, sponsored by or connected to any of them.
