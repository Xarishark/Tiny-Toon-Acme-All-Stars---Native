# 05 — VDP

> State: the VDP stores, draws, and derives status and the H/V counter from the
> cycle clock. Measured: **this game uses no raster effects** — see the scanline
> section.

In 3,000 frames (≈1 min of play):

```
VDP: 320x224, plane 128x32 tiles, display on
  plane A $C000 | plane B $E000 | sprites $B800 | hscroll $BC00
  writes: VRAM 926,593, CRAM 26,205, VSRAM 6,618 | DMA 5,211 (1,737,397 bytes)
```

## The control port is a protocol, not a register

This was the detail that forced a change in the bus. `$C00004` is 16 bits wide
and has two forms, distinguished by the top two bits of the **first** word:

- `10rrrrr dddddddd` — a register write, `r` = number, `d` = value. It ends there.
- anything else — the first half of an address; the **second word** completes the
  address and the CD code, and it is that one that can trigger DMA.

The game writes 32 bits at once all the time (a single long write of an address command to the
control port), which is
the two words in sequence.

The problem: `md_write16` decomposed everything into two `md_write8`. For RAM
that is equivalent; for the control port it turns **one** word into **two**, and
the state machine goes permanently out of phase. So the VDP is now served at word
level, and the *byte* write became the derived case — duplicating the byte into
both halves, as a 16-bit bus does.

It holds as a general rule for the runtime: decomposition into bytes is a
convenience of memory, not of hardware. Any peripheral with a protocol needs to
see the whole word.

## DMA does not go through the CPU

DMA moves up to 128 KB at once. If every byte went through `md_read8`, each
transfer would advance the frame clock by thousands of accesses and trigger
V-blank in the middle of itself. On hardware, DMA is the VDP using the bus, not
the CPU — hence `md_peek8`, a raw read with no side effects.

The three modes come from the top two bits of register `$17`: `0x` = 68000→VDP,
`10` = VRAM fill, `11` = VRAM copy. A fill stays **armed** until the next write
to the data port, which is what brings the byte.

## Two details that change the image

**An odd VRAM byte swaps, it does not shift.** VRAM is byte-addressed but
word-written, and bit 0 of the address swaps the two bytes. Not a curiosity: it
is what makes a DMA fill distribute the byte the way the game expects.

**Colour levels are not linear.** CRAM stores `0000 BBB0 GGG0 RRR0`, three bits
per channel. Using `level × 36` would give a similar and wrong image in the dark
tones. The real DAC levels are `{0, 52, 87, 116, 144, 172, 206, 255}`.

## Composition

Each layer becomes a buffer of "VDP pixels" — 0 is transparent, otherwise
priority + palette + index. Composition follows the hardware's order, bottom to
top:

```
background → B low → A low → sprites low → B high → A high → sprites high
```

Deciding priority during drawing would be faster and much harder to check. Three
buffers and one composition pass cost memory and return a rule you can read.

A sprite detail nobody guesses: the tiles of a multi-cell sprite are stored in
**columns**, not rows.

## Output

On exit — frame limit, budget overrun or abort — a development build writes:

| File | What it is |
|---|---|
| `tela.png` | planes B+A and sprites composed, at the current size (320×224 or 256×224) |
| `vram.png` | the whole VRAM as a sheet of 2,048 tiles, palette 0 |
| `cram.png` | the 4 palettes, 16 colours each |

The PNG is written without zlib or libpng: the deflate format allows *stored*
blocks, so a valid PNG comes out of a trivial zlib stream. Bigger file, zero
dependencies.

Running with different frame limits gives different moments:

```bash
./tinytoon rom.bin 0 700     # Konami logo, in H32
./tinytoon rom.bin 0 1800    # title screen
./tinytoon rom.bin 0 3000    # basketball court
```

## Three bugs found by playing

The user reported "the first screen has text you cannot read because the
background is broken". The cause was four levels away, and the path to it is
worth more than the fix.

### The method: separate the layers

Composing three layers and looking at the result tells you *something* is wrong;
it does not tell you what. `vdp_dump_layers` draws each one alone, with
transparent in magenta — fifteen lines that turned "the background is broken"
into **"plane B draws where it should be transparent"**. From there the raw VRAM
(`vdp_dump_vram_bin`) showed that plane B's table was correctly zeroed and that
the problem was **tile 0** containing garbage.

### 1. Reading the control port clears the second-word latch

Without that the latch stays stuck, and the next register write is swallowed as
half of an address. The port log caught it in the act:

```
 12 ctrl  0000  1st word   second=0 code=$00 addr=$0010
 13 ctrl  96FC  2nd word   second=1 code=$00 addr=$0000   <- $96FC is register $16!
```

With CD5 accidentally set, that fired a phantom DMA of 65535 words.

### 2. V-blank must not arrive before the game enables it

The check on **bit 5 of register `$01`** (IE0) was missing. Two locks, not one:
the SR mask belongs to the CPU, bit 5 belongs to the VDP, and it is the one the
game uses to say "you may interrupt me now". Without that lock the handler ran
during boot and read register shadows still zeroed — `$FD02` held `$0000` instead
of `$81xx`, so setting bit 4 on it produced `$0010`, which lacks the `$8x` register-write
prefix and entered the port as an address. From there all parity broke.

### 3. A DMA length of zero transfers nothing

This contradicts the usual emulator convention (0 = 65536). The evidence against
it comes from the game itself:

- the handler calls `sub_001922` at `$000450`, which sets up the sprite table DMA
  reading the length from `$FFFB3C`;
