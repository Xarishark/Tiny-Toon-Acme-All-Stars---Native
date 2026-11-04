/* Estado inicial de CPU para o teste diferencial.
 *
 * Este arquivo e' incluido pelos DOIS lados — o meu e o do Musashi. E' de
 * proposito: se cada lado gerasse o proprio estado, uma diferenca aqui apareceria
 * como divergencia de instrucao, e eu passaria o dia atras do bug errado.
 */

#ifndef TINYTOON_ESTADO_TESTE_H
#define TINYTOON_ESTADO_TESTE_H

#include <stdint.h>
#include "memoria_teste.h"

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t usp;       /* pilha de usuario: restaurar o SR da pilha pode trocar o A7 */
    uint8_t ccr;        /* X N Z V C nos bits 4..0 */
} EstadoTeste;

static inline uint32_t et_rnd(uint32_t *x) {
    *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5;
    return *x;
}

/* Valores de borda.
 *
 * Sorteio uniforme de 32 bits praticamente nunca produz $7FFFFFFF nem $80000000
 * — e e' exatamente ali que erro de sinal e de overflow aparece. Sem este viés,
 * um teste de um milhao de casos pode passar sem nunca ter somado 1 ao maior
 * positivo. */
static const uint32_t ET_BORDAS[] = {
    0x00000000u, 0x00000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u,
    0x0000FFFFu, 0x00008000u, 0x00007FFFu, 0x000000FFu, 0x00000080u,
    0x0000007Fu, 0xFFFF0000u, 0x0000000Au, 0x00000099u, 0x12345678u,
};
#define ET_N_BORDAS (sizeof ET_BORDAS / sizeof *ET_BORDAS)

static inline uint32_t et_valor(uint32_t *x) {
    uint32_t v = et_rnd(x);
    /* Uma em quatro vem da lista de bordas. */
    if ((v & 3u) == 0) return ET_BORDAS[(v >> 8) % ET_N_BORDAS];
    return v;
}

static inline void et_gerar(EstadoTeste *e, uint32_t semente) {
    uint32_t x = semente ? semente : 0x9E3779B9u;
    for (int i = 0; i < 8; i++) e->d[i] = et_valor(&x);
    /* Registradores de endereco apontam para o meio da RAM, alinhados em
     * palavra. Alinhados porque o 68000 gera erro de endereco em acesso impar
     * de palavra, e este teste e' sobre aritmetica, nao sobre excecoes; no meio
     * porque predecremento e pos-incremento precisam de espaco dos dois lados. */
    for (int i = 0; i < 8; i++) {
        e->a[i] = (MT_RAM_BASE + 0x8000u + (et_rnd(&x) & 0x3FFEu)) & ~1u;
    }
    e->usp = (MT_RAM_BASE + 0x4000u + (et_rnd(&x) & 0x3FFEu)) & ~1u;
    e->ccr = (uint8_t)(et_rnd(&x) & 0x1Fu);
}

#endif /* TINYTOON_ESTADO_TESTE_H */
