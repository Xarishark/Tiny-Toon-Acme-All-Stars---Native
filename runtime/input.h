/* Controle do Mega Drive, 3 botoes.
 *
 * O port de controle nao e' um registrador que devolve os botoes: e' um
 * multiplexador. A linha TH (bit 6) escolhe qual metade do controle aparece nos
 * seis bits de baixo, e o jogo alterna TH para ler as duas. Todos os sinais sao
 * ativos em ZERO — botao solto le' 1.
 *
 *   TH = 1:  ?1CBRLDU
 *   TH = 0:  ?0SA00DU
 *
 * Escolha deliberada: 3 botoes, nao 6. Um controle de 3 botoes e' configuracao
 * real e o jogo tem que funcionar com ela — e a deteccao de 6 botoes depende de
 * o contador de ciclos zerar por TEMPO (~1,5 ms sem acesso), que este runtime
 * ainda nao tem. Implementar 6 botoes sem relogio seria adivinhar. Fica para
 * depois do contador de linha do VDP.
 */

#ifndef TINYTOON_INPUT_H
#define TINYTOON_INPUT_H

#include <stdint.h>

#define MD_PAD_UP    0x0001u
#define MD_PAD_DOWN  0x0002u
#define MD_PAD_LEFT  0x0004u
#define MD_PAD_RIGHT 0x0008u
#define MD_PAD_B     0x0010u
#define MD_PAD_C     0x0020u
#define MD_PAD_A     0x0040u
#define MD_PAD_START 0x0080u

#define MD_PORTAS 2

/* Bitmask de botoes pressionados; 1 = pressionado (o inverso do barramento). */
void md_pad_set(unsigned porta, unsigned botoes);

/* Portas de dados ($A10003, $A10005) e de direcao ($A10009, $A1000B). */
uint8_t md_pad_read(unsigned porta);
void md_pad_write(unsigned porta, uint8_t valor);
void md_pad_set_ctrl(unsigned porta, uint8_t valor);

/* Explorador de cobertura: aperta botoes sozinho, deterministico pela semente.
 * Ver a justificativa em input.c. */
void md_explorar_ligar(uint32_t semente);
int md_explorando(void);
void md_explorar_quadro(void);

#endif /* TINYTOON_INPUT_H */
