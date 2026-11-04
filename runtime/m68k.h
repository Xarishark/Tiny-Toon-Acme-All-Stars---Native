/* Estado da CPU 68000 e helpers para o codigo recompilado.
 *
 * Principio: primeiro reproduzir fielmente. As flags sao calculadas de forma
 * *ansiosa*, uma a uma, a cada instrucao que as afeta. E' mais lento que
 * calcular sob demanda, e e' de proposito: enquanto nao houver um harness de
 * comparacao contra hardware, correcao vale mais que velocidade.
 *
 * Os registradores sao uint32_t e o acesso por tamanho passa pelos helpers,
 * porque no 68000 escrever .b ou .w num registrador de dados **preserva** os
 * bits de cima. Escrever direto seria o erro mais facil de cometer aqui.
 */

#ifndef TINYTOON_M68K_H
#define TINYTOON_M68K_H

#include <stdint.h>

typedef struct M68K {
    uint32_t d[8];
    uint32_t a[8];      /* a[7] e' o stack pointer */
    uint8_t n, z, v, c, x;
    uint16_t sr_high;   /* bits 8-15 de SR: mascara de interrupcao, S, T */
    uint32_t usp;       /* user stack pointer, so' tocado por MOVE An,USP */
    /* Niveis de retorno a pular.
     *
     * Duas rotinas do jogo desempilham um long para um registrador de dados e logo dao rts: desempilham
     * o proprio endereco de retorno para voltar DOIS niveis acima. Na traducao
     * para C, onde jsr virou chamada de funcao, isso nao acontece sozinho — a
     * rotina marca aqui, e cada chamador confere depois de chamar. Ver
     * analysis/stack-returns.csv. */
    uint8_t unwind;
    /* Relogio. Anda em ciclos do 68000, emitidos instrucao a instrucao pelo
     * recompilador — ver tools/ciclos.py. Antes o runtime media o tempo em
     * acessos a memoria, o que faz uma instrucao de 20 ciclos adiantar o
     * relogio tanto quanto uma de 4. */
    uint32_t ciclos;
} M68K;

/* SR e CCR sao vistas das mesmas flags. Guardar as flags separadas e montar a
 * word quando alguem pede e' mais rapido no caso comum, que e' testar uma
 * flag; o jogo le' SR inteira poucas vezes, quase sempre para mascarar
 * interrupcao no V-blank. */
static inline uint16_t get_sr(const M68K *cpu) {
    return (uint16_t)(cpu->sr_high | (cpu->x << 4) | (cpu->n << 3)
                      | (cpu->z << 2) | (cpu->v << 1) | cpu->c);
}
static inline void set_sr(M68K *cpu, uint16_t v) {
    /* Bit 13 e' o S de supervisor, e mudar de modo TROCA O A7: o 68000 tem dois
     * ponteiros de pilha e so' um fica visivel de cada vez. Sem isso, escrever
     * um SR com S diferente deixa o codigo usando a pilha errada — e o estrago
     * so' aparece muito depois, no primeiro rts.
     *
     * O jogo roda sempre em supervisor, entao na pratica isto nunca dispara.
     * Esta aqui porque "nunca dispara" e' uma afirmacao sobre o jogo, nao sobre
     * a CPU, e o teste diferencial compara contra a CPU. */
    uint16_t antes = cpu->sr_high;
    cpu->sr_high = v & 0xFF00u;
    if ((antes ^ cpu->sr_high) & 0x2000u) {
        uint32_t t = cpu->a[7];
        cpu->a[7] = cpu->usp;
        cpu->usp = t;
    }
    cpu->x = (v >> 4) & 1; cpu->n = (v >> 3) & 1; cpu->z = (v >> 2) & 1;
    cpu->v = (v >> 1) & 1; cpu->c = v & 1;
}
static inline uint8_t get_ccr(const M68K *cpu) { return (uint8_t)get_sr(cpu); }
static inline void set_ccr(M68K *cpu, uint8_t v) {
    set_sr(cpu, (uint16_t)(cpu->sr_high | (v & 0x1Fu)));
}

