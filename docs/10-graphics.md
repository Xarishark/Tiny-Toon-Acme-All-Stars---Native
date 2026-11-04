# 10 — Graphics

The data extraction track. Covers palettes and compressed graphics streams.

## Palettes

### The format gives an exact detector

A VDP colour is a word:

```
bit  15 14 13 12 | 11 10  9  8 | 7  6  5  4 | 3  2  1  0
      0  0  0  0 |  B  B  B  0 | G  G  G  0 | R  R  R  0
```

Three bits per component, and the bits in mask `$F111` are **always zero**. This
is not a heuristic: a sequence that fails the mask cannot be a palette. The
converse does not hold — zeros pass trivially — so the detector also requires a
minimum of distinct, non-zero colours.

### What was found

`python tools/extract_palettes.py --png`

| Range | Palettes | Bytes |
|---|---|---|
| `$038FD0`–`$03958F` | 46 | 1,472 |
| `$000EDC`–`$000F1B` | 2 | 64 |
| `$0113F6`–`$011415` | 1 | 32 |

**49 palettes, 1,568 bytes.** The main bank is contiguous and referenced by 8
`lea`s in traced code, with `$038FD0` and `$039530` as the most-used bases.

The two palettes at `$000EDC` are different from the rest: they are monotonic
ramps (each colour a single step brighter than the last), i.e. a **fade** table, not a scene palette.

Outputs: `analysis/palettes.csv` (colours in VDP format and RGB) and a contact
sheet in `build/palettes/all.png`, one palette per row.

### A trap in the detector

The first version scanned two bytes at a time and, when a candidate failed the
richness filter, advanced two bytes and tried again. Inside a palette bank that
is disastrous: almost every shifted window also passes the mask, so the scanner
locks onto the wrong grid. The result was "44 palettes in 8 blocks", with
addresses like `$03915A`, which is not a multiple of 32 from the bank's start.

The correct version first finds the **maximal runs** of words that pass the mask,
and only then slices each run into 16-colour palettes from the run's start. Then
the result matches manual inspection: one contiguous block of 46.

The symptom that exposed the error was misalignment. It holds as a general rule
for the next extractors: **if the addresses found do not fall on a regular grid,
the detector is slipping.**

## How identified data enters the disassembly

`analysis/data-regions.csv` is an **input** file, versioned, in the same spirit as
`jump-tables.csv`:

```csv
start,end,kind,name,notes
$038FD0,$03958F,palette,PaletteBank,"Main bank: 46 contiguous palettes..."
```

Two things follow:

- `tools/trace_code.py` takes those bytes out of the `unknown` bucket and
  classifies them in `rom-map.csv`;
- `tools/emit_asm.py` emits the range with a **label and `dc.w`** instead of
  anonymous `dc.b`, one palette per line: a `PaletteBank` label followed by one
  `dc.w` line of 16 colour words for each palette, with a comment numbering it.

Same bytes — the byte-identical gate stays green. What changes is that the range
stops being opaque.

## Compression: the graphics are LZSS

Yes, they are compressed. The game has **two variants** of the same algorithm,
both using a 1024-byte ring buffer at `$FF8800` (`LzRingBuffer`).

Full format details, including the third codec (`RleUnpack`), are in
[25-codecs.md](25-codecs.md).

### `LzDecompressA` (`$001E3A`)

Called by `VdpUploadEntry`. Ring zeroed, initial write position `$3DF`, mask
`$3FF`.

The stream alternates a control byte (8 bits, least to most significant) and the
tokens those bits describe. Bit 0 is a literal. Bit 1 reads a byte `b` that
selects among three forms — and this is where the format departs from textbook
LZSS:

| `b` | Meaning |
|---|---|
| `$1F` | end of stream |
| `< $80` | copy from an **absolute** ring position: reads one more byte, `offset = ((b << 3) & $300) \| byte` (10 bits), `length = (b & $1F) + 3` |
| `$80`–`$C0` | copy **relative** to the write position: `distance = b & $F`, `length = ((b >> 4) & 3) + 2`. The source is recomputed per byte, so the copy slides and becomes RLE when the distance is small |
| `> $C0` | a block of `(b & $3F) + 8` literals straight from the stream, without spending a control bit per byte |

### `LzDecompressB` (`$001DAC`)

29 callers. Same algorithm, three differences: the ring is filled with `$20`
instead of zero, the initial position is `$3C0`, and the end is given by
**length** (read a word, add to `a1`, stop when `a5` reaches `a1`) instead of a
token.

