#include "psg.h"

#include "audio.h"

#include <string.h>

/* Relogio: 3.579.545 Hz divididos por 16 antes de chegar aos contadores. */
#define PSG_CLOCK   3579545u
#define PSG_DIV     16u

/* Volume por atenuacao, em passos de 2 dB. O indice 15 e' silencio absoluto —
 * nao "quase", e' desligado. Os valores sao a curva do chip, nao linear. */
static const int16_t VOLUME[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031,  819,  650,  516,  410,  326,    0,
};

static struct {
    uint16_t periodo[4];       /* 10 bits; o canal 3 usa 3 bits (controle de ruido) */
    uint8_t volume[4];         /* atenuacao 0-15 */
    int16_t contador[4];
    uint8_t saida[4];          /* 0 ou 1 */
    uint16_t lfsr;
    uint8_t canal_latch;
    uint8_t tipo_latch;        /* 0 = tom, 1 = volume */
    /* Fracao de ciclo acumulada entre amostras. O PSG anda a 223.721 Hz e a
     * saida a 44.100: sem acumular a fracao, a afinacao sai errada. */
    uint32_t resto;
} psg;

void psg_reset(void) {
    memset(&psg, 0, sizeof psg);
    for (int i = 0; i < 4; i++) {
        psg.volume[i] = 15;    /* silencio */
        psg.periodo[i] = 1;
    }
    psg.lfsr = 0x8000;
    psg.canal_latch = 0;
}

void psg_escrever(uint8_t v) {
    if (v & 0x80) {
        /* Byte de latch: canal nos bits 6-5, tipo no 4, 4 bits baixos de dado. */
        psg.canal_latch = (uint8_t)((v >> 5) & 3);
        psg.tipo_latch = (uint8_t)((v >> 4) & 1);
        if (psg.tipo_latch) {
            psg.volume[psg.canal_latch] = (uint8_t)(v & 0x0F);
        } else {
            psg.periodo[psg.canal_latch] =
                (uint16_t)((psg.periodo[psg.canal_latch] & 0x3F0) | (v & 0x0F));
            if (psg.canal_latch == 3) psg.lfsr = 0x8000;
        }
    } else {
        /* Byte de dado: completa os 6 bits altos do periodo. Volume nao usa. */
        if (psg.tipo_latch) {
            psg.volume[psg.canal_latch] = (uint8_t)(v & 0x0F);
        } else if (psg.canal_latch == 3) {
            psg.periodo[3] = (uint16_t)(v & 0x07);
            psg.lfsr = 0x8000;
        } else {
            psg.periodo[psg.canal_latch] =
                (uint16_t)((psg.periodo[psg.canal_latch] & 0x00F) | ((v & 0x3F) << 4));
        }
    }
}

/* Periodo do canal de ruido: 0/1/2 sao divisores fixos, 3 acompanha o canal 2.
 * Bit 2 do registro escolhe ruido branco (LFSR com dois taps) ou periodico. */
static uint16_t periodo_ruido(void) {
    switch (psg.periodo[3] & 3) {
    case 0: return 0x10;
    case 1: return 0x20;
    case 2: return 0x40;
    default: return psg.periodo[2] ? psg.periodo[2] : 1;
    }
}

static void avancar(unsigned passos) {
    for (int c = 0; c < 3; c++) {
        uint16_t p = psg.periodo[c] ? psg.periodo[c] : 1;
        psg.contador[c] = (int16_t)(psg.contador[c] - (int)passos);
        while (psg.contador[c] <= 0) {
            psg.contador[c] = (int16_t)(psg.contador[c] + p);
            psg.saida[c] ^= 1;
        }
    }

    uint16_t pr = periodo_ruido();
    psg.contador[3] = (int16_t)(psg.contador[3] - (int)passos);
    while (psg.contador[3] <= 0) {
        psg.contador[3] = (int16_t)(psg.contador[3] + pr);
        /* Ruido branco: XOR dos bits 0 e 3. Periodico: so' o bit 0 recircula. */
        unsigned bit = (psg.periodo[3] & 4)
                     ? ((psg.lfsr ^ (psg.lfsr >> 3)) & 1)
                     : (psg.lfsr & 1);
        psg.lfsr = (uint16_t)((psg.lfsr >> 1) | (bit << 15));
        psg.saida[3] = (uint8_t)(psg.lfsr & 1);
    }
}

int16_t psg_amostra(void) {
    /* Quantos passos do PSG cabem numa amostra, com a fracao acumulada. Sem
     * acumular a fracao a afinacao sai errada. */
    psg.resto += PSG_CLOCK / PSG_DIV;
    unsigned passos = psg.resto / MD_TAXA_AUDIO;
    psg.resto %= MD_TAXA_AUDIO;
    if (passos) avancar(passos);

    int32_t v = 0;
    for (int c = 0; c < 4; c++) {
        if (psg.volume[c] == 15) continue;
        v += psg.saida[c] ? VOLUME[psg.volume[c]] : -VOLUME[psg.volume[c]];
    }
    /* Quatro canais somados cabem em 15 bits; dividir por 4 deixa margem para o
     * YM2612 somar em cima sem estourar. */
    return (int16_t)(v / 4);
}
