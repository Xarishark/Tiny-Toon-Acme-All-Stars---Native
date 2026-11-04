# Credits

This project would not have been verifiable without other people's work.

The distinction below matters. Some of these are **code this project actually
runs** — mostly as independent references to check our own implementations
against. Others are **documentation and methodology** that shaped decisions.
Both are worth naming, but they are not the same kind of debt.

---

## Code this project runs

### Musashi — Karl Stenerud
<https://github.com/kstenerud/Musashi>

A mature, independently written 68000 emulator. Used as the reference in
`tools/difftest.py`: the recompiler's C output is executed side by side with
Musashi over the same instruction and the same register state, and every flag
and register is compared. 739,451 comparisons across 2,889 distinct instruction
forms from this ROM.

It found real bugs that would otherwise have shipped, including the one this
project would never have guessed: the 68000's `DIVS` of `$80000000` by `-1`
returns **zero with Z set**, not an overflow. Musashi implements that special
case; the obvious arithmetic reading does not.

### Nuked-OPN2 — Alexey Khokholov (nukeykt)
<https://github.com/nukeykt/Nuked-OPN2>

A cycle-accurate YM2612 derived from a die shot of the chip. Used as the
reference in `tools/ymdiff.py` to verify this project's FM synthesis sample by
sample. Without it, "the music sounds about right" would have been the entire
verification.

### zexdoc / zexall — Frank Cringle
Z80 instruction exerciser, with the published hardware CRCs. Used to verify the
Z80 core that runs the game's actual sound driver: 67 of 67 tests matching.

### vasm — Volker Barthelmann and Frank Wille
<http://sun.hasenbraten.de/vasm/>

The 68000 assembler used for the byte-identical reassembly gate. The emitted
assembly is assembled by vasm and compared against the original ROM byte for
byte. That an independent assembler reproduces the exact bytes is what makes the
disassembly trustworthy rather than merely plausible.

### SDL3 — Sam Lantinga and the SDL contributors
<https://www.libsdl.org/>

Window, input, gamepad and audio. Linked into the shipped executable, and
compiled from source inside the build container.

### Capstone — Nguyen Anh Quynh and contributors
<https://www.capstone-engine.org/>

The disassembly engine underneath the whole Python analysis stack, and shipped
(unmodified, version 5.0.7) in the Windows download so the recompiler can run
on the player's machine.

### TinyCC — Fabrice Bellard and contributors
<https://bellard.org/tcc/>

The C compiler in the Windows download. It turns the recompiler's output into
`jogo.dll` on the player's machine, in a few seconds, which is what lets the
release ship without any game code. Measured before adopting it: after 3,000
frames, a TinyCC build of the game produces the same screen, VRAM, CRAM and
cycle counts as the gcc build. Shipped unmodified (0.9.27), under the LGPL 2.1.

### Python — the Python Software Foundation
<https://www.python.org/>

The embeddable 3.12 distribution in the Windows download runs the recompiler.
Shipped unmodified, under the PSF license.

---

## Methodology and reference material

### segagenesisrecomp — mstan
<https://github.com/mstan/segagenesisrecomp>

Prior art for static recompilation of 68000 code, the shape of the generated C,
and the structure of a native runtime. Consulted for approach, not code.

### aerobiz-disasm — matiaszanolli
<https://github.com/matiaszanolli/aerobiz-disasm>

Disassembly methodology: labelling, separating code from data, and treating
byte-identical reassembly as the checkpoint that proves the work. That last idea
became this project's first gate.

### PokemonRedAndBlueRecomp — mstan
<https://github.com/mstan/PokemonRedAndBlueRecomp>

The closest peer to this project, by the same author as segagenesisrecomp, and
almost line for line the same architecture: SM83 machine code translated to C at
build time and compiled native, the ROM supplied by the user and loaded for data
at run time, generated C never committed, hardware emulated in a runtime library.

One deliberate difference: it falls back to an **interpreter** for code paths the
static analyser has not found, logging the address so it can be added to the
metadata. This project **aborts** instead, for the same reason but a different
trade-off — see `md_unresolved_dispatch`. Their choice is more robust; this one
is stricter, and only affordable because unresolved indirect flow reached zero.

### Gen1Recomp
<https://gen1recomp.com/>

Worth naming precisely, because the name misleads and this project cited it as a
peer for a while: **it is not a recompilation.** It is a hand-written recreation
in Lua/LÖVE2D that imports maps, graphics and text from a user-supplied ROM. It
does not translate the original assembly. The project itself acknowledges the
naming mismatch. Still a good reference for how to structure extraction and mod
layers.

### Genesis Plus GX / gpgx_debugger — lab313ru, and BlastEm — Michael Pavone
<https://github.com/lab313ru/gpgx_debugger> · <https://www.retrodev.com/blastem/>

Dynamic analysis. Used to answer "which functions run when X happens" instead of
inferring it statically.

### SGDK — Stéphane Dallongeville
<https://github.com/Stephane-D/SGDK>

Reference for how modern code addresses Mega Drive hardware. Not part of the
runtime; consulted to understand register semantics.

### megadrive_samples — BigEvilCorporation (Matt Phillips)
<https://github.com/BigEvilCorporation/megadrive_samples>

68000 assembly idioms, which is what makes hand-reading a disassembly possible
at all.

### Mega Drive architecture — Rodrigo Copetti
<https://www.copetti.org/writings/consoles/mega-drive-genesis/>

### Genesis Software Manual — Sega
The primary source for VDP registers, DMA modes and timing.

---

## The game

*Tiny Toon Adventures: ACME All-Stars* was developed and published by **Konami**
in 1994 (serial `GM T-95146-00`), based on characters owned by **Warner Bros.**

Whatever this project achieves technically, the game is theirs. It is a good
game, and taking it apart instruction by instruction mostly produced respect for
how tightly it was built — a 1 MB cartridge holding four sports, a full roster,
an FM soundtrack and a story mode, with the CPU budget managed closely enough
that it stutters in exactly the places the hardware forced it to.

## Tools

Python, GCC, mingw-w64, Docker, CMake and Ninja carry the build.
