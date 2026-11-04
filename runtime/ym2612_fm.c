/* Sintese FM do YM2612.
 *
 * O chip trabalha em DOMINIO LOGARITMICO, e entender isso e' entender o resto:
 * o seno esta' tabelado como atenuacao em dB, nao como amplitude. Somar
 * atenuacoes e' multiplicar amplitudes, entao aplicar o envelope vira uma soma
 * inteira, e so' no fim uma tabela exponencial devolve a amplitude. E' por isso
 * que nao ha' um unico multiplicador no caminho do sinal.
 *
 *   atenuacao total = seno_log[fase] + envelope + volume
 *   amostra         = 2^(-atenuacao)
 *
 * Cada canal tem quatro operadores. O algoritmo (0-7) decide quem modula quem e
 * quem vai para a saida; o operador 1 pode modular a si mesmo (realimentacao).
 */

#include "ym2612_fm.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>

#ifndef EG_DIV
#define EG_DIV 3
#endif

#define CANAIS 6
#define OPS 4

/* Seno em log2, 256 passos por quarto de onda, escala de 256 por oitava.
 *
 * A exponencial devolve amplitude CHEIA DE 14 BITS (8192), que e' a largura da
 * saida de um operador no chip. Com 1024 acontecem duas coisas ao mesmo tempo:
 * o som sai oito vezes baixo demais e, pior, a modulacao fica oito vezes fraca —
 * o timbre inteiro muda, porque em FM a amplitude do modulador E' o indice de
 * modulacao. */
static uint16_t seno_log[256];
static uint16_t exp_tab[256];
static int tabelas_fm;

static void montar_tabelas_fm(void) {
    for (int i = 0; i < 256; i++) {
        /* Meio passo evita o infinito em sin(0). */
        double m = sin(((double)i + 0.5) * M_PI / 512.0);
        seno_log[i] = (uint16_t)(-log(m) / log(2.0) * 256.0 + 0.5);
        exp_tab[i] = (uint16_t)(pow(2.0, -((double)i + 1.0) / 256.0) * 8192.0 + 0.5);
    }
    tabelas_fm = 1;
}

/* Detune, em passos de fase, indexado por [dt][keycode>>2]. Valores do chip. */
static const uint8_t DETUNE[4][32] = {
    {0},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22},
};

/* Quatro bits altos do fnum viram os dois bits baixos do keycode, por esta
 * regra do chip — nao e' truncamento simples. */
static uint8_t keycode_de(uint16_t fnum, uint8_t bloco) {
    unsigned f11 = (fnum >> 10) & 1;
    unsigned f10 = (fnum >> 9) & 1;
    unsigned f9 = (fnum >> 8) & 1;
    unsigned f8 = (fnum >> 7) & 1;
    unsigned n = f11 & (f10 | f9 | f8);
    unsigned m = f11 & ~(f10 | f9 | f8) ? 0 : (f11 ? 1 : (f10 & f9 & f8));
    unsigned baixo = (n << 1) | (n ? 1 : m);
    return (uint8_t)((bloco << 2) | (baixo & 3));
}

enum { PARADO = 0, ATAQUE, DECAI, SUSTENTA, SOLTA };

typedef struct {
    uint32_t fase;
    uint32_t inc;
    int estado;
    int nivel;                 /* atenuacao 0 (alto) .. 1023 (mudo) */
    uint8_t dt, mul, tl, ks, ar, dr, sr, rr, sl;
    uint8_t am;                /* este operador responde ao tremolo? */
    uint8_t ligado;
    int32_t saida[2];          /* duas ultimas amostras, para realimentacao */
} Op;

typedef struct {
    Op op[OPS];
    uint16_t fnum;
    uint8_t bloco;
    uint8_t alg, fb;
    uint8_t esq, dir;
    uint8_t ams, pms;   /* profundidade de tremolo e vibrato */
} Canal;

