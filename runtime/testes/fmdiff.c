/* Compara a minha sintese FM com a do Nuked-OPN2 num patch controlado.
 *
 * Comparar rodando o jogo mostra QUE difere, nao ONDE: seis canais sobrepostos,
 * envelopes em fases diferentes, DAC no meio. Aqui um patch por vez, com os
 * parametros escolhidos para isolar uma coisa de cada vez:
 *
 *   alg 7, TL=0     quatro portadoras puras — testa escala e envelope
 *   alg 0           cadeia de quatro — testa profundidade de modulacao
 *   alg 4, fb       testa realimentacao
 *   TL alto no mod  testa a curva de volume do modulador
 *
 * Os dois recebem exatamente as mesmas escritas de registrador.
 */

#include "ym2612_fm.h"
#include "ym3438.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAXA 53267
#define AMOSTRAS 16000

static ym3438_t chip;
static int dac_ligado;

/* Escreve nos dois. O Nuked latcha uma escrita por clock, entao cada uma leva
 * 24 clocks — o mesmo que uma amostra. */
static void esc(unsigned reg, unsigned v) {
    Bit16s b[2];
    OPN2_Write(&chip, 0, (Bit8u)reg);
    for (int i = 0; i < 24; i++) OPN2_Clock(&chip, b);
    OPN2_Write(&chip, 1, (Bit8u)v);
    for (int i = 0; i < 24; i++) OPN2_Clock(&chip, b);

    if (reg == 0x28) fm_key((uint8_t)v);
    else if (reg == 0x2A) fm_dac(dac_ligado, (int8_t)(v - 128));
    else if (reg == 0x2B) { dac_ligado = (v & 0x80) != 0; fm_dac(dac_ligado, 0); }
    else if (reg >= 0x30) fm_escrever_reg(0, (uint8_t)reg, (uint8_t)v);
}

static void patch(unsigned alg, unsigned fb, const unsigned tl[4], unsigned ar,
                  unsigned mul) {
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip);
    fm_reset();

    esc(0x22, 0x00);              /* LFO desligado: comparar so' a sintese */
    esc(0x27, 0x00);
    esc(0x28, 0x00);
    for (unsigned op = 0; op < 4; op++) {
        unsigned o = op * 4;
        esc(0x30 + o, mul);       /* DT=0 */
        esc(0x40 + o, tl[op]);
        esc(0x50 + o, ar);        /* KS=0 */
        esc(0x60 + o, 0x00);      /* DR=0: sustenta, para medir regime */
        esc(0x70 + o, 0x00);
        esc(0x80 + o, 0x0F);      /* SL=0 RR=15 */
    }
    esc(0xB0, (fb << 3) | alg);
    esc(0xB4, 0xC0);
    esc(0xA4, 0x22);              /* bloco 4 */
    esc(0xA0, 0x69);
    esc(0x28, 0xF0);              /* key on canal 1 */
}

/* Potencia numa frequencia, por fasor incremental. */
static double banda(const double *x, int n, double f) {
    double cr = cos(2 * M_PI * f / TAXA), ci = sin(2 * M_PI * f / TAXA);
    double pr = 1, pi = 0, ar = 0, ai = 0;
    for (int i = 0; i < n; i++) {
        ar += x[i] * pr; ai += x[i] * pi;
        double t = pr * cr - pi * ci;
        pi = pr * ci + pi * cr; pr = t;
    }
    return sqrt(ar * ar + ai * ai) / n;
}

static double centro(const double *x, int n) {
    double num = 0, den = 0;
    for (int b = 0; b < 16; b++) {
        double f = 60.0 * pow(2.0, b * 0.6);
        if (f > TAXA / 2.2) break;
        double p = banda(x, n, f);
        num += f * p; den += p;
    }
    return den > 0 ? num / den : 0;
}

static double rms(const double *x, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += x[i] * x[i];
    return sqrt(s / n);
}

static void medir(const char *nome) {
    static double meu[AMOSTRAS], ref[AMOSTRAS];
    Bit16s b[2];

    for (int s = 0; s < AMOSTRAS; s++) {
        int32_t e, d;
        fm_amostra(&e, &d);
        meu[s] = e;

        int32_t re = 0;
        for (int i = 0; i < 24; i++) { OPN2_Clock(&chip, b); re += b[0]; }
        ref[s] = re;
    }

    /* Sem os primeiros mil: o ataque ainda esta' subindo nos dois. */
    const int ini = 2000, n = AMOSTRAS - ini;
    double rm = rms(meu + ini, n), rr = rms(ref + ini, n);
    double cm = centro(meu + ini, n), cr = centro(ref + ini, n);

    printf("%-28s rms %8.0f / %8.0f = %5.2f | centro %6.0f / %6.0f = %5.2f\n",
           nome, rm, rr, rr > 0 ? rm / rr : 0, cm, cr, cr > 0 ? cm / cr : 0);
}

/* Onda quadrada de 170 Hz pelo DAC, nos dois. Compara a escala do caminho de
 * PCM com a do caminho de FM medida acima. */