/* -- acesso a registrador de dados por tamanho -------------------------- */

static inline uint8_t d8(const M68K *cpu, int r)   { return (uint8_t)cpu->d[r]; }
static inline uint16_t d16(const M68K *cpu, int r) { return (uint16_t)cpu->d[r]; }
static inline uint32_t d32(const M68K *cpu, int r) { return cpu->d[r]; }

static inline void set_d8(M68K *cpu, int r, uint8_t v) {
    cpu->d[r] = (cpu->d[r] & 0xFFFFFF00u) | v;
}
static inline void set_d16(M68K *cpu, int r, uint16_t v) {
    cpu->d[r] = (cpu->d[r] & 0xFFFF0000u) | v;
}
static inline void set_d32(M68K *cpu, int r, uint32_t v) { cpu->d[r] = v; }

/* Escrita em registrador de endereco e' sempre estendida com sinal para 32
 * bits, mesmo na forma .w — regra do 68000 que nao tem equivalente nos
 * registradores de dados. */
static inline void set_a16(M68K *cpu, int r, uint16_t v) {
    cpu->a[r] = (uint32_t)(int32_t)(int16_t)v;
}
static inline void set_a32(M68K *cpu, int r, uint32_t v) { cpu->a[r] = v; }

/* -- flags --------------------------------------------------------------- */

/* Operacoes logicas e move: N e Z pelo resultado, V e C zerados, X intacto. */
static inline void flags_logic8(M68K *cpu, uint8_t v) {
    cpu->n = (v >> 7) & 1; cpu->z = (v == 0); cpu->v = 0; cpu->c = 0;
}
static inline void flags_logic16(M68K *cpu, uint16_t v) {
    cpu->n = (v >> 15) & 1; cpu->z = (v == 0); cpu->v = 0; cpu->c = 0;
}
static inline void flags_logic32(M68K *cpu, uint32_t v) {
    cpu->n = (v >> 31) & 1; cpu->z = (v == 0); cpu->v = 0; cpu->c = 0;
}

/* Soma: X acompanha C. */
static inline uint32_t flags_add32(M68K *cpu, uint32_t a, uint32_t b) {
    uint64_t wide = (uint64_t)a + b;
    uint32_t r = (uint32_t)wide;
    cpu->c = cpu->x = (wide >> 32) & 1;
    cpu->v = (((a ^ r) & (b ^ r)) >> 31) & 1;
    cpu->n = (r >> 31) & 1;
    cpu->z = (r == 0);
    return r;
}
static inline uint16_t flags_add16(M68K *cpu, uint16_t a, uint16_t b) {
    uint32_t wide = (uint32_t)a + b;
    uint16_t r = (uint16_t)wide;
    cpu->c = cpu->x = (wide >> 16) & 1;
    cpu->v = (((a ^ r) & (b ^ r)) >> 15) & 1;
    cpu->n = (r >> 15) & 1;
    cpu->z = (r == 0);
    return r;
}

/* Subtracao: C e' emprestimo. */
static inline uint16_t flags_sub16(M68K *cpu, uint16_t a, uint16_t b) {
    uint32_t wide = (uint32_t)a - b;
    uint16_t r = (uint16_t)wide;
    cpu->c = cpu->x = (wide >> 16) & 1;
    cpu->v = (((a ^ b) & (a ^ r)) >> 15) & 1;
    cpu->n = (r >> 15) & 1;
    cpu->z = (r == 0);
    return r;
}

static inline uint8_t flags_add8(M68K *cpu, uint8_t a, uint8_t b) {
    uint16_t wide = (uint16_t)a + b;
    uint8_t r = (uint8_t)wide;
    cpu->c = cpu->x = (wide >> 8) & 1;
    cpu->v = (((a ^ r) & (b ^ r)) >> 7) & 1;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0);
    return r;
}
static inline uint8_t flags_sub8(M68K *cpu, uint8_t a, uint8_t b) {
    uint16_t wide = (uint16_t)a - b;
    uint8_t r = (uint8_t)wide;
    cpu->c = cpu->x = (wide >> 8) & 1;
    cpu->v = (((a ^ b) & (a ^ r)) >> 7) & 1;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0);
    return r;
}
static inline uint32_t flags_sub32(M68K *cpu, uint32_t a, uint32_t b) {
    uint64_t wide = (uint64_t)a - b;
    uint32_t r = (uint32_t)wide;
    cpu->c = cpu->x = (wide >> 32) & 1;
    cpu->v = (((a ^ b) & (a ^ r)) >> 31) & 1;
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0);
    return r;
}