/* LFO: um oscilador lento que faz TREMOLO (amplitude) e VIBRATO (frequencia).
 *
 * Sem ele o som fica "reto" — nota certa, timbre certo, e ainda assim
 * perceptivelmente diferente do console, porque quase toda musica de Mega Drive
 * usa vibrato em sustentacao.
 *
 * O tremolo e' exato: a profundidade em dB esta' no datasheet e entra somando
 * atenuacao, que e' a moeda deste chip.
 *
 * O vibrato e' calculado a partir das PROFUNDIDADES EM CENTS documentadas
 * (3,4 / 6,7 / 10 / 14 / 20 / 40 / 80), e nao da tabela interna do chip, que eu
 * nao tenho. Isso da' vibrato na taxa e na profundidade certas, mas nao
 * bit-a-bit igual. Esta marcado aqui porque e' a diferenca que sobra. */
static const uint8_t LFO_PASSOS[8] = {108, 77, 71, 67, 62, 44, 8, 5};
static const uint8_t AMS_DESLOC[4] = {8, 3, 1, 0};   /* 0, 1.4, 5.9, 11.8 dB */

static uint32_t lfo_pos, lfo_resto;
static uint8_t lfo_ligado, lfo_freq;
static uint32_t pm_mult[8][128];      /* 16.16: multiplicador do incremento */
static int tabelas_lfo;

static void montar_lfo(void) {
    static const double CENTS[8] = {0, 3.4, 6.7, 10, 14, 20, 40, 80};
    for (int pms = 0; pms < 8; pms++) {
        for (int p = 0; p < 128; p++) {
            /* Triangulo de quatro quadrantes, -32..+32. */
            int q = p >> 5, r = p & 31, amp;
            if (q == 0) amp = r;
            else if (q == 1) amp = 32 - r;
            else if (q == 2) amp = -r;
            else amp = r - 32;
            double cents = CENTS[pms] * amp / 32.0;
            pm_mult[pms][p] = (uint32_t)(pow(2.0, cents / 1200.0) * 65536.0 + 0.5);
        }
    }
    tabelas_lfo = 1;
}

/* Triangulo unipolar 0..126 para o tremolo. */
static unsigned lfo_am(void) {
    unsigned t = lfo_pos & 63;
    return ((lfo_pos & 64) ? (63u - t) : t) * 2u;
}

static Canal canal[CANAIS];
static int dac_ligado;
static int8_t dac_valor;
static uint32_t eg_contador;
static uint32_t eg_resto;

void fm_reset(void) {
    if (!tabelas_fm) montar_tabelas_fm();
    memset(canal, 0, sizeof canal);
    /* O DAC tambem. Sem isto o canal 6 continua emitindo a ultima amostra para
     * sempre — um nivel constante que nunca cala e que se soma a tudo. */
    dac_ligado = 0;
    dac_valor = 0;
    for (int c = 0; c < CANAIS; c++) {
        for (int o = 0; o < OPS; o++) {
            canal[c].op[o].nivel = 1023;
            canal[c].op[o].estado = PARADO;
        }
        canal[c].esq = canal[c].dir = 1;
    }
    eg_contador = eg_resto = 0;
}

/* O layout dos registradores mistura canal e operador de um jeito nao obvio:
 * os dois bits baixos escolhem o canal, os bits 2-3 escolhem o operador, e a
 * ordem dos operadores no registrador (0,2,1,3) NAO e' a ordem do algoritmo. */
static const uint8_t ORDEM_OP[4] = {0, 2, 1, 3};

static void atualizar_inc(Canal *ch, Op *op) {
    uint8_t kc = keycode_de(ch->fnum, ch->bloco);
    uint32_t base = ((uint32_t)ch->fnum << ch->bloco) >> 1;
    unsigned dt = DETUNE[op->dt & 3][kc >> 1 > 31 ? 31 : (kc >> 1)];
    if (op->dt & 4) base -= dt; else base += dt;
    base &= 0x1FFFF;
    op->inc = op->mul ? base * op->mul : base >> 1;
}

