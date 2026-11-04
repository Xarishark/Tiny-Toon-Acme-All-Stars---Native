/* Substitui o YM2612 inteiro pelo Nuked-OPN2, para teste diferencial.
 *
 * O Nuked foi escrito a partir da analise do SILICIO do chip — nao do datasheet.
 * E' a referencia mais forte que existe, e sobretudo e' independente da minha
 * leitura, que ja' errou a escala do operador e o relogio do chip.
 *
 * Este arquivo implementa as MESMAS funcoes que ym2612.c e ym2612_fm.c juntos,
 * entao o resto do runtime nao muda: o jogo roda igual, o driver ve' os mesmos
 * temporizadores, e a unica diferenca entre os dois binarios e' de quem sai a
 * amostra. Comparar os dois WAV isola exatamente a sintese.
 *
 * Nao entra no jogo — e' referencia de teste, como o Musashi e o BlastEm.
 */

#include "ym2612.h"
#include "ym2612_fm.h"

#include <stdio.h>

#include "ym3438.h"

unsigned long ym_teclas_ligadas;

static ym3438_t chip;
static uint8_t endereco[2];

/* Fila de escritas.
 *
 * OPN2_Write nao escreve: ele LATCHA uma escrita, que so' e' consumida no
 * proximo OPN2_Clock. Duas escritas seguidas sem clock no meio e a primeira se
 * perde — e o driver escreve endereco e dado em sequencia, entao quase tudo era
 * descartado. O sintoma foi 1,2 milhao de escritas chegando ao chip e ZERO
 * notas tocando.
 *
 * O chip real tambem leva tempo por escrita; a diferenca e' que o driver espera
 * pelo bit de ocupado. Aqui a fila faz esse papel: ha' ~62 clocks de folga por
 * escrita, entao ela nunca atrasa nada de forma perceptivel. */
#define FILA 8192
static struct { uint8_t porta, dado; } fila[FILA];
static unsigned fila_e, fila_s, fila_n;
static unsigned long fila_perdidas;

void ym2612_reset(void) {
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip);
    ym_teclas_ligadas = 0;
    fila_e = fila_s = fila_n = 0;
    fila_perdidas = 0;
}

void ym2612_escrever(unsigned porta, uint8_t v) {
    if ((porta & 1) == 0) endereco[(porta >> 1) & 1] = v;
    else if (((porta >> 1) & 1) == 0 && endereco[0] == 0x28 && (v & 0xF0)) {
        ym_teclas_ligadas++;
    }
    if (fila_n < FILA) {
        fila[fila_e].porta = (uint8_t)porta;
        fila[fila_e].dado = v;
        fila_e = (fila_e + 1) % FILA;
        fila_n++;
    } else {
        fila_perdidas++;
    }
}

uint8_t ym2612_ler(void) { return OPN2_Read(&chip, 0); }

/* Os temporizadores do Nuked andam com OPN2_Clock, que e' chamado ao gerar
 * amostra. Nao ha' o que adiantar aqui. */
void ym2612_avancar(unsigned ciclos) { (void)ciclos; }

void ym2612_relatorio(void) {
    printf("YM2612: NUKED-OPN2 (referencia) | %lu notas | fila %u, %lu perdidas\n",
           ym_teclas_ligadas, fila_n, fila_perdidas);
}

/* --- o lado que audio.c chama ------------------------------------------- */

void fm_reset(void) { ym2612_reset(); }

/* Uma amostra sao 24 clocks internos: o chip multiplexa os seis canais nos
 * pinos de saida, um por vez, e somar os 24 e' o que reconstroi o valor. */
void fm_amostra(int32_t *esquerda, int32_t *direita) {
    Bit16s buf[2];
    int32_t e = 0, d = 0;
    for (int i = 0; i < 24; i++) {
        /* Uma escrita por clock: e' assim que o latch do Nuked absorve a fila
         * sem perder nada. */
        if (fila_n) {
            OPN2_Write(&chip, fila[fila_s].porta, fila[fila_s].dado);
            fila_s = (fila_s + 1) % FILA;
            fila_n--;
        }
        OPN2_Clock(&chip, buf);
        e += buf[0];
        d += buf[1];
    }
    *esquerda = e;
    *direita = d;
}

/* Sem uso: no build de referencia as escritas vao direto para o Nuked. */
void fm_escrever_reg(unsigned parte, uint8_t reg, uint8_t v) {
    (void)parte; (void)reg; (void)v;
}
void fm_key(uint8_t v) { (void)v; }
void fm_dac(int ligado, int8_t amostra) { (void)ligado; (void)amostra; }