/* CMP e TST nao escrevem o destino e, ao contrario de SUB, NAO tocam X. */
static inline void flags_cmp8(M68K *cpu, uint8_t a, uint8_t b) {
    uint8_t r = (uint8_t)(a - b);
    cpu->c = a < b;
    cpu->v = (((a ^ b) & (a ^ r)) >> 7) & 1;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0);
}
static inline void flags_cmp16(M68K *cpu, uint16_t a, uint16_t b) {
    uint16_t r = (uint16_t)(a - b);
    cpu->c = a < b;
    cpu->v = (((a ^ b) & (a ^ r)) >> 15) & 1;
    cpu->n = (r >> 15) & 1; cpu->z = (r == 0);
}
static inline void flags_cmp32(M68K *cpu, uint32_t a, uint32_t b) {
    uint32_t r = (uint32_t)(a - b);
    cpu->c = a < b;
    cpu->v = (((a ^ b) & (a ^ r)) >> 31) & 1;
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0);
}

/* NEG: subtrai de zero. NOT e EXT so' mexem em N e Z. */
static inline uint16_t op_neg16(M68K *cpu, uint16_t v) { return flags_sub16(cpu, 0, v); }
static inline uint32_t op_neg32(M68K *cpu, uint32_t v) { return flags_sub32(cpu, 0, v); }

/* Deslocamentos. C recebe o ultimo bit que saiu; contagem zero deixa C em 0
 * e nao toca X, detalhe do 68000 que e' facil de perder. */
static inline uint8_t op_lsl8(M68K *cpu, uint8_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic8(cpu, v); return v; }
    uint8_t r = (n >= 8) ? 0 : (uint8_t)(v << n);
    cpu->c = cpu->x = (n <= 8) ? ((v >> (8 - n)) & 1) : 0;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint8_t op_lsr8(M68K *cpu, uint8_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic8(cpu, v); return v; }
    uint8_t r = (n >= 8) ? 0 : (uint8_t)(v >> n);
    cpu->c = cpu->x = (n <= 8) ? ((v >> (n - 1)) & 1) : 0;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint8_t op_asr8(M68K *cpu, uint8_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic8(cpu, v); return v; }
    uint8_t sign = (uint8_t)((v >> 7) & 1);
    uint8_t r = v;
    for (unsigned i = 0; i < n; i++) {
        cpu->c = cpu->x = r & 1;
        r = (uint8_t)((r >> 1) | (sign << 7));
    }
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint16_t op_lsl16(M68K *cpu, uint16_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic16(cpu, v); return v; }
    uint16_t r = (n >= 16) ? 0 : (uint16_t)(v << n);
    cpu->c = cpu->x = (n <= 16) ? ((v >> (16 - n)) & 1) : 0;
    cpu->n = (r >> 15) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint16_t op_lsr16(M68K *cpu, uint16_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic16(cpu, v); return v; }
    uint16_t r = (n >= 16) ? 0 : (uint16_t)(v >> n);
    cpu->c = cpu->x = (n <= 16) ? ((v >> (n - 1)) & 1) : 0;
    cpu->n = (r >> 15) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint16_t op_asr16(M68K *cpu, uint16_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic16(cpu, v); return v; }
    uint16_t sign = (uint16_t)((v >> 15) & 1);
    uint16_t r = v;
    for (unsigned i = 0; i < n; i++) {
        cpu->c = cpu->x = r & 1;
        r = (uint16_t)((r >> 1) | (sign << 15));
    }
    cpu->n = (r >> 15) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint32_t op_lsl32(M68K *cpu, uint32_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic32(cpu, v); return v; }
    uint32_t r = (n >= 32) ? 0 : (uint32_t)(v << n);
    cpu->c = cpu->x = (n <= 32) ? ((v >> (32 - n)) & 1) : 0;
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
/* ASL nao e' LSL.
 *
 * O padrao de bits e' o mesmo, e por isso a primeira versao mapeava um no outro.
 * A diferenca esta' no OVERFLOW: LSL sempre zera V; ASL liga V quando o bit de
 * sinal MUDA em algum momento do deslocamento — que e' o unico jeito de o
 * programa perceber que o valor deslocado nao cabia mais.
 *
 * A regra equivalente e mais facil de conferir: olhe os (n+1) bits do topo do
 * valor original; se nao forem todos iguais, o sinal mudou no caminho.
 *
 * Achado pelo teste diferencial contra o Musashi, em asl.b, asl.w e asl.l. */