void fm_escrever_reg(unsigned parte, uint8_t reg, uint8_t v) {
    if (!tabelas_fm) montar_tabelas_fm();

    if (parte == 0 && reg == 0x22) {
        lfo_ligado = (uint8_t)((v >> 3) & 1);
        lfo_freq = (uint8_t)(v & 7);
        return;
    }

    unsigned c = reg & 3;
    if (c == 3) return;                       /* nao existe canal 4 por metade */
    c += parte * 3;
    Canal *ch = &canal[c];

    if (reg >= 0x30 && reg < 0xA0) {
        Op *op = &ch->op[ORDEM_OP[(reg >> 2) & 3]];
        switch (reg & 0xF0) {
        case 0x30: op->dt = (uint8_t)((v >> 4) & 7); op->mul = (uint8_t)(v & 15);
                   atualizar_inc(ch, op); break;
        case 0x40: op->tl = (uint8_t)(v & 0x7F); break;
        case 0x50: op->ks = (uint8_t)(v >> 6); op->ar = (uint8_t)(v & 0x1F); break;
        case 0x60: op->dr = (uint8_t)(v & 0x1F);
                   op->am = (uint8_t)((v >> 7) & 1); break;
        case 0x70: op->sr = (uint8_t)(v & 0x1F); break;
        case 0x80: op->sl = (uint8_t)(v >> 4); op->rr = (uint8_t)(v & 0x0F); break;
        default: break;                       /* $90: SSG-EG, nao usado aqui */
        }
        return;
    }

    switch (reg & 0xFC) {
    case 0xA0: ch->fnum = (uint16_t)((ch->fnum & 0x700) | v); break;
    case 0xA4: ch->fnum = (uint16_t)((ch->fnum & 0x0FF) | ((v & 7) << 8));
               ch->bloco = (uint8_t)((v >> 3) & 7); break;
    case 0xB0: ch->alg = (uint8_t)(v & 7); ch->fb = (uint8_t)((v >> 3) & 7); break;
    case 0xB4: ch->esq = (uint8_t)((v >> 7) & 1); ch->dir = (uint8_t)((v >> 6) & 1);
               ch->ams = (uint8_t)((v >> 4) & 3); ch->pms = (uint8_t)(v & 7);
               break;
    default: return;
    }
    for (int o = 0; o < OPS; o++) atualizar_inc(ch, &ch->op[o]);
}

void fm_key(uint8_t v) {
    unsigned n = v & 7;
    if ((n & 3) == 3) return;
    unsigned c = (n & 3) + ((n & 4) ? 3 : 0);
    Canal *ch = &canal[c];
    for (int o = 0; o < OPS; o++) {
        int ligar = (v >> (4 + o)) & 1;       /* bits 4-7 na ordem 0,2,1,3 */
        Op *op = &ch->op[ORDEM_OP[o]];
        if (ligar && !op->ligado) {
            op->ligado = 1;
            op->estado = ATAQUE;
            op->fase = 0;
        } else if (!ligar && op->ligado) {
            op->ligado = 0;
            op->estado = SOLTA;
        }
    }
}

/* Passo do envelope.
 *
 * O gerador nao anda a cada amostra: ele anda quando o contador cruza um
 * limiar que depende da taxa. Taxas baixas andam raramente e pouco; taxas altas
 * andam sempre e muito. Os padroes de oito passos sao do chip — servem para que
 * taxas vizinhas tenham velocidade media diferente sem precisar de fracao. */
static const uint8_t PAD_BAIXO[4][8] = {
    {0,1,0,1,0,1,0,1},
    {0,1,0,1,1,1,0,1},
    {0,1,1,1,0,1,1,1},
    {0,1,1,1,1,1,1,1},
};
static const uint8_t PAD_ALTO[4][8] = {
    {1,1,1,1,1,1,1,1},
    {1,1,1,2,1,1,1,2},
    {1,2,1,2,1,2,1,2},
    {1,2,2,2,1,2,2,2},
};

static int passo_eg(unsigned taxa, uint32_t cnt) {
    if (taxa >= 64) taxa = 63;
    unsigned alto = taxa >> 2, baixo = taxa & 3;
    if (alto < 12) {
        unsigned desloc = 11 - alto;
        if (cnt & ((1u << desloc) - 1)) return 0;
        return PAD_BAIXO[baixo][(cnt >> desloc) & 7];
    }
    if (alto >= 15) return 8;
    return PAD_ALTO[baixo][cnt & 7] << (alto - 12);
}

