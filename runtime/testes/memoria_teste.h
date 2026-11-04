/* Memoria plana compartilhada pelos dois lados do teste diferencial. */

#ifndef TINYTOON_MEMORIA_TESTE_H
#define TINYTOON_MEMORIA_TESTE_H

#include <stdint.h>

#define MT_TAMANHO  0x1000000u          /* 16 MB: o barramento do 68000 do MD */
#define MT_MASCARA  0x0FFFFFFu
#define MT_RAM_BASE 0x0FF0000u          /* RAM de trabalho, 64 KB */
#define MT_RAM_TAM  0x0010000u

uint8_t *mt_memoria(void);
void mt_preencher(uint32_t semente);
void mt_escrever(uint32_t addr, const uint8_t *dados, unsigned n);
void mt_guardar_ram(void);
void mt_restaurar_ram(void);
/* Diario de escritas: desfaz exatamente o que a instrucao mexeu, em
 * qualquer lugar dos 16 MB. Ver a justificativa em memoria_teste.c. */
void mt_diario_iniciar(void);
void mt_desfazer(void);
int mt_diario_estourou(void);
uint32_t mt_hash_ram(void);
uint32_t mt_primeira_diferenca(uint8_t *valor);
void mt_listar_diferencas(const char *rotulo, unsigned max);

uint8_t md_read8(uint32_t addr);
uint16_t md_read16(uint32_t addr);
uint32_t md_read32(uint32_t addr);
uint8_t md_peek8(uint32_t addr);
void md_write8(uint32_t addr, uint8_t v);
void md_write16(uint32_t addr, uint16_t v);
void md_write32(uint32_t addr, uint32_t v);
void md_idle(void);
void md_unresolved_dispatch(uint32_t site, uint32_t alvo);
void md_untranslated(uint32_t addr, const char *motivo);

#endif /* TINYTOON_MEMORIA_TESTE_H */
