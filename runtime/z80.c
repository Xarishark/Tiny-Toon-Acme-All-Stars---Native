/* Core Z80.
 *
 * A decodificacao usa os campos x/y/z/p/q do opcode em vez de 256 casos escritos
 * a' mao. O mapa do Z80 e' regular: $40-$7F e' inteiro "LD r,r'", $80-$BF e'
 * inteiro "ALU A,r". Escrever isso como dois lacos em vez de 128 casos e' menos
 * codigo e, principalmente, menos lugar onde errar uma letra.
 *
 *   x = op >> 6      y = (op >> 3) & 7     z = op & 7
 *   p = y >> 1       q = y & 1
 *
 * Os prefixos DD e FD trocam HL por IX/IY, e (HL) por (IX+d)/(IY+d). Em vez de
 * duplicar as tabelas, o prefixo e' um parametro que muda o acesso a registrador.
 */

#include "z80.h"

#include <stddef.h>

/* ------------------------------------------------------------------ tabelas */

static uint8_t paridade[256];
static int tabelas_prontas;

static void montar_tabelas(void) {
    for (int i = 0; i < 256; i++) {
        int n = 0;
        for (int b = 0; b < 8; b++) n += (i >> b) & 1;
        paridade[i] = (n & 1) ? 0 : Z80_FP;      /* P = 1 quando PAR */
    }
    tabelas_prontas = 1;
}

/* Ciclos base por opcode principal. Aproximacao suficiente para o ritmo de um
 * driver de som; nao modela espera de barramento nem refresh. */
static const uint8_t CICLOS[256] = {
     4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};

/* ------------------------------------------------------- acesso a memoria */

static uint8_t busca8(Z80 *z) { return z80_ler(z->pc++); }

static uint16_t busca16(Z80 *z) {
    uint16_t v = busca8(z);
    return (uint16_t)(v | (busca8(z) << 8));
}

static uint16_t ler16(uint16_t a) {
    return (uint16_t)(z80_ler(a) | (z80_ler((uint16_t)(a + 1)) << 8));
}