static unsigned taxa_efetiva(const Canal *ch, const Op *op, unsigned base) {
    if (!base) return 0;                      /* taxa zero nunca anda */
    uint8_t kc = keycode_de(ch->fnum, ch->bloco);
    unsigned ksr = kc >> (3 - op->ks);
    unsigned r = base * 2 + ksr;
    return r > 63 ? 63 : r;
}

static void avancar_eg(void) {
    for (int c = 0; c < CANAIS; c++) {
        Canal *ch = &canal[c];
        for (int o = 0; o < OPS; o++) {
            Op *op = &ch->op[o];
            switch (op->estado) {
            case ATAQUE: {
                unsigned r = taxa_efetiva(ch, op, op->ar);
                if (r >= 62) { op->nivel = 0; op->estado = DECAI; break; }
                int inc = passo_eg(r, eg_contador);
                if (inc) {
                    /* Ataque e' exponencial: quanto mais perto do topo, menor o
                     * passo. Dai o "~nivel" no lugar de uma constante. */
                    op->nivel += (~op->nivel * inc) >> 4;
                    if (op->nivel <= 0) { op->nivel = 0; op->estado = DECAI; }
                }
                break;
            }
            case DECAI: {
                int inc = passo_eg(taxa_efetiva(ch, op, op->dr), eg_contador);
                op->nivel += inc;
                int limiar = (op->sl == 15) ? 1023 : op->sl * 32;
                if (op->nivel >= limiar) { op->nivel = limiar; op->estado = SUSTENTA; }
                break;
            }
            case SUSTENTA: {
                int inc = passo_eg(taxa_efetiva(ch, op, op->sr), eg_contador);
                op->nivel += inc;
                if (op->nivel >= 1023) { op->nivel = 1023; op->estado = PARADO; }
                break;
            }
            case SOLTA: {
                /* RR tem 4 bits e vale o dobro mais um na escala de taxa. */
                int inc = passo_eg(taxa_efetiva(ch, op, op->rr * 2 + 1), eg_contador);
                op->nivel += inc;
                if (op->nivel >= 1023) { op->nivel = 1023; op->estado = PARADO; }
                break;
            }
            default: break;
            }
        }
    }
}

/* Um operador: soma a modulacao a' fase, le' o seno em log, soma a atenuacao do
 * envelope e do volume, e volta ao dominio linear. */
static int32_t operar(const Op *op, int32_t mod, uint32_t am) {
    uint32_t p = ((op->fase >> 10) + (uint32_t)mod) & 0x3FF;
    uint32_t i = p & 0xFF;
    uint32_t idx = (p & 0x100) ? (255u - i) : i;

    /* Tudo somado em atenuacao: envelope, volume e tremolo. Uma unidade de
     * envelope vale quatro de atenuacao, dai os deslocamentos. */
    uint32_t att = seno_log[idx] + ((uint32_t)op->nivel << 2)
                 + ((uint32_t)op->tl << 5) + (op->am ? (am << 2) : 0);
    if (att > 0x1FFF) return 0;

    int32_t v = exp_tab[att & 0xFF] >> (att >> 8);
    return (p & 0x200) ? -v : v;
}

/* Vibrato entra aqui: multiplica o incremento de fase de todos os operadores do
 * canal pelo mesmo fator, que e' o que "mudar a nota" significa. */
static void avancar_fase(Canal *ch) {
    uint32_t m = (lfo_ligado && ch->pms) ? pm_mult[ch->pms][lfo_pos] : 65536u;
    for (int o = 0; o < OPS; o++) {
        uint32_t inc = ch->op[o].inc;
        if (m != 65536u) inc = (uint32_t)(((uint64_t)inc * m) >> 16);
        ch->op[o].fase += inc;
    }
}

void fm_dac(int ligado, int8_t amostra) {
    dac_ligado = ligado;
    dac_valor = amostra;
}

