/* O Z80 do Mega Drive visto pelo 68000 e pelo runtime. */

#ifndef TINYTOON_Z80_BUS_H
#define TINYTOON_Z80_BUS_H

#include <stdint.h>

void z80_bus_reset(void);

/* $A00000-$A0FFFF: a RAM do Z80 vista pelo 68000. */
uint8_t z80_bus_ler_ram(uint32_t addr);
void z80_bus_escrever_ram(uint32_t addr, uint8_t v);

/* $A11100 (BUSREQ) e $A11200 (RESET). */
void z80_bus_busreq(uint8_t v);
uint8_t z80_bus_estado(void);
void z80_bus_reset_linha(uint8_t v);

/* O Z80 so' anda quando esta' fora de reset E o 68000 nao pediu o barramento. */
int z80_bus_rodando(void);
void z80_bus_executar(unsigned ciclos);
void z80_bus_vblank(void);
void z80_bus_relatorio(void);

extern unsigned long z80_instrucoes;
extern unsigned long z80_escritas_ym, z80_escritas_psg;

#endif /* TINYTOON_Z80_BUS_H */
