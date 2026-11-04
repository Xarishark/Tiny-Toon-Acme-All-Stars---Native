/* SN76489, o gerador de som secundario do Mega Drive.
 *
 * Tres canais de onda quadrada e um de ruido, quatro bits de volume cada. No
 * Mega Drive fica no VDP, em $C00011 para o 68000 e $7F11 para o Z80 — herança
 * do Master System, que so' tinha ele.
 */

#ifndef TINYTOON_PSG_H
#define TINYTOON_PSG_H

#include <stdint.h>

void psg_reset(void);
void psg_escrever(uint8_t v);

/* Uma amostra mono, na taxa de geracao do runtime. Amostra por amostra e nao em
 * lote, porque o som e' gerado junto com o relogio — ver audio.h. */
int16_t psg_amostra(void);

#endif /* TINYTOON_PSG_H */
