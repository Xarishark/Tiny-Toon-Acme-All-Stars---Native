/* filtro-cor — an example mod, and the smallest useful one.
 *
 * Repaints the finished frame through a colour ramp. It exists to prove the mod
 * path end to end and to be a template you can copy: it touches every hook that
 * matters and nothing else.
 *
 * WHY THIS AND NOT A SEPIA ONE-LINER
 *
 * A filter you have to squint at is a bad test. "Game Boy" mode maps every pixel
 * to four shades of green — if it loaded, you know within one frame, from across
 * the room. A test whose result is ambiguous is a test you end up debugging
 * instead of the thing it was testing.
 *
 * Pick a ramp with the environment variable:
 *
 *     TINYTOON_FILTRO=gameboy   four greens (default)
 *     TINYTOON_FILTRO=ambar     amber monochrome monitor
 *     TINYTOON_FILTRO=cinza     plain greyscale
 *     TINYTOON_FILTRO=sepia
 *
 * Build:  make          (see the Makefile beside this file)
 * Use:    copy the resulting library into `mods/` next to the executable
 */

#include "../../runtime/mod.h"

#include <stdlib.h>
#include <string.h>

static const MdModHost *host;

/* Luminance weights. Not thirds: the eye is far more sensitive to green than to
 * blue, and averaging the channels equally makes reds and blues collapse into
 * the same grey. These are the usual Rec.601 coefficients, in fixed point so
 * there is no floating point in a per-pixel loop. */
#define LUM(r, g, b)  (((r) * 77 + (g) * 150 + (b) * 29) >> 8)

typedef struct {
    const char *nome;
    int niveis;            /* how many steps; 4 is what gives the Game Boy look */
    unsigned char cor[4][3];
} Rampa;

static const Rampa RAMPAS[] = {
    /* The original DMG palette, darkest first. */
    { "gameboy", 4, { {15, 56, 15}, {48, 98, 48}, {139, 172, 15}, {155, 188, 15} } },
    { "ambar",   4, { {24, 12, 0}, {102, 54, 0}, {204, 122, 0}, {255, 176, 40} } },
    { "cinza",   4, { {24, 24, 24}, {96, 96, 96}, {176, 176, 176}, {245, 245, 245} } },
    { "sepia",   4, { {28, 20, 12}, {102, 78, 52}, {176, 142, 100}, {240, 214, 176} } },
};
#define N_RAMPAS ((int)(sizeof RAMPAS / sizeof *RAMPAS))

static const Rampa *rampa = &RAMPAS[0];

/* Luminance (0-255) to ramp index, precomputed once.
 *
 * Doing it per pixel would be a division in the inner loop, 320x224 times per
 * frame. A 256-entry table turns it into an array read, and the table costs
 * nothing to build. */
static unsigned char indice[256];

static MdModInfo INFO = {
    MD_MOD_ABI,
    "filtro-cor",
    "1.0",
    "exemplo",
    "Repaints the frame through a four-tone colour ramp",
};

MD_MOD_EXPORT MdModInfo *md_mod_info(void) { return &INFO; }

MD_MOD_EXPORT int md_mod_iniciar(const MdModHost *h) {
    host = h;

    const char *pedido = getenv("TINYTOON_FILTRO");
    if (pedido && *pedido) {
        int achou = 0;
        for (int i = 0; i < N_RAMPAS; i++) {
            if (strcmp(pedido, RAMPAS[i].nome) == 0) { rampa = &RAMPAS[i]; achou = 1; break; }
        }
        /* An unknown name is reported and then ignored, rather than refusing to
         * load. Somebody who mistyped a ramp still wants to play. */
        if (!achou) {
            host->log("ramp '%s' unknown; using '%s'. Options:", pedido, rampa->nome);
            for (int i = 0; i < N_RAMPAS; i++) host->log("  %s", RAMPAS[i].nome);
        }
    }

    for (int i = 0; i < 256; i++) {
        int k = i * rampa->niveis / 256;
        indice[i] = (unsigned char)(k >= rampa->niveis ? rampa->niveis - 1 : k);
    }

    host->log("ramp '%s', %d levels", rampa->nome, rampa->niveis);
    return 0;
}

MD_MOD_EXPORT void md_mod_pixels(unsigned char *rgb, int largura, int altura) {
    const long n = (long)largura * altura;
    for (long p = 0; p < n; p++) {
        unsigned char *px = rgb + p * 3;
        const unsigned char *c = rampa->cor[indice[LUM(px[0], px[1], px[2])]];
        px[0] = c[0];
        px[1] = c[1];
        px[2] = c[2];
    }
}

MD_MOD_EXPORT void md_mod_encerrar(void) {
    if (host) host->log("done");
}
