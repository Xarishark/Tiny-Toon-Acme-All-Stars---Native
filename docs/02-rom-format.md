# 02 — ROM format

Everything here is verifiable by running `python tools/rom_info.py --vectors`.

## Identity

| Field | Value |
|---|---|
| Size | 1,048,576 bytes (1 MB), raw `.bin` (not interleaved) |
| Console | `SEGA MEGA DRIVE` |
| Copyright | `(C)T-95 1994.SEP` (T-95 = Konami) |
| Domestic title | *(blank)* |
| Overseas title | `TINY TOON ADVENTURES ACME ALL STARS` |
| Serial | `GM T-95146-00` |
| Region | `E` (Europe) |
| I/O | `J4` — joypad, 4 players / Team Player |
| ROM range | `$000000`–`$0FFFFF` |
| RAM range | `$FF0000`–`$FFFFFF` |
| SRAM | none |
| Header checksum | `$E511` — matches the word sum from `$200` to the end |
| MD5 | `abddd42b2548e9b708991f689d726c9a` |
| SHA-1 | `2672018d9e005a9a3b5006fa8f61e08f2d1909aa` |

Frozen in `analysis/rom-baseline.json`; `tools/rom_info.py --check` validates
against it.

The `J4` field is **false**, and the code proves it — see
[09-input.md](09-input.md). It is metadata the developer fills in, and here it
does not match what the game does.

## Layout

| Range | Contents |
|---|---|
| `$000000`–`$0000FF` | the 68000's 64-vector table |
| `$000100`–`$0001FF` | cartridge header |
| `$000200`–`$0FFFFF` | code and data (the area covered by the checksum) |

## Vectors

Initial SP `$FFFFFF00`, entry point `$000206`.

Of the 64 vectors, nearly all point at two shared stubs:

- `$000200` — `nop` / `bra *` (hang): 39 vectors, all exception and reserved.
- `$000204` — `rte`: 20 vectors, including the 16 `TRAP`s and IRQ levels 1, 3, 5
  and 7.

Only three vectors have their own destination:

| Vector | Destination | Real or stub |
|---|---|---|
| level 2 (external) | `$00041C` | `rte` — stub |
| level 4 (H-blank) | `$00041E` | `rte` — stub |
| level 6 (V-blank) | `$000420` | **real handler**, 31 instructions |

So the game handles **only V-blank**. There is no H-blank and no external
interrupt, which considerably simplifies the timing model the native runtime
needs: one tick per frame.

## Boot

The entry point begins with Sega's standard hardware initialisation — version/TMSS
probe by testing the long at the I/O version register `$A10008`, writing `'SEGA'` to `$A14000`, clearing RAM/VRAM,
setting up the Z80. Nothing game-specific until `$000294`.

## The central problem: indirect dispatch

A recursive scan from the vectors covers very little of the ROM on its own,
because control flow goes through tables. Measured progression:

| Knowledge applied | Instructions | Functions |
|---|---|---|
| vectors only | 1,165 | 50 |
| + 3 jump tables | 3,457 | 162 |
| + dispatcher `$0005CE` | 5,173 | 270 |
| + `offset16` format at the `jmp` site | 6,631 | 383 |
| + automatic idiom recognition | 13,468 | 783 |
| + automatic pointer tables | 19,572 | 1,130 |
| + hand-verified tables | **27,690** | **~1,570** |

Each table resolved exposes new tables. To reproduce the bottom line:
`python tools/trace_code.py`; the top one: `--no-tables`.

## Jump table taxonomy

The game uses three formats. Telling them apart is what makes coverage grow.

### 1. `pointer` — absolute 32-bit longs

```
movea.l BASE(pc, dN.w), aN
jmp     (aN)                  ; or jsr (aN)
```

Each entry is a 4-byte absolute address. E.g. `$0004B6` (16 game states),
`$00154A` (5 VDP opcodes), `$007410` (53 object behaviours).

### 2. `offset16` — offset words relative to the base

```
move.w  BASE(pc, dN.w), dN    ; load the offset from the table itself
jmp     BASE(pc, dN.w)        ; jump to BASE + offset
```

