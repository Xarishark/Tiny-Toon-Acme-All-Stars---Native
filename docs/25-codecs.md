# 25 — The three codecs, and why "it decoded" is not evidence

> `tools/codecs.py` reimplements the ROM's three decompressors in Python.
> `tools/codecdiff.py` checks them against the running game: **63 runs, all
> matching byte for byte**. Without that gate the reimplementation would be
> worth nothing, and the reason is measured.

## The measurement that changed the plan

The first idea was good and it was wrong: if an unknown block decodes cleanly as
LZ and ends exactly where the next block begins, that would prove it is a
compressed stream. I implemented it, ran it, and gained 7,683 bytes of new
classification.

Then I measured the false-positive rate — decoding from random offsets into the
ROM:

| codec | isolated offset | chain of 3 |
|---|---|---|
| **LzDecompressA** | **99.5%** | **97.2%** |
| LzDecompressB | 20.5% | 2.0% |

LzA accepts practically anything. It has no checksum, and its `$1F` end token
occurs by chance all the time. The 7,683 bytes were discarded: they were exactly
the kind of guess the project's rule forbids recording as fact.

LzB is far stricter because it carries an explicit length field, and the length
has to land right.

**What survives as evidence** is something else: the streams **tiling** the range
the game read. Decoding from the start of the region and landing, stream after
stream, on the last byte the read map recorded — not one more, not one less.
77 regions do that, covering 209,576 compressed bytes.

## The formats

A 1024-byte ring at `$FFFF8800`, shared by both LZ codecs, with different
initialisations:

| | ring | initial position |
|---|---|---|
| `LzDecompressA` | 1024 bytes zeroed | `$3DF` |
| `LzDecompressB` | 960 bytes of `$20` | `$3C0` |

### LzDecompressB (`$001DAC`) — length-delimited

The first word is how many stream bytes follow it. Then flag bits in groups of
8, least significant first:

- **bit 1** → a 1-byte literal
- **bit 0** → two words: `cccccc oooooooooo`, 6 bits of length (+1) and 10 bits
  of **absolute** offset into the ring

### LzDecompressA (`$001E3A`) — end-token delimited

Same flag scheme, but the command is selected by the byte's range:

| byte | meaning |
|---|---|
| flag 0 | 1-byte literal |
| `$00`–`$1E` | ring copy, 10-bit absolute offset, length `(b & $1F) + 3` |
| `$1F` | **end of stream** |
| `$80`–`$C0` | relative copy, distance `b & $0F`, length `((b >> 4) & 3) + 2` |
| `$C1`–`$FF` | literal run of `(b & $3F) + 8` bytes |

### RleUnpack (`$01AD02`) — destination-delimited

Words: a negative word is a literal run of `w - $8000` words; a positive one is
`high byte` copies of the word formed by replicating the `low byte`. It stops
when the destination buffer fills — and the length comes from the caller,
`d1 * 32` bytes into a 2 KB slot at `$FF0000 + $800 * d0`.

**That is why it is the only one of the three you cannot recognise on its own in
the ROM**: an RLE stream does not say where it ends.

Three variants, masking each written word according to the slot: no mask for
`d0 < 3`, a mask of $BBBB for `3 ≤ d0 < 6`, a mask of $7777 for `d0 ≥ 6`. The first two
appear in the ground truth (24 and 15 runs).

## The gate

Consuming the same bytes **does not prove** producing the same bytes. The ground
truth comes from the game itself: with `TINYTOON_CODECS=1` the runtime records,
for each codec run, where it read from and what it wrote.

One trap sat in the way: both LZ codecs write into the ring using the sibling
instruction of the one that writes the output, and **both go through
`md_write8`**. Without excluding the ring range (`$FFFF8800`–`$FFFF8BFF`), the
ground truth would come out with output and ring interleaved byte by byte and
the comparison would never close.

```bash
tools/codecdiff.sh          # capture and compare
```

```
63 comparable runs (1 truncated by the buffer, ignored)
  LzDecompressA      12 match,   0 differ
  LzDecompressB      12 match,   0 differ
  RleUnpack          39 match,   0 differ
ALL MATCH byte for byte against the game.
```

It is the project's fifth independent-reference gate, alongside `vasm` against
the ROM, Musashi against the generated C, Nuked-OPN2 against the FM synthesis
and published hardware CRCs against the Z80.

## Extraction

```bash
python tools/extrair.py --gravar
```

```
confirmed regions (exact tiling): 77
blocks extracted: 77
compressed in ROM: 209576 bytes
decompressed:      397769 bytes (ratio 1.90x)
  lza: 66 blocks, smallest 49, largest 8192, mean 4772
  lzb: 11 blocks, smallest 1600, largest 8192, mean 7525
```

It writes one `.bin` per block and a PNG tile sheet beside it. The sheet is a
grey ramp by index, not colour: without the right palette colour means nothing,
and the question the sheet has to answer is "is this art or is this a table?".
`lza-040842-000.png` answers on its own — those are sprites, recognisable by eye.

Only confirmed regions are extracted. Extracting whatever "decoded" would fill
`build/` with plausible garbage, which is worse than having nothing: plausible
garbage is what nobody reviews.
