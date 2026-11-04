# 03 — Memory map

Two different things: the **console's** map (fixed, documented by Sega) and the
**game's** map inside work RAM (discovered by analysis, still incomplete).

## Console map (as the 68000 sees it)

Encoded in `MEMORY_REGIONS`, in [`tools/romlib.py`](../tools/romlib.py).

| Range | Region |
|---|---|
| `$000000`–`$3FFFFF` | cartridge ROM (this game uses `$000000`–`$0FFFFF`) |
| `$A00000`–`$A0FFFF` | Z80 address space (Z80 RAM, YM2612) |
| `$A10000`–`$A1001F` | I/O: version register and controller ports |
| `$A11100` | Z80 BUSREQ |
| `$A11200` | Z80 RESET |
| `$A14000` | TMSS register |
| `$C00000`–`$C0001F` | VDP ports (data, control, HV counter, PSG) |
| `$FF0000`–`$FFFFFF` | work RAM, 64 KB |

## Hardware actually touched

Extracted from `analysis/symbols.csv` (`hw_*` rows), counting references in
already-traced code:

| Address | Symbol | Refs |
|---|---|---|
| `$C00004` | `VDP_CTRL` | 94 |
| `$A11100` | `Z80_BUSREQ` | 31 |
| `$C00000` | `VDP_DATA` | 12 |
| `$A10001` | `IO_VERSION` (high byte) | 4 |
| `$A11200` | `Z80_RESET` | 2 |
| `$A10003`, `$A10008`–`$A1000D` | joypad ports | 1 each |

No direct write to the YM2612 (`$A04000`+) has appeared in traced code —
consistent with audio being driven by the Z80. The weight of `Z80_BUSREQ` (31
references) shows the usual pattern: the 68000 requests the bus to write
commands into Z80 RAM and gives it back.

## Game RAM — what is known

From `analysis/ram-map.csv`, generated from every absolute-addressing operand
pointing into `$FF0000`–`$FFFFFF`. **407 distinct addresses** so far.

### Block `$FFA6xx` — the hottest region of RAM

Eleven of the twelve most-referenced addresses in the traced ROM sit between
`$FFA604` and `$FFA66C`, always with `cmp`/`cmpi`/`add`/`clr` (fields read and
compared, not buffer bases):

| Address | Size | Refs |
|---|---|---|
| `$FFA622` | long/word | 200 |
| `$FFA604` | long/word | 133 |
| `$FFA608` | long/word | 126 |
| `$FFA66C` | word | 119 |
| `$FFA60C` | long/word | 101 |
| `$FFA62A`, `$FFA618`, `$FFA658` | word | 81–90 |

This is the game's central struct — probably match/player state. Naming it field
by field is likely the highest-return step left in RAM analysis.

### Named variables

`analysis/ram-names.csv` is an **input** file, versioned, with mandatory
evidence — same rules as `symbol-names.csv`. The names appear in the emitted
disassembly (as `EQU` at the top of the source, with the sign-extended value the
68000 uses for short absolute addressing) and in the `name` column of
`ram-map.csv`. A name never changes a byte: the byte-identical gate stays green
afterwards.

| Address | Name | Evidence |
|---|---|---|
| `$FFFB00` | `FrameCounter` | incremented by one once per V-blank, and nowhere else |
| `$FFFB02` | `RandomSeed` | LCG seed; written by a single instruction in the whole ROM, read directly by 75 sites |
| `$FFFB04` | `GameState` | indexes the 16-pointer table; confirmed in the emulator |
| `$FFFB06` | `SubState` | feeds `SubStateDispatch` and the `offset16` tables; cleared on every state change |
| `$FFFB08` | `StateDone` | cleared by `SetGameState`; handlers only advance when non-zero |
| `$FFFB1E` | `VBlankDivider` | decremented by one per frame, reloads with 4 — an event every 5 frames |
| `$FFFBE7` | `VBlankBusy` | `st.b` on V-blank entry, `clr.b` on exit; reentrancy guard |
| `$FFEC00` | `VScrollBuffer` | long written to VSRAM `$0` during V-blank — vertical scroll for planes A and B |
| `$FF9000` | `VdpCmdStream` | stream consumed by the interpreter at `$001542` |
| `$FF8800` | `TextBuffer` | `ClearTextBuffer` fills 960 bytes with the space tile |
| `$FFD410` | `SoundQueue` | 256-byte ring of sound requests; producer `PlaySound`, consumer `SoundQueueDrain` |

In the emitted source these names make the V-blank handler (`$000420`) read
directly: it saves all data and address registers on the stack, tests
`VBlankBusy` and branches to the exit at `$0004A0` if it is already set, otherwise
sets it and increments `FrameCounter`; further on it copies `VScrollBuffer` to the
VDP data port; and near the end it decrements `VBlankDivider`, branching to
`$00047C` while it is not negative and reloading it with 4 when it is.

The cluster at `$FFFB00`–`$FFFBFF` is the block of global flow-control
variables, and most of the names above come from it. That `$FFFB06` (`SubState`)
is the most-referenced variable in the traced ROM is coherent: almost every state
routine begins by reading the sub-state.

Two still unnamed, but heavy: `$FFE020` (135 references, compared with `cmpi`)
and `$FFE00A` (113, an `add`/`and`/`clr` accumulator).

### Structured buffers

Detected by `lea` with an absolute address — loading the base address of an area
is the buffer access pattern:

| Base | Indication |
|---|---|
| `$FF9000` | **VDP command stream**, consumed by the interpreter at `$001542` |
| `$FF9200` | second stream, loaded together with `$FF9000` |
| `$FFA000` | base of the `$FFA6xx` struct |
| `$FFC000`, `$FFEC00` | large buffers |
| `$FF8000`, `$FF8800`, `$FF4000`, `$FF6000` | probable tile/sprite buffers |

## Read/write heuristic

The `access` column in `ram-map.csv` (`r`, `w`, `rw`) comes from a heuristic
documented in `Disassembler._access`, in [`tools/m68k.py`](../tools/m68k.py):

- the destination is the **last operand**, by 68000 convention;
- instructions in `WRITES_LAST_OPERAND` write to the last operand;
- instructions in `READ_MODIFY_WRITE` (`add`, `and`, `asl`, `bset`, `neg`, …)
  are marked `rw`;
- single-operand instructions in `WRITES_SINGLE_OPERAND` (`clr`, `not`, `tas`, …)
  write to it;
- everything else is a read.

**It is not exact.** `movem` with pre-decrement, `exg` and some `movep` cases do
not follow the rule. Treat `access` as a hint for prioritising investigation, not
as a proven fact. The `refs`, `sizes` and `first_ref` columns *are* exact — they
come straight from the disassembly.

## Still open

- **`$FFA600`–`$FFA6FF` is the highest-return target**: it is the hottest struct
  in the ROM and does not have a single named field. `$FFA622` alone has 200
  references.
- 13 of 407 addresses named. `$FFFB00`–`$FFFBFF` is well covered; the rest is
  not.
- VRAM/CRAM/VSRAM layout has not been touched — it depends on decoding the
  commands written to `VDP_CTRL` and the stream format at `$FF9000`.