static void escrever16(uint16_t a, uint16_t v) {
    z80_escrever(a, (uint8_t)v);
    z80_escrever((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static void empilhar(Z80 *z, uint16_t v) {
    z->sp -= 2;
    escrever16(z->sp, v);
}

static uint16_t desempilhar(Z80 *z) {
    uint16_t v = ler16(z->sp);
    z->sp += 2;
    return v;
}

/* ------------------------------------------------------ pares e registradores */

static uint16_t par(uint8_t alto, uint8_t baixo) {
    return (uint16_t)((alto << 8) | baixo);
}

static uint16_t *reg_indice(Z80 *z, int pfx) {
    return pfx == 1 ? &z->ix : pfx == 2 ? &z->iy : NULL;
}

/* HL, ou IX/IY quando ha' prefixo. */
static uint16_t ler_hl(Z80 *z, int pfx) {
    uint16_t *ir = reg_indice(z, pfx);
    return ir ? *ir : par(z->h, z->l);
}

static void escrever_hl(Z80 *z, int pfx, uint16_t v) {
    uint16_t *ir = reg_indice(z, pfx);
    if (ir) { *ir = v; }
    else { z->h = (uint8_t)(v >> 8); z->l = (uint8_t)v; }
}

/* rp[] = BC, DE, HL, SP  —  usado por LD rp,nn / INC rp / ADD HL,rp */
static uint16_t ler_rp(Z80 *z, int p, int pfx) {
    switch (p) {
    case 0: return par(z->b, z->c);
    case 1: return par(z->d, z->e);
    case 2: return ler_hl(z, pfx);
    default: return z->sp;
    }
}

static void escrever_rp(Z80 *z, int p, int pfx, uint16_t v) {
    switch (p) {
    case 0: z->b = (uint8_t)(v >> 8); z->c = (uint8_t)v; break;
    case 1: z->d = (uint8_t)(v >> 8); z->e = (uint8_t)v; break;
    case 2: escrever_hl(z, pfx, v); break;
    default: z->sp = v; break;
    }
}

/* rp2[] = BC, DE, HL, AF  —  usado por PUSH/POP */
static uint16_t ler_rp2(Z80 *z, int p, int pfx) {
    if (p == 3) return par(z->a, z->f);
    return ler_rp(z, p, pfx);
}

static void escrever_rp2(Z80 *z, int p, int pfx, uint16_t v) {
    if (p == 3) { z->a = (uint8_t)(v >> 8); z->f = (uint8_t)v; return; }
    escrever_rp(z, p, pfx, v);
}

/* r[] = B, C, D, E, H, L, (HL), A.
 *
 * Com prefixo, os indices 4 e 5 viram as metades de IX/IY, e o 6 vira (IX+d).
 * O deslocamento ja' veio buscado pelo chamador, porque em DDCB ele aparece
 * ANTES do opcode — ordem que so' existe nesse prefixo e e' facil de errar. */
static uint8_t ler_r(Z80 *z, int r, int pfx, uint16_t ea) {
    uint16_t *ir = reg_indice(z, pfx);
    switch (r) {
    case 0: return z->b;
    case 1: return z->c;
    case 2: return z->d;
    case 3: return z->e;
    case 4: return ir ? (uint8_t)(*ir >> 8) : z->h;
    case 5: return ir ? (uint8_t)*ir : z->l;
    case 6: return z80_ler(ea);
    default: return z->a;
    }
}

static void escrever_r(Z80 *z, int r, int pfx, uint16_t ea, uint8_t v) {
    uint16_t *ir = reg_indice(z, pfx);
    switch (r) {
    case 0: z->b = v; break;
    case 1: z->c = v; break;
    case 2: z->d = v; break;
    case 3: z->e = v; break;
    case 4: if (ir) *ir = (uint16_t)((*ir & 0x00FF) | (v << 8)); else z->h = v; break;
    case 5: if (ir) *ir = (uint16_t)((*ir & 0xFF00) | v); else z->l = v; break;
    case 6: z80_escrever(ea, v); break;
    default: z->a = v; break;
    }
}

/* Quando o operando e' (HL)/(IX+d), "H" e "L" usados por LD r,r' voltam a ser os
 * de verdade — "LD (IX+d),H" grava H, nao IXH. Essa excecao existe so' aqui. */
static uint8_t ler_r_sem_indice(Z80 *z, int r, int pfx, uint16_t ea) {
    if (r == 4) return z->h;
    if (r == 5) return z->l;
    return ler_r(z, r, pfx, ea);
}

static void escrever_r_sem_indice(Z80 *z, int r, int pfx, uint16_t ea, uint8_t v) {
    if (r == 4) { z->h = v; return; }
    if (r == 5) { z->l = v; return; }
    escrever_r(z, r, pfx, ea, v);
}

/* ------------------------------------------------------------------- flags */

static void flags_sz(Z80 *z, uint8_t v) {
    z->f = (uint8_t)((z->f & ~(Z80_FS | Z80_FZ | Z80_FX | Z80_FY))
                     | (v & (Z80_FS | Z80_FX | Z80_FY))
                     | (v ? 0 : Z80_FZ));
}

static void alu(Z80 *z, int op, uint8_t v) {
    uint8_t a = z->a;
    unsigned r;
    uint8_t f = 0;

    switch (op) {
    case 0: case 1: {                       /* ADD / ADC */
        unsigned c = (op == 1) ? (z->f & Z80_FC) : 0;
        r = a + v + c;
        f = (uint8_t)((r & 0x100) ? Z80_FC : 0);
        f |= (uint8_t)(((a ^ v ^ r) & 0x10) ? Z80_FH : 0);
        f |= (uint8_t)((((a ^ r) & (v ^ r) & 0x80)) ? Z80_FP : 0);
        break;
    }
    case 2: case 3: case 7: {               /* SUB / SBC / CP */
        unsigned c = (op == 3) ? (z->f & Z80_FC) : 0;
        r = a - v - c;
        f = (uint8_t)((r & 0x100) ? Z80_FC : 0);
        f |= (uint8_t)(((a ^ v ^ r) & 0x10) ? Z80_FH : 0);
        f |= (uint8_t)((((a ^ v) & (a ^ r) & 0x80)) ? Z80_FP : 0);
        f |= Z80_FN;
        break;
    }
    case 4: r = a & v; f = Z80_FH | paridade[r & 0xFF]; break;
    case 5: r = a ^ v; f = paridade[r & 0xFF]; break;
    default: r = a | v; f = paridade[r & 0xFF]; break;
    }

    uint8_t res = (uint8_t)r;
    f |= (uint8_t)(res & Z80_FS);
    f |= (uint8_t)(res ? 0 : Z80_FZ);
    /* CP copia X e Y do OPERANDO, nao do resultado. E' a unica instrucao que faz
     * isso, e o zexdoc cobra. */
    f |= (uint8_t)((op == 7 ? v : res) & (Z80_FX | Z80_FY));
    z->f = f;
    if (op != 7) z->a = res;
}

static uint8_t inc8(Z80 *z, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    z->f = (uint8_t)((z->f & Z80_FC)
                     | (r & (Z80_FS | Z80_FX | Z80_FY))
                     | (r ? 0 : Z80_FZ)
                     | ((r & 0x0F) == 0 ? Z80_FH : 0)
                     | (r == 0x80 ? Z80_FP : 0));
    return r;
}

static uint8_t dec8(Z80 *z, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    z->f = (uint8_t)((z->f & Z80_FC)
                     | (r & (Z80_FS | Z80_FX | Z80_FY))
                     | (r ? 0 : Z80_FZ)
                     | ((r & 0x0F) == 0x0F ? Z80_FH : 0)
                     | (r == 0x7F ? Z80_FP : 0)
                     | Z80_FN);
    return r;
}

static uint16_t add16(Z80 *z, uint16_t a, uint16_t b) {
    unsigned r = a + b;
    z->f = (uint8_t)((z->f & (Z80_FS | Z80_FZ | Z80_FP))
                     | (((a ^ b ^ r) & 0x1000) ? Z80_FH : 0)
                     | ((r & 0x10000) ? Z80_FC : 0)
                     | ((r >> 8) & (Z80_FX | Z80_FY)));
    return (uint16_t)r;
}

static uint16_t adc16(Z80 *z, uint16_t a, uint16_t b) {
    unsigned c = z->f & Z80_FC;
    unsigned r = a + b + c;
    uint16_t res = (uint16_t)r;
    z->f = (uint8_t)((((a ^ b ^ r) & 0x1000) ? Z80_FH : 0)
                     | ((r & 0x10000) ? Z80_FC : 0)
                     | (((a ^ r) & (b ^ r) & 0x8000) ? Z80_FP : 0)
                     | (res ? 0 : Z80_FZ)
                     | ((res >> 8) & (Z80_FS | Z80_FX | Z80_FY)));
    return res;
}

static uint16_t sbc16(Z80 *z, uint16_t a, uint16_t b) {
    unsigned c = z->f & Z80_FC;
    unsigned r = a - b - c;
    uint16_t res = (uint16_t)r;
    z->f = (uint8_t)((((a ^ b ^ r) & 0x1000) ? Z80_FH : 0)
                     | ((r & 0x10000) ? Z80_FC : 0)
                     | (((a ^ b) & (a ^ r) & 0x8000) ? Z80_FP : 0)
                     | (res ? 0 : Z80_FZ)
                     | ((res >> 8) & (Z80_FS | Z80_FX | Z80_FY))
                     | Z80_FN);
    return res;
}

/* rot[] = RLC, RRC, RL, RR, SLA, SRA, SLL, SRL */
static uint8_t rot(Z80 *z, int op, uint8_t v) {
    uint8_t c;
    uint8_t r;
    switch (op) {
    case 0: c = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | c); break;
    case 1: c = (uint8_t)(v & 1); r = (uint8_t)((v >> 1) | (c << 7)); break;
    case 2: c = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | (z->f & Z80_FC)); break;
    case 3: c = (uint8_t)(v & 1); r = (uint8_t)((v >> 1) | ((z->f & Z80_FC) << 7)); break;
    case 4: c = (uint8_t)(v >> 7); r = (uint8_t)(v << 1); break;
    case 5: c = (uint8_t)(v & 1); r = (uint8_t)((v >> 1) | (v & 0x80)); break;
    /* SLL e' indocumentada: desloca para a esquerda e mete 1 no bit 0. */
    case 6: c = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | 1); break;
    default: c = (uint8_t)(v & 1); r = (uint8_t)(v >> 1); break;
    }
    z->f = (uint8_t)(c | (r & (Z80_FS | Z80_FX | Z80_FY))
                     | (r ? 0 : Z80_FZ) | paridade[r]);
    return r;
}