void fm_amostra(int32_t *esquerda, int32_t *direita) {
    if (!tabelas_fm) montar_tabelas_fm();

    if (!tabelas_lfo) montar_lfo();

    /* O LFO anda no seu proprio passo, contado em amostras. */
    if (lfo_ligado && ++lfo_resto >= LFO_PASSOS[lfo_freq]) {
        lfo_resto = 0;
        lfo_pos = (lfo_pos + 1) & 127;
    }
    uint32_t am_bruto = lfo_ligado ? lfo_am() : 0;

    /* O envelope anda a um terco do ritmo das amostras. */
    if (++eg_resto >= EG_DIV) { eg_resto = 0; eg_contador++; avancar_eg(); }

    int32_t soma_e = 0, soma_d = 0;
    for (int c = 0; c < CANAIS; c++) {
        Canal *ch = &canal[c];
        Op *o1 = &ch->op[0], *o2 = &ch->op[1], *o3 = &ch->op[2], *o4 = &ch->op[3];

        /* Realimentacao: a media das duas ultimas saidas do operador 1,
         * deslocada pelo nivel. Media e nao ultima amostra — e' o que suaviza o
         * ruido que a realimentacao crua produziria. */
        int32_t fb = 0;
        if (ch->fb) fb = (o1->saida[0] + o1->saida[1]) >> (10 - ch->fb);

        int32_t s1 = operar(o1, fb, am_bruto);
        o1->saida[1] = o1->saida[0];
        o1->saida[0] = s1;

        int32_t s2, s3, s4, saida;
        /* Canal 6 em modo DAC nao sintetiza: a amostra de 8 bits do driver
         * ocupa o lugar da saida FM dele. E' por ai que passa a percussao. */
        if (c == 5 && dac_ligado) {
            avancar_fase(ch);
            int32_t v = (int32_t)dac_valor * 64;   /* 8 bits com sinal -> 14 bits */
            if (ch->esq) soma_e += v;
            if (ch->dir) soma_d += v;
            continue;
        }
        switch (ch->alg) {
        case 0:  /* 1->2->3->4 */
            s2 = operar(o2, s1 >> 1, am_bruto); s3 = operar(o3, s2 >> 1, am_bruto);
            saida = operar(o4, s3 >> 1, am_bruto); break;
        case 1:  /* (1+2)->3->4 */
            s2 = operar(o2, 0, am_bruto); s3 = operar(o3, (s1 + s2) >> 1, am_bruto);
            saida = operar(o4, s3 >> 1, am_bruto); break;
        case 2:  /* 1 + (2->3) -> 4 */
            s2 = operar(o2, 0, am_bruto); s3 = operar(o3, s2 >> 1, am_bruto);
            saida = operar(o4, (s1 + s3) >> 1, am_bruto); break;
        case 3:  /* (1->2) + 3 -> 4 */
            s2 = operar(o2, s1 >> 1, am_bruto); s3 = operar(o3, 0, am_bruto);
            saida = operar(o4, (s2 + s3) >> 1, am_bruto); break;
        case 4:  /* (1->2) + (3->4) */
            s2 = operar(o2, s1 >> 1, am_bruto); s3 = operar(o3, 0, am_bruto);
            s4 = operar(o4, s3 >> 1, am_bruto); saida = s2 + s4; break;
        case 5:  /* 1 -> (2, 3, 4) */
            s2 = operar(o2, s1 >> 1, am_bruto); s3 = operar(o3, s1 >> 1, am_bruto);
            s4 = operar(o4, s1 >> 1, am_bruto); saida = s2 + s3 + s4; break;
        case 6:  /* (1->2) + 3 + 4 */
            s2 = operar(o2, s1 >> 1, am_bruto); s3 = operar(o3, 0, am_bruto);
            s4 = operar(o4, 0, am_bruto); saida = s2 + s3 + s4; break;
        default: /* 1 + 2 + 3 + 4 */
            s2 = operar(o2, 0, am_bruto); s3 = operar(o3, 0, am_bruto); s4 = operar(o4, 0, am_bruto);
            saida = s1 + s2 + s3 + s4; break;
        }

        avancar_fase(ch);
        /* A saida de um canal e' de 14 bits no chip: somar quatro operadores
         * pode passar disso, e o hardware satura em vez de dar a volta. */
        if (saida > 8191) saida = 8191;
        if (saida < -8192) saida = -8192;
        if (ch->esq) soma_e += saida;
        if (ch->dir) soma_d += saida;
    }
    *esquerda = soma_e;
    *direita = soma_d;
}
