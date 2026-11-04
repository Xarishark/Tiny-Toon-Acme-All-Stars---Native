# 24 — The read map: playing the game as the extraction tool

> Static analysis does not reach this game's data. A port that **executes** does.
> ROM coverage went from **34.61%** to **89.19% with evidence** by adding a
> two-field counter to the bus.

## The problem

After the byte-identical reassembly and the tracer, 65% of the megabyte was still
opaque. Not for lack of effort: for a limit of method.

The game does not reference its data with literal addresses. It reaches it
through pointer tables indexed at run time — a long pointer loaded from a table
at an index held in a data register, and variations. Following that statically would require data-flow analysis, and even
then it would produce candidates, not facts. Recording a candidate as fact is
what the project's rule forbids.

## What changed

The port runs the game. So just ask the bus.

Two maps, one entry per ROM byte, written by `runtime/memory.c`:

| file | contents |
|---|---|
| `romlido.bin` | **how** it was read — bit 0 CPU, bit 1 VDP DMA, bit 2 Z80 bank window |
| `romleitor.bin` | **who** read it — the 68000 function's address, or the Z80's PC |

This is not inference. It is not "I think this is data": it is "`LzDecompressA`
read these bytes, on this path, on this frame".

The "who" is what makes the map worth having. Knowing a block was read says it is
data. Knowing it was read by `RleUnpack` says what it **means**, because that
routine is disassembled and you can read what it does with it. And the block's
boundaries are not guessed — they are where the reader changes.

### How "who" is recorded

The recompiler emits `MD_FUNCAO(0xADDR)` at the entry of every function. The
macro uses gcc's `__attribute__((cleanup))`, which restores the previous value on
**any** exit — including the several `rts` in the middle of a body, which are the
rule and not the exception in recompiled code. Saving on entry and restoring "at
the end" by hand would be wrong in almost every function.

Cost: two memory operations per call, and the map is only written when
`TINYTOON_ROMLIDO` is set.

## Two traps that nearly invalidated the map

**The boot checksum.** The first measurement reported 99.95% of the ROM read — a
useless number. Boot runs the header checksum loop, which walks `$200..$FFFFF`
byte by byte. Reading everything once in order to add it up is not evidence that
a byte *is* anything. The map is now cleared at frame 200; what is lost are the
reads exclusive to boot, which static analysis already classified as code.

**The Z80 bank window.** After that, 64 KB appeared as "DMA" — all in
`$0E0000`–`$0FFFFF`, and attributed to `Random`, a seven-instruction routine that
only touches RAM. The cause: `z80_ler` uses `md_peek8` for the bank window, the
Z80 executes interleaved, and the recorded reader was whoever happened to be
interrupted. Fixed with its own path (bit 2) and with the **Z80's PC** as the
reader — which is what separates a PCM sample from an instrument table.

Measured afterwards: **zero** ROM-to-VRAM DMA. This game decompresses to RAM and
DMAs from there.

## The coverage explorer

The map only classifies what the game reads, and the game only reads what the
current screen asks for. Running only the attract loop leaves the art of a
character nobody selected opaque forever. Somebody has to play — and a machine
plays sixteen matches at once.

`md_explorar_quadro()` presses buttons on its own. There is nothing clever about
it: it is a monkey at a keyboard. That is enough, because the goal is not to win
the match, it is to **get through a menu and reach a new screen**. START is
weighted higher because almost every menu in this game opens with START, and the
button is released half the time because a menu that reads a rising edge ignores
a button that never rises.

Deterministic on purpose: same seed, same buttons, same map. A coverage run has
to be repeatable to count as evidence.

The maps **accumulate**: merging is a logical OR, and today's session adds to
yesterday's. ROM coverage is monotone — a byte somebody has seen the game read
does not go back to being unknown.

Different seeds take different paths through the menu tree, so several short runs
cover more than one long one. The measured curve:

| round | classified |
|---|---|
| attract only, 20k frames | 58.34% |
| 16 × 20k | 72.15% |
| + 16 × 20k | 75.74% |
| + 16 × 20k | 78.41% |
| + 16 × 20k | 79.40% |
| + 3 × 16 × 60k | 81.01% |