static void daa(Z80 *z) {
    uint8_t a = z->a;
    uint8_t ajuste = 0;
    uint8_t carry = z->f & Z80_FC;

    if ((z->f & Z80_FH) || (a & 0x0F) > 9) ajuste |= 0x06;
    if (carry || a > 0x99) { ajuste |= 0x60; carry = Z80_FC; }

    uint8_t r = (z->f & Z80_FN) ? (uint8_t)(a - ajuste) : (uint8_t)(a + ajuste);
    uint8_t h = (z->f & Z80_FN)
              ? (((z->f & Z80_FH) && (a & 0x0F) < 6) ? Z80_FH : 0)
              : (((a & 0x0F) > 9) ? Z80_FH : 0);

    z->a = r;
    z->f = (uint8_t)(carry | (z->f & Z80_FN) | h
                     | (r & (Z80_FS | Z80_FX | Z80_FY))
                     | (r ? 0 : Z80_FZ) | paridade[r]);
}

static int condicao(Z80 *z, int cc) {
    switch (cc) {
    case 0: return !(z->f & Z80_FZ);
    case 1: return  (z->f & Z80_FZ) != 0;
    case 2: return !(z->f & Z80_FC);
    case 3: return  (z->f & Z80_FC) != 0;
    case 4: return !(z->f & Z80_FP);
    case 5: return  (z->f & Z80_FP) != 0;
    case 6: return !(z->f & Z80_FS);
    default: return (z->f & Z80_FS) != 0;
    }
}