static void dac_teste(void) {
    static double meu[AMOSTRAS], ref[AMOSTRAS];
    Bit16s b[2];

    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip);
    fm_reset();
    esc(0x2B, 0x80);                     /* liga o DAC no canal 6 */
    esc(0xB4 + 2, 0xC0);                 /* canal 6: esquerda + direita */

    const int periodo = TAXA / 170 / 2;
    for (int s = 0; s < AMOSTRAS; s++) {
        unsigned v = ((s / periodo) & 1) ? 0xE0 : 0x20;
        OPN2_Write(&chip, 0, 0x2A);
        OPN2_Clock(&chip, b);
        OPN2_Write(&chip, 1, (Bit8u)v);
        fm_dac(1, (int8_t)(v - 128));

        int32_t e, d;
        fm_amostra(&e, &d);
        meu[s] = e;
        int32_t re = 0;
        for (int i = 0; i < 24; i++) { OPN2_Clock(&chip, b); re += b[0]; }
        ref[s] = re;
    }
    const int ini = 2000, n = AMOSTRAS - ini;
    double rm = rms(meu + ini, n), rr = rms(ref + ini, n);
    printf("%-28s rms %8.0f / %8.0f = %5.2f   <- comparar com ~10.4 da FM\n",
           "DAC onda quadrada 170 Hz", rm, rr, rr > 0 ? rm / rr : 0);
}

/* Forma do envelope ao longo do tempo.
 *
 * Os patches acima usam DR=0 e SR=0 — sustentacao pura. Musica de verdade vive
 * de decaimento, e um envelope rapido demais mata a nota antes da hora: o que
 * sobra e' ataque, que e' agudo e curto. Som "sem fundo" e "abafado" saem daqui
 * tanto quanto da sintese.
 *
 * Mede energia em janelas de 50 ms depois do key-on, e depois do key-off. */
static void envelope_teste(unsigned ar, unsigned dr, unsigned sl, unsigned sr,
                           unsigned rr, const char *nome) {
    Bit16s b[2];
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip);
    fm_reset();
    esc(0x22, 0x00); esc(0x27, 0x00); esc(0x28, 0x00);
    for (unsigned op = 0; op < 4; op++) {
        unsigned o = op * 4;
        esc(0x30 + o, 1); esc(0x40 + o, 0);
        esc(0x50 + o, ar); esc(0x60 + o, dr); esc(0x70 + o, sr);
        esc(0x80 + o, (sl << 4) | rr);
    }
    esc(0xB0, 0x07); esc(0xB4, 0xC0);
    esc(0xA4, 0x22); esc(0xA0, 0x69);
    esc(0x28, 0xF0);

    const int janela = TAXA / 20;      /* 50 ms */
    printf("%-22s ", nome);
    for (int j = 0; j < 10; j++) {
        if (j == 6) { esc(0x28, 0x00); }        /* key off aos 300 ms */
        double sm = 0, sr2 = 0;
        for (int i = 0; i < janela; i++) {
            int32_t e, d;
            fm_amostra(&e, &d);
            sm += (double)e * e;
            int32_t re = 0;
            for (int k = 0; k < 24; k++) { OPN2_Clock(&chip, b); re += b[0]; }
            sr2 += (double)re * re;
        }
        /* Valores CRUS, nao razao: uma razao entre dois numeros minusculos nao
         * distingue "os dois calaram" de "o meu nao calou". */
        printf("%6.0f/%-5.0f ", sqrt(sm / janela) / 10.4, sqrt(sr2 / janela));
    }
    printf("  (meu/ref por 50 ms; key off no 7o)\n");
}

int main(void) {
    unsigned zero[4] = {0, 0, 0, 0};
    unsigned so_port[4] = {40, 40, 40, 0};   /* moduladores atenuados */

    printf("%-28s %-28s | %s\n", "patch", "energia meu/ref", "centro meu/ref");

    patch(7, 0, zero, 0x1F, 1);  medir("alg7 4 portadoras TL=0");
    patch(7, 0, zero, 0x1F, 2);  medir("alg7 mul=2");
    patch(0, 0, zero, 0x1F, 1);  medir("alg0 cadeia TL=0");
    patch(0, 0, so_port, 0x1F, 1); medir("alg0 moduladores TL=40");
    patch(4, 0, zero, 0x1F, 1);  medir("alg4 duas cadeias");
    patch(4, 7, zero, 0x1F, 1);  medir("alg4 realimentacao 7");
    patch(2, 0, so_port, 0x1F, 1); medir("alg2 TL=40");
    patch(5, 0, so_port, 0x1F, 1); medir("alg5 TL=40");

    /* DAC: e' por onde passa a percussao e o grave da maioria dos drivers de
     * Mega Drive. Se a escala dele nao acompanhar a da FM, o resultado e' som
     * sem fundo — que e' exatamente o que "abafado" costuma querer dizer. */
    dac_teste();

    printf("\nenvelope (1.00 = igual a' referencia):\n");
    envelope_teste(0x1F, 0x00, 0, 0x00, 0x0F, "AR31 sem decaimento");
    envelope_teste(0x1F, 0x0A, 4, 0x00, 0x0F, "AR31 DR10 SL4");
    envelope_teste(0x1F, 0x10, 8, 0x08, 0x0F, "AR31 DR16 SL8 SR8");
    envelope_teste(0x10, 0x0A, 2, 0x04, 0x08, "AR16 DR10 SL2 SR4 RR8");
    return 0;
}