It saturates near 81%: the monkey neither finishes a match nor visits every
character. Beyond that the gain comes from real play — and the same map records
it, with `TINYTOON_ROMLIDO=romlido.bin` in a normal session.

## Evidence, constant and inference do not add up

`analysis/rom-leituras.csv` has a `confianca` column with three values, and the
coverage report **never** merges them into one number:

- **`execucao`** — the game read these bytes, on this path, in this routine.
- **`constante`** — a run of a single repeated byte that no session ever touched.
  It is the space between two files, or the tail of the last bank. Verifiable by
  looking at the ROM.
- **`inferido`** — a hole surrounded on both sides by the **same** reader. A
  compressed archive stores its blocks back to back; if the block before and the
  block after share an owner, the hole is almost certainly another block nobody
  selected. *Almost* certainly. It is labelled with a `?` and reported on its own
  line.

The golden rule applies to metrics too: a number that adds what is known to what
is supposed stops measuring progress and starts measuring optimism.

```
CLASSIFIED (evidence):  89.19%  935,239 bytes
  + inferred:            4.46%   46,783 bytes
  MISSING:               6.35%   66,554 bytes
```

## The third codec

The map produced a discovery no step of static analysis had found:
**`RleUnpack` (`$01AD02`)**, the ROM's largest data consumer after the two LZ
codecs — 147 KB read across 382 regions.

It is a word-oriented RLE. It reads a word from `(a4)+`:

- **negative** → a literal run of `w - $8000` words;
- **positive** → the low byte is replicated into both halves of a word and
  written `high byte` times.

It stops when the destination fills. Two variants mask each written word:
a mask of $7777 for `d0 >= 6`, a mask of $BBBB for `3 <= d0 < 6`. The destination comes
from a table at `$01ACCE`: RAM `$FF0000 + $800*d0`, in 2 KB slices.

It was found by the reverse of the usual path — not "I found a routine, what does
it do?", but "147 KB of the ROM ends up at `$01AD02`, who is that?".

## Sound bank sweep

After the random explorer saturated at 81%, **69% of what was still missing lived
in `$0E0000`–`$0FFFFF`** — the audio bank. A track nobody selected is not read,
and the monkey never reaches the sound test.

The game has the right door for this. `PlaySound` (`$0E5064`) only deposits a
byte into a ring in RAM (`SoundQueue`, `$FFD410`) and advances the write index;
what delivers it to the Z80 is `SoundQueueDrain` (`$0E5098`), on the next frame,
taking the bus via `$A11100` and writing into the `$A01E00` mailbox.

Writing into that ring is **exactly** what the sound test does. The runtime
writes all 256 IDs, one every N frames; the bus protocol is untouched and the
game cannot tell the request did not come from a button.

It returned less than the estimate promised — 88.4% to 89.0%. The large blocks
left in `$0E`/`$0F` are statistically **indistinguishable** from confirmed PCM
data (mean 120-136, mean delta between neighbouring bytes 90-100, against 50 for
4bpp art), but none of the 256 IDs reaches them. They remain strong candidates
without execution evidence — which is precisely what the inferred label exists to
say.

## An island of code nobody calls

`$01EAE0`–`$01F273`, 1,940 bytes, decodes cleanly from start to end: clear
`$FF0000` and `$FF2000`, write VDP register `$0B` from a table at `$01EB16`, and
16 calls to a VRAM filler at `$01ED26`.

And nothing reaches it. The only absolute call that enters the region comes **from
inside it** (`$01ECCA`); the island is closed. And the port has executed more than
1.4 million frames without ever dispatching outside a table.

Recorded as `codigo_orfao` — probably a test or reset routine left in the ROM. It
does not enter the recompiled build, and now the fact that it does not has
evidence behind it instead of being a silent omission.

## Workflow

```bash
# 1. record (a normal session records too)
TINYTOON_ROMLIDO=romlido.bin ./tinytoon rom.bin

# 2. or many sessions in parallel, accumulating
tools/explorar.sh 16 20000 0

# 3. cut into regions and rewrite analysis/rom-leituras.csv
python tools/rom_leituras.py --csv

# 4. measure
python tools/rom_coverage.py
```

`analysis/rom-leituras.csv` is **regenerable output** — never edit it by hand.
When a region earns a confirmed semantic name, the name goes into
`analysis/data-regions.csv`, which is curated input and takes precedence.