/* --------------------------------------------------------------- prefixo CB */

static unsigned executar_cb(Z80 *z, int pfx, uint16_t ea) {
    uint8_t op = busca8(z);
    int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
    unsigned ciclos = pfx ? 15 : (r == 6 ? 15 : 8);

    /* Com prefixo, a operacao e' sempre sobre (IX+d); se r != 6, o resultado
     * TAMBEM vai para o registrador r. Indocumentado, mas e' o que o silicio faz
     * e alguns drivers de som usam sem saber. */
    uint8_t v = pfx ? z80_ler(ea) : ler_r(z, r, 0, ea);

    if (x == 1) {                                    /* BIT */
        uint8_t b = (uint8_t)(v & (1u << y));
        z->f = (uint8_t)((z->f & Z80_FC) | Z80_FH
                         | (b ? (b & Z80_FS) : (Z80_FZ | Z80_FP)));
        /* X e Y vem do operando; com (IX+d), do byte alto do endereco. */
        z->f |= (uint8_t)((pfx ? (ea >> 8) : v) & (Z80_FX | Z80_FY));
        return pfx ? 20 : (r == 6 ? 12 : 8);
    }

    uint8_t res;
    if (x == 0) res = rot(z, y, v);
    else if (x == 2) res = (uint8_t)(v & ~(1u << y));
    else res = (uint8_t)(v | (1u << y));

    if (pfx) {
        z80_escrever(ea, res);
        if (r != 6) escrever_r(z, r, 0, ea, res);
        return 23;
    }
    escrever_r(z, r, 0, ea, res);
    return ciclos;
}

/* --------------------------------------------------------------- prefixo ED */

