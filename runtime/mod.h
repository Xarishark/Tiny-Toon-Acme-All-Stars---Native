/* The mod ABI.
 *
 * This is the only header a mod needs. Copy it, include it, implement the hooks
 * you care about, build a shared library, drop it in `mods/`. No rebuild of the
 * game, no patching of the executable.
 *
 * THE ONE DESIGN DECISION EVERYTHING ELSE FOLLOWS FROM:
 *
 *   A mod never links against the executable.
 *
 * Every service the runtime offers arrives as a function pointer in MdModHost,
 * handed to the mod at load time. Nothing is resolved by symbol name across the
 * boundary. That is what makes mods work against a statically linked, stripped,
 * single-file release build — where there are no exported symbols to link to at
 * all. It also means a mod compiled today keeps working when the runtime's
 * internals are rewritten, as long as the ABI number below is unchanged.
 *
 * WHAT MODS CAN AND CANNOT DO, IN THIS VERSION
 *
 * They can read game state and rewrite the finished image. They cannot write
 * game state. That is deliberate, not an oversight:
 *
 *   - A mod that only reads and repaints cannot change the game's behaviour, so
 *     a modded session and an unmodded one still play identically. The
 *     project's rule is reproduce faithfully first; this keeps that intact
 *     while the interesting parts get built.
 *   - Writing RAM breaks replay determinism. Replays are the project's
 *     regression tests, and a hook that silently invalidates them would be a
 *     bad trade for a first version.
 *
 * Write access is the obvious next step and the ABI has a version field so it
 * can be added without breaking anything compiled against this one.
 */

#ifndef TINYTOON_MOD_H
#define TINYTOON_MOD_H

#include <stdint.h>

/* Bumped when the structs below change shape. The runtime refuses to load a mod
 * whose number it does not know, rather than reading a struct with a different
 * layout and producing a crash nobody can explain. */
#define MD_MOD_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ mod → runtime */

typedef struct {
    int abi;                  /* must be MD_MOD_ABI */
    const char *nome;         /* short name, shown in the log */
    const char *versao;
    const char *autor;
    const char *descricao;    /* one line: what it does */
} MdModInfo;

/* ------------------------------------------------------------------ runtime → mod */

/* Services. Every pointer here is guaranteed non-NULL for the ABI version the
 * runtime reported; check `abi` before using anything added in a later one. */
typedef struct {
    unsigned abi;

    /* Writes to the game's log (tinytoon.log in a release build, stdout
     * otherwise), prefixed with the mod's name. Use it — a mod that fails
     * silently is worse than one that does not load. */
    void (*log)(const char *fmt, ...);

    /* Frame counter since boot. */
    unsigned long (*quadro)(void);

    /* Work RAM, 64 KB. Index 0 is $FF0000, so the game's `$FFA622` is
     * `ram()[0xA622]`. Read-only; writing through this pointer is undefined and
     * will be caught by nothing. */
    const uint8_t *(*ram)(void);

    /* Video RAM, 64 KB: tiles, name tables, the sprite table. */
    const uint8_t *(*vram)(void);

    /* Colour RAM: 64 words, 4 palettes of 16. A colour is
     * `0000 BBB0 GGG0 RRR0`, three bits per channel. The DAC levels are not
     * linear — they are {0, 52, 87, 116, 144, 172, 206, 255}. */
    const uint16_t *(*cram)(void);

    /* VDP registers, 24 of them, as last written. */
    const uint8_t *(*regs)(void);

    /* Current display size: 320x224 or 256x224, since the game switches
     * between H40 and H32 depending on the screen. */
    void (*tela)(int *largura, int *altura);
} MdModHost;

/* ------------------------------------------------------------------ the hooks */

/* Required. Everything else is optional — a mod that only implements a filter
 * exports `md_mod_info` and `md_mod_pixels` and nothing more. Missing hooks are
 * simply not called. */

#if defined(_WIN32)
#define MD_MOD_EXPORT __declspec(dllexport)
#else
#define MD_MOD_EXPORT __attribute__((visibility("default")))
#endif

/* Identify yourself. Called first; if this is missing, or reports an ABI the
 * runtime does not know, the mod is not loaded and the log says why. */
MD_MOD_EXPORT MdModInfo *md_mod_info(void);

/* Called once, after the runtime is up and the ROM is verified. Return 0 to
 * accept, anything else to decline — a mod that finds its configuration missing
 * should decline here rather than misbehave for the rest of the session.
 *
 * Keep the host pointer; it stays valid until md_mod_encerrar. */
MD_MOD_EXPORT int md_mod_iniciar(const MdModHost *host);

/* Called once per frame, before the image is composed. This is where a mod
 * reads game state. */
MD_MOD_EXPORT void md_mod_quadro(void);

/* Called with the finished image, before it reaches the screen.
 *
 * `rgb` is `largura * altura * 3` bytes, R, G, B, one byte each, top row first.
 * Rewrite it in place. This runs once per *presented* frame, which is not
 * always once per game frame — during fast-forward the game runs far ahead of
 * the display.
 */
MD_MOD_EXPORT void md_mod_pixels(unsigned char *rgb, int largura, int altura);

/* Called at exit. Free what you allocated. */
MD_MOD_EXPORT void md_mod_encerrar(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYTOON_MOD_H */