- what writes `$FFFB3C` is `sub_0016A8`, called at `$00047C` — **afterwards**;
- `sub_0016A8` has exactly **one** caller, that one.

So on the first V-blank the length is inevitably the initialisation value
(`$94009300`, zero) — on the console as much as here. With 65536, that DMA dumps
128 KB of RAM over the entire VRAM, and the game **never clears VRAM again**:
after boot, the only DMAs to a low address go to CRAM (`code $23`). Tile 0 stays
dirty forever, and plane B — its table all zeros — shows that garbage across the
whole screen. A shipped game does not do that on hardware.

This is inference from **behaviour**, not from a datasheet, and it is marked as
such in the code. If another screen breaks in a way only 65536 explains, it is
the first place to look.

### Also: the PAL bit

Status now returns bit 0 = 1. This ROM is region E, and the game uses that bit to
choose timing constants.

## DMA charged to the 68000 — and the FIFO that need not exist

On hardware the VDP takes the bus during a DMA and the CPU **stops**. It was the
most expensive operation a Mega Drive game performs, and here it cost nothing.

The rates are the hardware's in H40: the VDP drains ~83.5 words per line during
retrace and ~9 with the display active; a line is 490 68000 cycles. That gives
5.9 and 54 cycles per word.

Measured before implementing, and after:

```
DMA cost:              1,682 cycles per frame (1% of the budget)
V-blank handler:      62,867 → 64,120 cycles/frame   (+2%)
frames that overrun:      176 → 272 of 20,001        (+55%)
```

Small on average, large in the tail — and the tail is what matters. Those 96
extra frames are screen transitions where the console stutters and the port did
not.

**The FIFO-full stall was not implemented, and the reason is measured.** The
hardware has a four-slot queue; when the 68000 writes to the data port faster
than the VDP drains, it stalls. This game performs **153 data-port writes with
the display active in 20,000 frames** — zero per frame. Without writes during
display there is no way to fill the queue. Implementing the state machine would
be code that never executes.

It is recorded in the code as the first place to look if another game enters this
runtime.

## Per-line sprite limit — this one *was* reachable

The VDP has a per-line budget: **20 sprites and 320 pixels in H40** (16 and 256
in H32). It processes the sprite list in *link* order and, on overrun, simply
**stops drawing** on that line — and sets bit 6 of the status.

Measured before implementing, over 20,000 frames:

```
peak sprites per line:   21  (limit 20)
peak pixels per line:   384  (limit 320)
lines asking for more than the budget:  208
```

Unlike Team Player, the line counter and the FIFO — **the game reaches this
one**. Rare (0.01 lines per frame), but where it happens the console shows fewer
sprites than the port was showing.

Implemented, the cutoff fires **168 times** over the same 20,000 frames.

### How the cutoff is computed

Because the discard is by **order**, it is enough to record the index of the
first rejected sprite on each line: whatever comes before passes, whatever comes
after does not. That avoids restructuring the renderer from sprite-major to
line-major.

The order that matters is the **linked list's** (index 0, following the link
field), not the address order in the table — that is how the hardware processes
it, and it is why games reorder the list to choose who survives.

### One implementation, not two

The first version had the measurement in one place and the cutoff in another. I
merged them: `calcular_corte` does the cutoff **and** the peaks, in the same loop
over the same list. Two implementations of the same calculation drift apart over
time, and the one nobody looks at is the one that is wrong.

## Sprite masking: measured, and the game does not use it

This was the last VDP feature outside the runtime — the idiom of putting a sprite
at X=0 to hide the ones after it on that line. Before implementing, measure
whether this game uses it: a counter in `montar_lista` counts every entry with a
raw X of 0 that is not the first in the list.

```
12 explorers × 25,000 frames = 300,000 frames
sprite masks: 0    (zero across all 12 runs)
```

**Zero.** Implementing the mask would be writing code that never executes in this
game. The counter stays in the report permanently: if another game enters this
runtime, it warns instead of letting the image go silently wrong.

With that, **no VDP feature this game uses is missing**. The three remaining gaps
— the FIFO stall, the line counter for raster, and now masking — were all
dissolved by measurement, not by implementation.

## Scanline: measured before implementing

The line counter was on the list as "unlocks raster effects". Before writing it,
I measured whether this game **uses** raster. Several independent pieces of
evidence:

| Measurement | Result |
|---|---|
| CRAM writes mid-frame | **65** in 20,000 frames (out of 189,969 total) |
| Registers changed mid-frame | **63** in 20,000 frames |
| H-blank interrupt | the level 4 vector is just an `rte` |
| H/V counter reads | **zero** |
| Status register reads | **77** in 20,000 frames |

A raster effect would make dozens of writes **per frame**. Sixty-five in total
are screen transitions. **This game does not do raster**, and a line counter
would not change the image.

What it does fix is something else: the status register was a stub that
**toggled** the V-blank bit on every read. With the clock in cycles the line
comes for free — 153,409 ÷ 313 = 490 cycles per line — so the honest version
costs fifteen lines and removes a lie from the code.

A detail the measurement made visible: **71 of the 77 reads come from inside the
V-blank handler**. A naive computation would say "line 0, display active" there,
because the handler is called just as the frame counter wraps — which would be
*worse* than the stub. While the handler runs, V-blank is happening by
construction, and that is what gets reported.

The H/V counter at `$C00008` went in too, despite being measured as never read:
returning garbage from a register that exists is worse than returning the right
value.

## Storing and timing, kept separate

Storing is what lets you see what the game draws; timing is what enables raster.
Mixing the two was the shortest path to being unable to debug either.