static unsigned executar_ed(Z80 *z) {
    uint8_t op = busca8(z);
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {
        switch (zz) {
        case 0: {                                    /* IN r,(C) */
            uint8_t v = z80_entrada(par(z->b, z->c));
            if (y != 6) escrever_r(z, y, 0, 0, v);
            z->f = (uint8_t)((z->f & Z80_FC) | (v & (Z80_FS | Z80_FX | Z80_FY))
                             | (v ? 0 : Z80_FZ) | paridade[v]);
            return 12;
        }
        case 1:                                      /* OUT (C),r */
            z80_saida(par(z->b, z->c), y == 6 ? 0 : ler_r(z, y, 0, 0));
            return 12;
        case 2: {                                    /* SBC/ADC HL,rp */
            uint16_t hl = par(z->h, z->l);
            uint16_t v = ler_rp(z, p, 0);
            uint16_t r = q ? adc16(z, hl, v) : sbc16(z, hl, v);
            z->h = (uint8_t)(r >> 8); z->l = (uint8_t)r;
            return 15;
        }
        case 3: {                                    /* LD (nn),rp / LD rp,(nn) */
            uint16_t nn = busca16(z);
            if (q) escrever_rp(z, p, 0, ler16(nn));
            else escrever16(nn, ler_rp(z, p, 0));
            return 20;
        }
        case 4: {                                    /* NEG */
            uint8_t a = z->a;
            z->a = 0;
            alu(z, 2, a);
            return 8;
        }
        case 5:                                      /* RETN / RETI */
            z->pc = desempilhar(z);
            z->iff1 = z->iff2;
            return 14;
        case 6:                                      /* IM n */
            z->im = (uint8_t)((y == 0 || y == 1 || y == 4 || y == 5) ? 0
                              : (y == 2 || y == 6) ? 1 : 2);
            return 8;
        default:
            switch (y) {
            case 0: z->i = z->a; return 9;
            case 1: z->r = z->a; return 9;
            case 2: case 3: {                        /* LD A,I / LD A,R */
                uint8_t v = (y == 2) ? z->i : z->r;
                z->a = v;
                z->f = (uint8_t)((z->f & Z80_FC)
                                 | (v & (Z80_FS | Z80_FX | Z80_FY))
                                 | (v ? 0 : Z80_FZ)
                                 | (z->iff2 ? Z80_FP : 0));
                return 9;
            }
            case 4: case 5: {                        /* RRD / RLD */
                uint16_t hl = par(z->h, z->l);
                uint8_t m = z80_ler(hl);
                uint8_t a = z->a;
                if (y == 4) {
                    z80_escrever(hl, (uint8_t)((a << 4) | (m >> 4)));
                    z->a = (uint8_t)((a & 0xF0) | (m & 0x0F));
                } else {
                    z80_escrever(hl, (uint8_t)((m << 4) | (a & 0x0F)));
                    z->a = (uint8_t)((a & 0xF0) | (m >> 4));
                }
                z->f = (uint8_t)((z->f & Z80_FC)
                                 | (z->a & (Z80_FS | Z80_FX | Z80_FY))
                                 | (z->a ? 0 : Z80_FZ) | paridade[z->a]);
                return 18;
            }
            default: return 8;
            }
        }
    }

    if (x == 2 && zz <= 3 && y >= 4) {                /* blocos */
        uint16_t hl = par(z->h, z->l);
        uint16_t bc = par(z->b, z->c);
        uint16_t de = par(z->d, z->e);
        int incrementa = !(y & 1);                    /* LDI/LDD, CPI/CPD, ... */
        int repete = (y & 2) != 0;
        int passo = incrementa ? 1 : -1;

        switch (zz) {
        case 0: {                                    /* LDI/LDD/LDIR/LDDR */
            uint8_t v = z80_ler(hl);
            z80_escrever(de, v);
            hl = (uint16_t)(hl + passo); de = (uint16_t)(de + passo);
            bc = (uint16_t)(bc - 1);
            uint8_t n = (uint8_t)(v + z->a);
            z->f = (uint8_t)((z->f & (Z80_FS | Z80_FZ | Z80_FC))
                             | ((n & 0x02) ? Z80_FY : 0)
                             | ((n & 0x08) ? Z80_FX : 0)
                             | (bc ? Z80_FP : 0));
            z->h = (uint8_t)(hl >> 8); z->l = (uint8_t)hl;
            z->d = (uint8_t)(de >> 8); z->e = (uint8_t)de;
            z->b = (uint8_t)(bc >> 8); z->c = (uint8_t)bc;
            if (repete && bc) { z->pc -= 2; return 21; }
            return 16;
        }
        case 1: {                                    /* CPI/CPD/CPIR/CPDR */
            uint8_t v = z80_ler(hl);
            uint8_t r = (uint8_t)(z->a - v);
            uint8_t h = (uint8_t)(((z->a ^ v ^ r) & 0x10) ? Z80_FH : 0);
            hl = (uint16_t)(hl + passo);
            bc = (uint16_t)(bc - 1);
            uint8_t n = (uint8_t)(r - (h ? 1 : 0));
            z->f = (uint8_t)((z->f & Z80_FC) | Z80_FN | h
                             | (r & Z80_FS) | (r ? 0 : Z80_FZ)
                             | ((n & 0x02) ? Z80_FY : 0)
                             | ((n & 0x08) ? Z80_FX : 0)
                             | (bc ? Z80_FP : 0));
            z->h = (uint8_t)(hl >> 8); z->l = (uint8_t)hl;
            z->b = (uint8_t)(bc >> 8); z->c = (uint8_t)bc;
            if (repete && bc && r) { z->pc -= 2; return 21; }
            return 16;
        }
        case 2: {                                    /* INI/IND/INIR/INDR */
            uint8_t v = z80_entrada(bc);
            z80_escrever(hl, v);
            hl = (uint16_t)(hl + passo);
            z->b = (uint8_t)(z->b - 1);
            z->h = (uint8_t)(hl >> 8); z->l = (uint8_t)hl;
            flags_sz(z, z->b);
            z->f |= Z80_FN;
            if (repete && z->b) { z->pc -= 2; return 21; }
            return 16;
        }
        default: {                                   /* OUTI/OUTD/OTIR/OTDR */
            uint8_t v = z80_ler(hl);
            z->b = (uint8_t)(z->b - 1);
            z80_saida(par(z->b, z->c), v);
            hl = (uint16_t)(hl + passo);
            z->h = (uint8_t)(hl >> 8); z->l = (uint8_t)hl;
            flags_sz(z, z->b);
            z->f |= Z80_FN;
            if (repete && z->b) { z->pc -= 2; return 21; }
            return 16;
        }
        }
    }
    return 8;                                        /* ED indefinido = NOP */
}