Filling the ring with spaces is the classic Okumura LZSS initialisation — that
detail is what gave the algorithm away.

### Verification

`tools/lzss.py` implements variant A. The stream at `$03E860` (pointed at by
an absolute `lea` of `$03E85E` into `a0` before the call to `VdpUpload` at
`$01C736`) produces:

```
input : $03E860, 955 bytes
output: 4768 bytes  (ratio 4.99x)
tiles : 149.0 of 32 bytes (4bpp 8x8)
```

**4768 is exactly 149 × 32.** Landing on an exact multiple of the tile size does
not happen by chance, and rendering the output as 4bpp confirms it: coherent
shapes with clean outlines, not noise.

### A name that was wrong

`$FF8800` had been named `TextBuffer` and `$001DAC` `ClearTextBuffer`, because I
had read only the loop filling the area with `$20` bytes and concluded "text
buffer". That was wrong: it is the decompressor's ring, and the `$20` is the LZSS
initialisation. The code continued below and I had not read it.

Lesson for future names: an initialisation loop does not tell you what an area is
for. You have to read whoever **uses** it.

## Stream catalogue

`tools/find_streams.py` finds the streams automatically. The caller always loads
the pointer with an absolute `lea` a few instructions before the call. A typical
site loads the stream address (`$047C42`) into `a5` and the destination
(`$FFFF4000`) into `a6`, then calls `LzDecompressB`.

So it is enough to follow the call sites and look backwards to the `lea` that
loads the right register — the same technique `trace_code.py` uses for jump
tables. **Every stream found is actually decompressed**, not merely recorded.

Result over 82 call sites:

| | |
|---|---|
| Distinct streams decompressed | **52** |
| Decompression failures | **0** |
| Output a multiple of 32 bytes (tiles) | 51 of 52 |
| Unresolved pointers | 5 |
| Compressed bytes | 79,976 (7.6% of the ROM) |
| Decompressed bytes | 165,760 |
| Tiles | **5,180** |

Output in `analysis/compressed-streams.csv`.

The only stream whose output is not a multiple of 32 is `$004254`, at 1,830
bytes — probably a tilemap.

### How I know it is right

Three independent checks:

1. **Zero failures** across 52 streams. A misread format breaks on the first
   unexpected token.
2. **51 of 52 outputs land on an exact multiple of 32 bytes**, the 4bpp tile size.
3. **Colours per tile between 2.5 and 8**, measured over the output. Real
   graphics use few colours per tile; noise would use close to 14 of the 16
   possible. The only stream giving 12.1 is precisely `$004254`, which is not
   tiles.

Rendering confirms it: character sprites in beige tones on a cyan background, the
colour used as transparency.

A trap here: assembling all 5,180 tiles into a single sheet, with one palette and
8 pixels per tile, **looks like noise**. It is not — it is too small a scale and
the wrong palette for most streams. It was the colours-per-tile measurement that
settled the question, not the eye.

## Scanning through pointer tables

Not every stream is reached by a direct `lea` — many are loaded from tables
indexed by stage or character. `tools/find_streams.py --scan` finds those: it
looks for runs of 4 or more consecutive longs that look like ROM addresses and
**tries to decompress each target**. The decompressor becomes the validator.

### The naive criterion is not enough

"Output a multiple of 32 bytes" on its own approves 1 in 32 streams by chance.
Run with only that, the scan approved 85 targets — including `$000400`, inside
the boot area, "decompressing" 25 KB into 220 KB. Garbage.

Three filters together solve it, and rejected 549 of 603 candidates:

| Filter | Why |
|---|---|
| compression ratio between 1.3× and 6× | 8.6× is fantasy |
| the consumed range collides with neither code nor already-declared data | a real stream does not invade code |
| **≤ 9 colours per tile** | real 4bpp graphics sit between 2.5 and 8; noise uses close to 14 |

The third does the heavy lifting.

### Result

| | Via direct `lea` | Via pointer table | Total |
|---|---|---|---|
| Streams | 52 | 53 | **105** |
| Compressed bytes | 79,976 | 167,935 | 247,911 |
| Tiles | 5,180 | 14,196 | **19,376** |

That put the ROM at **23.6% classified as compressed graphics**.

The streams found by scanning sit almost entirely in a contiguous band
`$049000`–`$05Exxx`, with round tile counts (256, 176, 80) — the shape of a stage
art bank.

### The confirmation that needs no metric

Rendering the stream at `$05041C` (256 tiles), you can read **"ACME LAND"** in the
middle of the image. There is no better metric than legible text.