Entries are **signed** words relative to the base. A negative offset is
legitimate: it points to a shared handler placed before the table. This is the
most common format in the ROM.

The same table also appears **inline after a `jsr`** to the generic dispatcher
`$0005CE` — see below.

### 3. `inline` — the entries *are* code

```
jmp     BASE(pc, dN.w)        ; with no move.w before it
```

Without the `move.w` loading the offset, the jump lands directly on the entry,
which is 4 bytes of code (typically `bra.w` or `nop`/`rts`). E.g. `$00077E` (8
peripherals), `$0009E4` (3 entries, with an explicit bound check
just before that compares the index in `d0` against 2).

## How to discover how many entries a table has

This is the hard problem — the ROM stores the size nowhere.

**Geometric rule (automatic).** The table bounds itself: since entries point at
code that comes *after* it, the end of the table can never pass the smallest
target. Grow `n` while `base + stride·n` fits before the smallest target seen so
far. Implemented in `Trace._offset16_extent` and `Trace._pointer_extent`.

A simpler and tempting rule — `count = first_offset / 2`, assuming entry [0]
points just past the table — holds for several tables and **fails** for others.
Counterexample: at `$0204D6` entries [1] and [2] point at `jmp` instructions
placed just after the table, not at its end.

**When the geometric rule is not enough.** If every entry points far away,
nothing limits growth and the count comes out too large. Then the terminator has
to be recognised by hand, and the signals that worked were:

- **code begins immediately after the table** — `$01C71E` ends at 4 entries
  because `$01C726` decodes as coherent code (a clear of the RAM word at `$FFE024`
  followed by an absolute call to `$0014A0`);
- **unused slots point at a shared `rts`** — `$00DB8A` has 24 of its 30 slots
  pointing at the same `rts` at `$00DB88`; `$007410` likewise, at `$00740E`;
- **regular runs of absolute-long `jmp` instructions** spaced 6 bytes apart, as at `$0074E4`;
- **the table ends where another begins** — `$00DB8A` ends exactly at `$00DBC6`,
  the base of the next table;
- **a `bra.w` just after the site jumping into what was assumed to be table** —
  at `$01577A` that proves `$01578C` is already code, closing the table at 3
  entries;
- **an entry that would fall inside the table itself** — at `$008C04`, 13 entries
  would make entry [0] land inside its own table, so there are 12.

## Validation: all or nothing

Every table is validated before any target is seeded. Each entry must be inside
the ROM, at an even address, decode as a valid 68000 instruction, and not fall
inside the table itself. **If one entry fails, the whole table is rejected** and
the site goes to `unresolved-flow.csv`.

Accepting the prefix up to the first invalid entry was tried and is worse: the
entries before the failure are not necessarily good. In practice, two such tables
produced *all* the invalid decodes and *all* the overlaps in the scan. With the
all-or-nothing rule, the current scan has zero of both — and those two metrics
are the contamination alarm: any non-zero value means some declared or recognised
table is wrong.

## Automatic recognition

The `offset16` and `pointer` idioms are recognised by the tracer, without manual
declaration. The required signature is strict: within the previous 4
instructions there must be a loader with the *same* table base and a destination
in the *same* register used by the jump, with nothing in between overwriting that
register. The 4-instruction slack exists because there are real cases with
instructions in between — at `$00DB70` there is a register exchange of `a0` and
`a1` between the `move.w` and the `jsr`.

This is not guessing: the count comes from the geometric rule and every target
passes the validation above. Tables recognised this way are written to
`analysis/jump-tables-detected.csv` (output, regenerable) for human review; the
hand-verified ones live in `analysis/jump-tables.csv` (input) and take precedence
over the automatic count.

### Notable jump tables

**`$0004B0` — the game's main state dispatcher**

It loads the state index from the RAM word at `$FFFB04` into `d0`, multiplies it
by four with two additions, loads a pointer from a PC-relative table of 16
four-byte entries at `$0004B6` indexed by `d0` into `a1`, and jumps through `a1`.

16 states. The table ends at `$0004F5`, and entry [8] points at `$0004F6` — the
first byte just after the table, which confirms the count. `$FFFB04` is the game
state variable; see [03-memory-map.md](03-memory-map.md).

**`$001542` — VDP command list interpreter**