/* ------------------------------------------------------------- tabela principal */

static unsigned executar(Z80 *z, int pfx) {
    uint8_t op = busca8(z);
    z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));

    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    unsigned ciclos = CICLOS[op] + (pfx ? 4u : 0u);

    /* (IX+d) precisa do deslocamento buscado antes de qualquer acesso. Fora do
     * prefixo, ea e' simplesmente HL. */
    uint16_t ea = 0;
    /* Quais opcodes de fato tocam (HL). Errar isto com prefixo e' fatal: buscar
     * um deslocamento que nao existe desalinha o resto da instrucao. "DD 06 nn"
     * e' LD B,n e nao tem deslocamento nenhum. */
    int usa_memoria = 0;
    if (x == 0 && (zz == 4 || zz == 5 || zz == 6)) usa_memoria = (y == 6);
    else if (x == 1) usa_memoria = (y == 6 || zz == 6);
    else if (x == 2) usa_memoria = (zz == 6);
    else if (x == 3 && zz == 3 && y == 1) usa_memoria = 1;   /* DDCB */

    if (pfx && usa_memoria) {
        int8_t d = (int8_t)busca8(z);
        ea = (uint16_t)(ler_hl(z, pfx) + d);
        ciclos += 8;
    } else if (!pfx) {
        ea = par(z->h, z->l);
    } else {
        ea = ler_hl(z, pfx);
    }

    switch (x) {
    case 0:
        switch (zz) {
        case 0:
            if (y == 0) return ciclos;                       /* NOP */
            if (y == 1) {                                    /* EX AF,AF' */
                uint8_t t;
                t = z->a; z->a = z->a_; z->a_ = t;
                t = z->f; z->f = z->f_; z->f_ = t;
                return ciclos;
            }
            if (y == 2) {                                    /* DJNZ */
                int8_t d = (int8_t)busca8(z);
                z->b = (uint8_t)(z->b - 1);
                if (z->b) { z->pc = (uint16_t)(z->pc + d); return 13; }
                return 8;
            }
            if (y == 3) {                                    /* JR d */
                int8_t d = (int8_t)busca8(z);
                z->pc = (uint16_t)(z->pc + d);
                return 12;
            }
            {                                                /* JR cc,d */
                int8_t d = (int8_t)busca8(z);
                if (condicao(z, y - 4)) { z->pc = (uint16_t)(z->pc + d); return 12; }
                return 7;
            }
        case 1:
            if (q) {
                escrever_hl(z, pfx, add16(z, ler_hl(z, pfx), ler_rp(z, p, pfx)));
            } else {
                escrever_rp(z, p, pfx, busca16(z));
            }
            return ciclos;
        case 2: {
            uint16_t hl = ler_hl(z, pfx);
            if (!q) {
                switch (p) {
                case 0: z80_escrever(par(z->b, z->c), z->a); break;
                case 1: z80_escrever(par(z->d, z->e), z->a); break;
                case 2: escrever16(busca16(z), hl); break;
                default: z80_escrever(busca16(z), z->a); break;
                }
            } else {
                switch (p) {
                case 0: z->a = z80_ler(par(z->b, z->c)); break;
                case 1: z->a = z80_ler(par(z->d, z->e)); break;
                case 2: escrever_hl(z, pfx, ler16(busca16(z))); break;
                default: z->a = z80_ler(busca16(z)); break;
                }
            }
            return ciclos;
        }
        case 3:
            escrever_rp(z, p, pfx, (uint16_t)(ler_rp(z, p, pfx) + (q ? -1 : 1)));
            return ciclos;
        case 4:
            escrever_r(z, y, pfx, ea, inc8(z, ler_r(z, y, pfx, ea)));
            return ciclos;
        case 5:
            escrever_r(z, y, pfx, ea, dec8(z, ler_r(z, y, pfx, ea)));
            return ciclos;
        case 6: {
            /* Com prefixo o deslocamento vem ANTES do imediato: DD 36 d n. */
            uint8_t n = busca8(z);
            escrever_r(z, y, pfx, ea, n);
            return ciclos;
        }
        default:
            switch (y) {
            case 0: case 1: case 2: case 3: {                /* RLCA/RRCA/RLA/RRA */
                /* A forma sobre A preserva S, Z e P/V — so' a do prefixo CB os
                 * altera. Reaproveitar rot() e depois restaurar os tres e' o
                 * jeito de nao ter duas implementacoes de rotacao. */
                uint8_t antes = z->f;
                uint8_t v = rot(z, y, z->a);
                z->a = v;
                z->f = (uint8_t)((z->f & Z80_FC)
                                 | (antes & (Z80_FS | Z80_FZ | Z80_FP))
                                 | (v & (Z80_FX | Z80_FY)));
                return ciclos;
            }
            case 4: daa(z); return ciclos;
            case 5:                                          /* CPL */
                z->a = (uint8_t)~z->a;
                z->f = (uint8_t)((z->f & (Z80_FS | Z80_FZ | Z80_FP | Z80_FC))
                                 | Z80_FH | Z80_FN
                                 | (z->a & (Z80_FX | Z80_FY)));
                return ciclos;
            case 6:                                          /* SCF */
                z->f = (uint8_t)((z->f & (Z80_FS | Z80_FZ | Z80_FP))
                                 | Z80_FC | (z->a & (Z80_FX | Z80_FY)));
                return ciclos;
            default:                                         /* CCF */
                z->f = (uint8_t)((z->f & (Z80_FS | Z80_FZ | Z80_FP))
                                 | ((z->f & Z80_FC) ? Z80_FH : 0)
                                 | ((z->f & Z80_FC) ? 0 : Z80_FC)
                                 | (z->a & (Z80_FX | Z80_FY)));
                return ciclos;
            }
        }

    case 1:
        if (y == 6 && zz == 6) {                             /* HALT */
            z->parado = 1;
            return ciclos;
        }
        /* Se um dos lados e' (HL)/(IX+d), o outro usa H/L de verdade. */
        if (pfx && (y == 6 || zz == 6)) {
            escrever_r_sem_indice(z, y, pfx, ea, ler_r_sem_indice(z, zz, pfx, ea));
        } else {
            escrever_r(z, y, pfx, ea, ler_r(z, zz, pfx, ea));
        }
        return ciclos;

    case 2:
        alu(z, y, ler_r(z, zz, pfx, ea));
        return ciclos;

    default:
        switch (zz) {
        case 0:
            if (condicao(z, y)) { z->pc = desempilhar(z); return 11; }
            return 5;
        case 1:
            if (!q) { escrever_rp2(z, p, pfx, desempilhar(z)); return ciclos; }
            switch (p) {
            case 0: z->pc = desempilhar(z); return 10;
            case 1: {                                        /* EXX */
                uint8_t t;
                t = z->b; z->b = z->b_; z->b_ = t;
                t = z->c; z->c = z->c_; z->c_ = t;
                t = z->d; z->d = z->d_; z->d_ = t;
                t = z->e; z->e = z->e_; z->e_ = t;
                t = z->h; z->h = z->h_; z->h_ = t;
                t = z->l; z->l = z->l_; z->l_ = t;
                return 4;
            }
            case 2: z->pc = ler_hl(z, pfx); return pfx ? 8 : 4;
            default: z->sp = ler_hl(z, pfx); return pfx ? 10 : 6;
            }
        case 2: {
            uint16_t nn = busca16(z);
            if (condicao(z, y)) z->pc = nn;
            return 10;
        }
        case 3:
            switch (y) {
            case 0: z->pc = busca16(z); return 10;
            case 1: return executar_cb(z, pfx, ea);
            case 2: {                                        /* OUT (n),A */
                uint8_t n = busca8(z);
                z80_saida((uint16_t)((z->a << 8) | n), z->a);
                return 11;
            }
            case 3: {                                        /* IN A,(n) */
                uint8_t n = busca8(z);
                z->a = z80_entrada((uint16_t)((z->a << 8) | n));
                return 11;
            }
            case 4: {                                        /* EX (SP),HL */
                uint16_t t = ler16(z->sp);
                escrever16(z->sp, ler_hl(z, pfx));
                escrever_hl(z, pfx, t);
                return pfx ? 23 : 19;
            }
            case 5: {                                        /* EX DE,HL */
                uint8_t t;
                t = z->d; z->d = z->h; z->h = t;
                t = z->e; z->e = z->l; z->l = t;
                return 4;
            }
            case 6: z->iff1 = z->iff2 = 0; return 4;
            default: z->iff1 = z->iff2 = 1; return 4;
            }
        case 4: {
            uint16_t nn = busca16(z);
            if (condicao(z, y)) { empilhar(z, z->pc); z->pc = nn; return 17; }
            return 10;
        }
        case 5:
            if (!q) { empilhar(z, ler_rp2(z, p, pfx)); return ciclos; }
            switch (p) {
            case 0: {
                uint16_t nn = busca16(z);
                empilhar(z, z->pc);
                z->pc = nn;
                return 17;
            }
            case 1: return executar(z, 1);                   /* DD */
            case 2: return executar_ed(z);                   /* ED */
            default: return executar(z, 2);                  /* FD */
            }
        case 6:
            alu(z, y, busca8(z));
            return ciclos;
        default:
            empilhar(z, z->pc);
            z->pc = (uint16_t)(y * 8);
            return 11;
        }
    }
}

