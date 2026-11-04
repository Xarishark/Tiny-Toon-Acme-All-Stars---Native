#include "ym2612.h"

#include "audio.h"
#include "ym2612_fm.h"

#include <stdio.h>
#include <string.h>

unsigned long ym_teclas_ligadas;

/* DIAGNOSTICO: quais recursos do chip o jogo usa e a sintese ignora. Contar e'
 * o que separa "isso pode estar faltando" de "isso esta' faltando". */
static unsigned long ym_ch3_especial, ym_ssg_eg, ym_lfo_ligado;

static struct {
    uint8_t reg[2][256];
    uint8_t endereco[2];

    uint16_t timer_a;          /* conta ate' 1024 */
    uint8_t timer_b;           /* conta ate' 256, a cada 16 tempos internos */
    uint8_t sub_b;
    uint8_t status;            /* bit 0 = estouro de A, bit 1 = estouro de B */

    int8_t dac;                /* amostra do canal 6 em modo DAC */
    uint8_t dac_ligado;

    uint32_t resto_ciclos;     /* fracao de tempo interno acumulada */
} ym;

void ym2612_reset(void) {
    memset(&ym, 0, sizeof ym);
    ym_teclas_ligadas = 0;
    fm_reset();
}

static uint16_t periodo_a(void) {
    unsigned na = (unsigned)((ym.reg[0][0x24] << 2) | (ym.reg[0][0x25] & 3));
    return (uint16_t)(1024u - na);
}

static uint16_t periodo_b(void) {
    return (uint16_t)(256u - ym.reg[0][0x26]);
}

void ym2612_escrever(unsigned porta, uint8_t v) {
    unsigned parte = (porta >> 1) & 1;

    if ((porta & 1) == 0) { ym.endereco[parte] = v; return; }

    uint8_t a = ym.endereco[parte];
    ym.reg[parte][a] = v;

    if (parte == 0) {
        switch (a) {
        case 0x27:
            /* Bits 4 e 5 zeram o estouro; 0 e 1 ligam a contagem. Zerar na
             * escrita e' como o driver reconhece que o tempo passou. */
            if (v & 0x10) ym.status &= (uint8_t)~0x01;
            if (v & 0x20) ym.status &= (uint8_t)~0x02;
            if (!(ym.reg[0][0x27] & 0x01)) ym.timer_a = periodo_a();
            if (!(ym.reg[0][0x27] & 0x02)) ym.timer_b = (uint8_t)periodo_b();
            break;
        case 0x28:
            /* Key on/off: bits 4-7 escolhem os operadores, 0-2 o canal. */
            if (v & 0xF0) ym_teclas_ligadas++;
            fm_key(v);
            break;
        case 0x2A:
            /* Empurrado na ESCRITA, nao na hora de renderizar: e' o valor
             * corrente no instante da amostra que importa. Ler no fim do
             * quadro dava um unico valor para as 882 amostras — todo PCM
             * virava ruido. */
            ym.dac = (int8_t)(v - 128);
            fm_dac(ym.dac_ligado, ym.dac);
            break;
        case 0x2B:
            ym.dac_ligado = (uint8_t)((v & 0x80) != 0);
            fm_dac(ym.dac_ligado, ym.dac);
            break;
        default: break;
        }
    }

    if (parte == 0 && a == 0x27 && (v & 0xC0)) ym_ch3_especial++;
    if (parte == 0 && a == 0x22 && (v & 0x08)) ym_lfo_ligado++;
    if (a >= 0x90 && a <= 0x9E && (v & 0x08)) ym_ssg_eg++;

    /* Tudo de $30 para cima e' sintese. */
    if (a >= 0x30) fm_escrever_reg(parte, a, v);
}

/* O status e' o unico registrador de leitura, e e' por ele que o driver sabe que
 * o tempo passou. Bit 7 seria "ocupado"; aqui a escrita e' instantanea, entao
 * devolver zero nesse bit e' o comportamento util e nao engana ninguem. */
uint8_t ym2612_ler(void) { return ym.status; }

void ym2612_avancar(unsigned ciclos) {
    ym.resto_ciclos += ciclos;
    unsigned tempos = ym.resto_ciclos / YM_DIV;
    ym.resto_ciclos %= YM_DIV;
    if (!tempos) return;

    if (ym.reg[0][0x27] & 0x01) {
        while (tempos--) {
            if (ym.timer_a) ym.timer_a--;
            if (!ym.timer_a) {
                ym.timer_a = periodo_a();
                if (ym.reg[0][0x27] & 0x04) ym.status |= 0x01;
            }
            if (++ym.sub_b >= 16) {
                ym.sub_b = 0;
                if (ym.reg[0][0x27] & 0x02) {
                    if (ym.timer_b) ym.timer_b--;
                    if (!ym.timer_b) {
                        ym.timer_b = (uint8_t)periodo_b();
                        if (ym.reg[0][0x27] & 0x08) ym.status |= 0x02;
                    }
                }
            }
        }
        return;
    }

    /* Mesmo com o A desligado, o B precisa andar. */
    if (ym.reg[0][0x27] & 0x02) {
        while (tempos--) {
            if (++ym.sub_b >= 16) {
                ym.sub_b = 0;
                if (ym.timer_b) ym.timer_b--;
                if (!ym.timer_b) {
                    ym.timer_b = (uint8_t)periodo_b();
                    if (ym.reg[0][0x27] & 0x08) ym.status |= 0x02;
                }
            }
        }
    }
}

void ym2612_relatorio(void) {
    printf("  usa: CH3 especial %lu, SSG-EG %lu, LFO %lu\n",
           ym_ch3_especial, ym_ssg_eg, ym_lfo_ligado);
    printf("YM2612: temporizadores %s%s | DAC %s | %lu notas disparadas\n",
           (ym.reg[0][0x27] & 1) ? "A " : "",
           (ym.reg[0][0x27] & 2) ? "B" : ((ym.reg[0][0x27] & 1) ? "" : "parados"),
           ym.dac_ligado ? "ligado" : "desligado", ym_teclas_ligadas);
}