#define DEF_ASL(LARG)                                                          \
static inline uint##LARG##_t op_asl##LARG(M68K *cpu, uint##LARG##_t v,         \
                                          unsigned n) {                        \
    if (n == 0) { cpu->c = 0; cpu->v = 0; flags_logic##LARG(cpu, v); return v; }\
    uint##LARG##_t r = (n >= LARG) ? 0 : (uint##LARG##_t)(v << n);             \
    cpu->c = cpu->x = (n <= LARG) ? ((v >> (LARG - n)) & 1) : 0;               \
    /* Mascara dos (n+1) bits do topo: V se nem todos forem iguais. */         \
    unsigned bits = (n + 1 >= LARG) ? LARG : n + 1;                            \
    uint##LARG##_t topo = (uint##LARG##_t)(v >> (LARG - bits));                \
    uint##LARG##_t cheio = (uint##LARG##_t)((bits >= LARG)                     \
                            ? (uint##LARG##_t)~(uint##LARG##_t)0               \
                            : (uint##LARG##_t)(((uint32_t)1 << bits) - 1));    \
    cpu->v = (topo != 0 && topo != cheio);                                     \
    cpu->n = (r >> (LARG - 1)) & 1; cpu->z = (r == 0);                         \
    return r;                                                                  \
}
DEF_ASL(8)
DEF_ASL(16)
DEF_ASL(32)
#undef DEF_ASL

static inline uint32_t op_lsr32(M68K *cpu, uint32_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic32(cpu, v); return v; }
    uint32_t r = (n >= 32) ? 0 : (uint32_t)(v >> n);
    cpu->c = cpu->x = (n <= 32) ? ((v >> (n - 1)) & 1) : 0;
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint32_t op_asr32(M68K *cpu, uint32_t v, unsigned n) {
    if (n == 0) { cpu->c = 0; flags_logic32(cpu, v); return v; }
    uint32_t sign = (uint32_t)((v >> 31) & 1);
    uint32_t r = v;
    for (unsigned i = 0; i < n; i++) {
        cpu->c = cpu->x = r & 1;
        r = (uint32_t)((r >> 1) | (sign << 31));
    }
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}

static inline uint8_t op_neg8(M68K *cpu, uint8_t v) { return flags_sub8(cpu, 0, v); }

/* ABCD, SBCD e SUBX tem uma peculiaridade que vale destacar: a flag Z e'
 * **acumulativa**. Ela nunca e' setada por essas instrucoes, so' zerada quando
 * o resultado e' diferente de zero. E' assim que somas BCD encadeadas em
 * varios bytes conseguem dizer "o total deu zero" no fim. Tratar Z como as
 * outras instrucoes quebraria comparacao de placar de multiplos digitos. */
/* BCD: somar binario e AJUSTAR POR 6, nao "subtrair 10".
 *
 * As duas formas dao o mesmo resultado para digitos BCD validos (0-9), e por
 * isso a versao anterior passava despercebida. Divergem quando um digito e'
 * A-F — situacao que o manual chama de indefinida, mas que o silicio resolve de
 * um jeito especifico, e e' esse jeito que o teste diferencial cobra.
 *
 * O carry sai da comparacao com $99 (nao de cada digito separado), e essa e' a
 * diferenca que fazia sbcd devolver $FF onde o hardware devolve $9F.
 *
 * Z e' CUMULATIVO: so' e' zerado, nunca ligado. E' assim que uma cadeia de abcd
 * sobre varios bytes acumula "o numero inteiro deu zero".
 *
 * V e' oficialmente indefinido. O valor abaixo e' o que o hardware produz, e
 * esta' aqui para o teste diferencial poder comparar tudo em vez de ter uma
 * excecao — excecao em teste e' onde bug se esconde. */
static inline uint8_t op_abcd(M68K *cpu, uint8_t a, uint8_t b) {
    unsigned r = (unsigned)(a & 0x0Fu) + (unsigned)(b & 0x0Fu) + cpu->x;
    unsigned v_indef = ~r;
    if (r > 9) r += 6;
    r += (unsigned)(a & 0xF0u) + (unsigned)(b & 0xF0u);
    unsigned vai = (r > 0x99u);
    if (vai) r -= 0xA0u;
    cpu->c = cpu->x = (uint8_t)vai;
    cpu->v = (uint8_t)((v_indef & r & 0x80u) != 0);
    uint8_t res = (uint8_t)r;
    if (res) cpu->z = 0;
    cpu->n = (res >> 7) & 1;
    return res;
}
/* SBCD: o espelho de op_abcd — subtrai binario e ajusta por 6. Ver os
 * comentarios la'; as mesmas regras valem para Z cumulativo e V indefinido. */
static inline uint8_t op_sbcd(M68K *cpu, uint8_t a, uint8_t b) {
    unsigned r = (unsigned)(a & 0x0Fu) - (unsigned)(b & 0x0Fu) - cpu->x;
    unsigned v_indef = ~r;
    unsigned corrigiu = 0;
    if (r > 9) { r -= 6; corrigiu = 1; }
    r += (unsigned)(a & 0xF0u) - (unsigned)(b & 0xF0u);
    unsigned empresta = (r > 0x99u);
    if (empresta) r += 0xA0u;
    (void)corrigiu;
    cpu->c = cpu->x = (uint8_t)empresta;
    cpu->v = (uint8_t)((v_indef & r & 0x80u) != 0);
    uint8_t res = (uint8_t)r;
    if (res) cpu->z = 0;
    cpu->n = (res >> 7) & 1;
    return res;
}
static inline uint16_t op_subx16(M68K *cpu, uint16_t a, uint16_t b) {
    uint32_t wide = (uint32_t)a - b - cpu->x;
    uint16_t r = (uint16_t)wide;
    cpu->c = cpu->x = (wide >> 16) & 1;
    cpu->v = (((a ^ b) & (a ^ r)) >> 15) & 1;
    cpu->n = (r >> 15) & 1;
    if (r) cpu->z = 0;
    return r;
}

/* Rotacoes. ROL/ROR nao passam por X; C recebe o bit que deu a volta. */
static inline uint16_t op_rol16(M68K *cpu, uint16_t v, unsigned n) {
    n %= 16;
    uint16_t r = n ? (uint16_t)((v << n) | (v >> (16 - n))) : v;
    cpu->c = n ? (r & 1) : 0;
    cpu->n = (r >> 15) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint16_t op_ror16(M68K *cpu, uint16_t v, unsigned n) {
    n %= 16;
    uint16_t r = n ? (uint16_t)((v >> n) | (v << (16 - n))) : v;
    cpu->c = n ? ((r >> 15) & 1) : 0;
    cpu->n = (r >> 15) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint8_t op_rol8(M68K *cpu, uint8_t v, unsigned n) {
    n %= 8;
    uint8_t r = n ? (uint8_t)((v << n) | (v >> (8 - n))) : v;
    cpu->c = n ? (r & 1) : 0;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint8_t op_ror8(M68K *cpu, uint8_t v, unsigned n) {
    n %= 8;
    uint8_t r = n ? (uint8_t)((v >> n) | (v << (8 - n))) : v;
    cpu->c = n ? ((r >> 7) & 1) : 0;
    cpu->n = (r >> 7) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint32_t op_rol32(M68K *cpu, uint32_t v, unsigned n) {
    n %= 32;
    uint32_t r = n ? ((v << n) | (v >> (32 - n))) : v;
    cpu->c = n ? (r & 1) : 0;
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}
static inline uint32_t op_ror32(M68K *cpu, uint32_t v, unsigned n) {
    n %= 32;
    uint32_t r = n ? ((v >> n) | (v << (32 - n))) : v;
    cpu->c = n ? ((r >> 31) & 1) : 0;
    cpu->n = (r >> 31) & 1; cpu->z = (r == 0); cpu->v = 0;
    return r;
}

/* MULS.W e DIVS.W: versoes com sinal. */
static inline uint32_t op_muls(M68K *cpu, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b);
    flags_logic32(cpu, r);
    return r;
}
static inline uint32_t op_divs(M68K *cpu, uint32_t dividend, uint16_t divisor,
                               int *trap_div0) {
    if (divisor == 0) { *trap_div0 = 1; return dividend; }
    int32_t n = (int32_t)dividend;
    int32_t d = (int32_t)(int16_t)divisor;
    /* $80000000 / -1: caso especial do 68000, e nao o que a aritmetica sugere.
     *
     * O quociente seria +2147483648, que nao cabe em 16 bits — a leitura obvia
     * e' "overflow, V liga, destino intacto". O hardware faz outra coisa:
     * devolve ZERO, com Z ligado e V DESLIGADO. Musashi implementa esse caso
     * explicitamente, e foi ele quem apontou a divergencia aqui.
     *
     * Duas armadilhas de uma vez. Em C, INT32_MIN / -1 e' comportamento
     * indefinido, e no x86 o idiv levanta SIGFPE: antes desta guarda o difftest
     * nao divergia, ele MORRIA. E a guarda tem que vir antes da divisao — o
     * teste de faixa logo abaixo nunca chegaria a executar. */
    if (n == INT32_MIN && d == -1) {
        cpu->n = 0; cpu->z = 1; cpu->v = 0; cpu->c = 0;
        return 0;
    }
    int32_t q = n / d;
    if (q > 32767 || q < -32768) { cpu->v = 1; return dividend; }
    int32_t rem = n % d;
    uint32_t r = ((uint32_t)(rem & 0xFFFF) << 16) | ((uint32_t)q & 0xFFFF);
    cpu->n = (q < 0); cpu->z = (q == 0); cpu->v = 0; cpu->c = 0;
    return r;
}

/* MULU.W: resultado de 32 bits, N e Z sobre ele, V e C zerados. */
static inline uint32_t op_mulu(M68K *cpu, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a * b;
    flags_logic32(cpu, r);
    return r;
}

/* DIVU.W: quociente no meio baixo, resto no alto.
 *
 * Overflow (quociente > $FFFF) apenas seta V e **deixa o destino intacto** —
 * nao e' excecao. Divisao por zero e' excecao de verdade; aqui vira um sinal
 * explicito em vez de comportamento indefinido do C. */
static inline uint32_t op_divu(M68K *cpu, uint32_t dividend, uint16_t divisor,
                               int *trap_div0) {
    if (divisor == 0) { *trap_div0 = 1; return dividend; }
    uint32_t quotient = dividend / divisor;
    if (quotient > 0xFFFF) { cpu->v = 1; return dividend; }
    uint32_t remainder = dividend % divisor;
    uint32_t r = (remainder << 16) | (quotient & 0xFFFF);
    cpu->n = (quotient >> 15) & 1;
    cpu->z = (quotient == 0);
    cpu->v = 0;
    cpu->c = 0;
    return r;
}

/* SWAP: troca as metades e testa o long inteiro. */
static inline uint32_t op_swap(M68K *cpu, uint32_t v) {
    uint32_t r = (v >> 16) | (v << 16);
    flags_logic32(cpu, r);
    return r;
}

#endif /* TINYTOON_M68K_H */