/* ------------------------------------------------------------------ interface */

void z80_reset(Z80 *z) {
    if (!tabelas_prontas) montar_tabelas();
    for (unsigned i = 0; i < sizeof *z; i++) ((uint8_t *)z)[i] = 0;
    z->f = 0;
    z->sp = 0xFFFF;
    z->ix = z->iy = 0xFFFF;
    z->a = 0xFF;
}

void z80_interromper(Z80 *z) { z->irq_pendente = 1; }

unsigned z80_passo(Z80 *z) {
    if (!tabelas_prontas) montar_tabelas();

    if (z->irq_pendente && z->iff1) {
        z->irq_pendente = 0;
        z->parado = 0;
        z->iff1 = z->iff2 = 0;
        empilhar(z, z->pc);
        /* No Mega Drive o Z80 so' usa modo 1: vetor fixo em $0038. Modo 0 e 2
         * existem no chip, mas nao ha' quem forneca o byte de vetor aqui. */
        z->pc = (z->im == 2) ? ler16((uint16_t)((z->i << 8) | 0xFF)) : 0x0038;
        z->ciclos += 13;
        return 13;
    }

    if (z->parado) { z->ciclos += 4; return 4; }

    unsigned c = executar(z, 0);
    z->ciclos += c;
    return c;
}

unsigned z80_rodar(Z80 *z, unsigned ciclos) {
    unsigned gastos = 0;
    while (gastos < ciclos) gastos += z80_passo(z);
    return gastos;
}