It points `a5` at the command stream at `$FF9000`, `a6` at `$FF9200` and `a3` at
the VDP control port `$C00004`, sets the VDP auto-increment to 2 (register `$0F`),
then reads an opcode word from the stream (post-incrementing `a5`), multiplies it
by four, loads a handler pointer from the PC-relative table at `$00154A` into
`a4` and calls it with `jsr`.

5 opcodes. Same self-terminating property: entry [0] points at `$00155E`, the
first byte after the table.

### The generic dispatcher `$0005CE`

Registered in [`analysis/dispatchers.csv`](../analysis/dispatchers.csv). It is
the most-used idiom in the game and the reason a naive scan dies early:

Five instructions, `$0005CE`–`$0005DA`: it doubles the index in `d0` (a shift
left by one) and masks it to the low byte (`$FF`) at `$0005D0`; at `$0005D4` it
pops the return address off the stack into `a0` — that address is the table base;
at `$0005D6` it loads the word at `a0 + d0` into `d1`; and at `$0005DA` it jumps to
`a0 + d1`.

Called with an absolute `jsr` to `$0005CE` with an `offset16` table **inline right after the `jsr`**.
Consequences:

- the `jsr` **does not return** to the following instruction — the bytes that
  follow are the table;
- the entry ceiling is 256, from the `$FF` mask at `$0005D0`;
- the indexed `jmp` through `a0` at `$0005DA` is indirect by construction. What resolves it
  is each call site's table, not itself, so it is declared as a `jump_site` in
  `dispatchers.csv` and does not count as pending.

The typical caller loads the word at `$FFFB06` into `d0` and then calls
`$0005CE`, meaning `$FFFB06` is
the **sub-state** index within each main state.

## Indirect flow: zero open sites

From 11 to 0. What resolved each group:

**The four indirect jumps through `a2`** (`$001E96`, `$001EC8`, `$001EFA`, `$001F12`) were never
unknown: they are **inside the decompressor** `LzDecompressA`, and `a2` is loaded
once at `$001E76` with the address `$001E7C` — it is the loop-back. They stayed on the
list because the tracer had no way to express "indirect jump with a single
destination". The `fixed` kind in `jump-tables.csv` was created for that.

**The two rejected tables** had the same cause: the geometric rule overestimated
the count. Verified by hand, `$0088AE` has 12 entries (entry [12] is already the
first word of an instruction) and `$0271B6` has 7. Together they unlocked ~5.2 KB of orphan
code.

**`$020BCA`** uses the same 53-behaviour object table at `$007410` already known,
only loaded into `a1` by `lea` instead of PC-relative access.

**`$02961E`** is the `offset16` idiom with an address-register base
(`a6` is loaded with the absolute address `$029722`, the offset word is read from
`a6 + d0`, and the call goes to `a6 + d0`) — which is why the
automatic detector, which requires a PC-relative base, did not catch it.

**`$020C08`** was the last one, and the game found it: an indexed `jsr` through `a1` + `d0`, where `a1`
comes from the caller, outside the look-back window. It stayed harmless until
somebody finished the racing stage and the deliberate abort fired. Two callers
supply two different bases, and each table's extent was proven independently —
see [18-recompilation.md](18-recompilation.md). Registering both added **1,076
previously unreachable instructions**.

## Orphan code

`$01EAE0`–`$01F273`, 1,940 bytes, decodes cleanly from start to end: clear
`$FF0000` and `$FF2000`, write VDP register `$0B` from a table at `$01EB16`, and
16 calls to a VRAM filler at `$01ED26`.

Nothing reaches it. The only absolute call entering the region comes **from
inside it** (`$01ECCA`), so the island is closed — and the port has executed over
1.4 million frames without ever dispatching outside a table. It is recorded in
`data-regions.csv` as `codigo_orfao`: probably a test or reset routine left in
the ROM.

**One limitation of the gate is worth recording:** the byte-identical rebuild
catches *encoding* errors, not *classification* errors. If a stretch of data were
seeded as code and happened to decode as valid instructions, it would re-encode
to the same bytes and the gate would stay green. What protects against that are
the tracer's overlap and invalid-decode metrics, which remain at zero.
