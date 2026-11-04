/* YM2612: seis canais de FM com quatro operadores cada.
 *
 * Duas metades bem diferentes convivem neste chip, e vale separa-las:
 *
 *   O SISTEMA — banco de registradores, dois temporizadores e o registrador de
 *   status. E' disso que o driver de som depende para ANDAR: quase todo driver
 *   de Mega Drive marca o tempo lendo o bit de overflow do temporizador A ou B,
 *   nao contando quadros.
 *
 *   A SINTESE — geradores de fase e de envelope, oito algoritmos, realimentacao.
 *   E' disso que depende o som SAIR.
 *
 * Um driver com a sintese muda toca certo e em silencio; um driver sem os
 * temporizadores nao toca de jeito nenhum, porque fica preso esperando um bit.
 */

#ifndef TINYTOON_YM2612_H
#define TINYTOON_YM2612_H

#include <stdint.h>

void ym2612_reset(void);

/* Quatro portas: $4000/$4001 sao endereco/dado da parte 1, $4002/$4003 da 2. */
void ym2612_escrever(unsigned porta, uint8_t v);
uint8_t ym2612_ler(void);

/* Avanca os temporizadores em ``ciclos`` do relogio do YM. */
void ym2612_avancar(unsigned ciclos);

void ym2612_relatorio(void);

extern unsigned long ym_teclas_ligadas;   /* quantas notas foram disparadas */

#endif /* TINYTOON_YM2612_H */
