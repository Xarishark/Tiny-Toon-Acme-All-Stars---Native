#include "audio.h"

#include "psg.h"
#include "ym2612_fm.h"

#include <string.h>

unsigned long audio_estourou;

/* Anel de pares estereo. Sete quadros de folga: o suficiente para absorver uma
 * pausa curta do hospedeiro sem virar latencia perceptivel. */
#define ANEL_PARES 8192

static int16_t anel[ANEL_PARES * 2];
static unsigned escrita, leitura, ocupados;

void audio_reset(void) {
    psg_reset();
    fm_reset();
    memset(anel, 0, sizeof anel);
    escrita = leitura = ocupados = 0;
    audio_estourou = 0;
}

static void empurrar(int32_t esq, int32_t dir) {
    if (ocupados >= ANEL_PARES) { audio_estourou++; return; }
    if (esq > 32767) esq = 32767; if (esq < -32768) esq = -32768;
    if (dir > 32767) dir = 32767; if (dir < -32768) dir = -32768;
    anel[escrita * 2] = (int16_t)esq;
    anel[escrita * 2 + 1] = (int16_t)dir;
    escrita = (escrita + 1) % ANEL_PARES;
    ocupados++;
}

void audio_gerar(unsigned pares) {
    while (pares--) {
        int32_t e, d;
        fm_amostra(&e, &d);

        /* O PSG e' mono e entra igual nos dois lados, como no somador do
         * console. Dividir a FM por dois deixa folga para ele. */
        int32_t p = psg_amostra();
        empurrar((e >> 1) + p, (d >> 1) + p);
    }
}

unsigned audio_disponivel(void) { return ocupados; }

unsigned audio_drenar(int16_t *destino, unsigned quadros) {
    unsigned n = quadros < ocupados ? quadros : ocupados;
    for (unsigned i = 0; i < n; i++) {
        destino[i * 2] = anel[leitura * 2];
        destino[i * 2 + 1] = anel[leitura * 2 + 1];
        leitura = (leitura + 1) % ANEL_PARES;
    }
    ocupados -= n;
    return n;
}
