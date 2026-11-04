# 27 — Mods

> Drop a shared library into `mods/` beside the executable. No rebuild of the
> game, no patching the binary. The whole contract is one header,
> [`runtime/mod.h`](../runtime/mod.h).

## Using a mod

```
tinytoon.exe
mods/
  filtro-cor.dll
rom.bin
```

That is the entire installation procedure. On startup the game scans `mods/`,
loads what it finds, and reports each one:

```
[mod filtro-cor] ramp 'gameboy', 4 levels
[mods] filtro-cor 1.0 by exemplo — Repaints the frame through a four-tone colour ramp
[mods] 1 loaded. A modded session is NOT a clean session — say so in any bug report.
```

That last line is not decoration. A mod changes what you see, and a bug report
from a modded session that does not mention the mod costs somebody an afternoon.

**Failure is always reported.** A library that does not open, declares an ABI
this build does not speak, or declines in `md_mod_iniciar` says so with the
reason. A mod that fails silently is worse than one that does not exist: you go
on believing the filter is on and that the *game* is what looks strange.

The directory searched is `mods/` **beside the executable**, not beside your
current directory. The two differ for shortcuts, "open with" and drag-and-drop,
and a mod that only loads when you launch from a terminal would be worse than no
mod at all.

## Writing one

A mod is a shared library exporting up to five functions. Only the first is
required.

```c
#include "mod.h"

static MdModInfo INFO = {
    MD_MOD_ABI, "meu-mod", "1.0", "you", "what it does"
};

MD_MOD_EXPORT MdModInfo *md_mod_info(void) { return &INFO; }

MD_MOD_EXPORT void md_mod_pixels(unsigned char *rgb, int w, int h) {
    for (long p = 0; p < (long)w * h * 3; p += 3) rgb[p] = 255;   /* all red */
}
```

That is a complete mod. Build it, drop it in, done.

| hook | when | for |
|---|---|---|
| `md_mod_info` | first, at load | identify yourself. **Required.** |
| `md_mod_iniciar(host)` | once, after the ROM is verified | keep the host pointer, read config. Return non-zero to decline. |
| `md_mod_quadro()` | once per **game** frame | read game state |
| `md_mod_pixels(rgb, w, h)` | once per **presented** frame | rewrite the finished image |
| `md_mod_encerrar()` | at exit | clean up |

The two frame hooks fire at different rates, and mixing them up is the first
mistake to make. During fast-forward the game runs about eleven times ahead of
the display: `md_mod_quadro` sees every frame, `md_mod_pixels` only the ones
actually shown. Read state in the first; paint in the second.

### The one design decision everything follows from

**A mod never links against the executable.** Every service arrives as a
function pointer in `MdModHost`, handed over at load:

```c
typedef struct {
    unsigned abi;
    void (*log)(const char *fmt, ...);
    unsigned long (*quadro)(void);
    const uint8_t  *(*ram)(void);    /* 64 KB, index 0 is $FF0000 */
    const uint8_t  *(*vram)(void);
    const uint16_t *(*cram)(void);
    const uint8_t  *(*regs)(void);
    void (*tela)(int *largura, int *altura);
} MdModHost;
```

Nothing is resolved by symbol name across the boundary, and that is not
stylistic. The release build is statically linked and passes through `-s`: there
is no symbol table for anyone to link against. A mod that called `md_frames()`
directly would build fine and then fail to load against the executable people
actually download.

The side effect is good: the boundary is explicit. What is in `MdModHost` is a
contract; everything else in the runtime can be rewritten without telling
anybody.

### Reading game state

`ram()` is the game's 64 KB of work RAM, index 0 being `$FF0000`. So the address
`$FFA622` — the most-referenced variable in the whole ROM — is `ram()[0xA622]`.

What is known about that memory is in [03-memory-map.md](03-memory-map.md), and
`analysis/ram-names.csv` carries the named addresses with the evidence for each.
Some useful ones today:

| | |
|---|---|
| `ram()[0xFB00]` | frame counter, word |
| `ram()[0xFB04]` | game state — which screen you are on |
| `ram()[0xFB06]` | sub-state within that screen |
| `ram()[0xA000]` | object array, 84 slots of `$80` bytes |

`cram()` is the 64 colour words, four palettes of sixteen. A colour is
`0000 BBB0 GGG0 RRR0`, three bits per channel, and the DAC levels are **not
linear** — they are `{0, 52, 87, 116, 144, 172, 206, 255}`. Using `level × 36`
gives an image that looks close and is wrong in the dark tones.

### What mods cannot do, and why

**They cannot write game state.** That is deliberate:

- A mod that only reads and repaints cannot change the game's behaviour, so a
  modded session and a clean one still play identically. The project's rule is
  reproduce faithfully first; this keeps that intact while the interesting parts
  get built.
- Writing RAM breaks replay determinism, and replays are this project's
  regression tests. A hook that silently invalidates them would be a bad trade
  for a first version.

Write access is the obvious next step. `MdModInfo` and `MdModHost` both carry an
ABI number so it can be added without breaking anything compiled against this
one — that is what the number is for.

## Building

```bash
cd mods
make                                          # for the host
make WINDOWS=1 CC=x86_64-w64-mingw32-gcc      # cross-compile to .dll
```

Any subdirectory containing `.c` files is a mod and gets built. There is no list
to keep in sync.

## Testing without a screen

The pixel hook lives in the presentation path, which only exists with SDL. Left
at that, a filter would only be testable by somebody watching a monitor — and a
mod you cannot test in automation is a mod nobody can verify.

So a headless run writes a **second** PNG:

| | |
|---|---|
| `tela.png` | the clean frame, straight from VDP state |
| `tela-mod.png` | the same frame with every mod applied |

The difference between the two *is* the mod's effect, measurable byte by byte.
`tela.png` stays clean on purpose: it exists to debug the renderer, and debugging
a renderer through a filter would be absurd.

```bash
./tinytoon rom.bin 0 1800
cmp tela.png tela-mod.png && echo "the mod did nothing"
```

## The example

`mods/filtro-cor/` repaints the frame through a four-tone ramp. It is the
smallest useful mod and touches every hook that matters.

```bash
TINYTOON_FILTRO=gameboy ./tinytoon rom.bin    # four greens (default)
TINYTOON_FILTRO=ambar   ./tinytoon rom.bin    # amber monitor
TINYTOON_FILTRO=cinza   ./tinytoon rom.bin
TINYTOON_FILTRO=sepia   ./tinytoon rom.bin
```

Two decisions in it are worth copying:

**The effect is unmistakable.** A filter you have to squint at is a bad test.
Four shades of green tell you it loaded from across the room. A test whose
result is ambiguous is one you end up debugging instead of the thing it was
supposed to test.

**A bad configuration value is reported, not fatal.** An unknown ramp name logs
the mistake and the valid options, then carries on with the default. Somebody
who mistyped a ramp still wants to play.

It also shows a small thing that matters in a per-pixel loop: luminance uses
Rec.601 weights (`77/150/29`) rather than a third each, because the eye is far
more sensitive to green, and the mapping from luminance to ramp index is a
256-entry table built once rather than a division 71,680 times per frame.

## Ideas that fit this ABI today

- **HUD overlays** — read the object array in `md_mod_quadro`, draw in
  `md_mod_pixels`. Hitboxes, coordinates, AI state.
- **Palette experiments** — read `cram()` and remap.
- **Telemetry** — log game state to a file for analysis. This is how you would
  work out what the unnamed fields of `$FFA6xx` mean.
- **Scanlines, CRT masks, colour-blindness filters** — all pure pixel work.
- **A screenshot trigger** on a game state change.

What does not fit yet is anything that changes the game: cheats, physics tweaks,
new behaviour. Those need write access, which is the next ABI version.
