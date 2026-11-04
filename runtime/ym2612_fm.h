/* Sintese FM do YM2612, separada da metade de sistema.
 *
 * ym2612.c cuida de registradores, temporizadores e status — o que o driver
 * precisa para ANDAR. Este arquivo cuida do que faz o som SAIR. Sao dois
 * assuntos com ritmos e modos de falha diferentes: um erro aqui produz som
 * errado; um erro la' produz silencio total.
 */

#ifndef TINYTOON_YM2612_FM_H
#define TINYTOON_YM2612_FM_H

#include <stdint.h>

void fm_reset(void);

/* Chamados por ym2612.c quando um registrador relevante muda. */
void fm_escrever_reg(unsigned parte, uint8_t reg, uint8_t v);
void fm_key(uint8_t v);

/* Canal 6 em modo DAC: a saida FM dele e' substituida pela amostra. */
void fm_dac(int ligado, int8_t amostra);

/* Uma amostra estereo no ritmo interno do chip (relogio / 144).
 *
 * Estereo porque o chip e' estereo: cada canal tem dois bits em $B4 que decidem
 * se ele sai pela esquerda, pela direita ou por ambas. Somar tudo em mono
 * descarta informacao que o jogo esta' dando de proposito. */
void fm_amostra(int32_t *esquerda, int32_t *direita);

#endif /* TINYTOON_YM2612_FM_H */
