/* Z80 do Mega Drive: o processador de som.
 *
 * O 68000 nao toca o YM2612 diretamente neste jogo. No boot ele escreve ~8 KB de
 * driver na RAM do Z80, byte a byte, e depois so' deposita comandos numa fila e
 * pede o barramento em $A11100. Quem gera musica e efeito e' o Z80 rodando esse
 * driver. Sem ele o jogo fica mudo, por mais completo que o YM2612 esteja.
 *
 * Interpretador, nao recompilador: o driver vive em RAM e e' escrito em tempo de
 * execucao, entao nao ha' o que analisar estaticamente. E' a diferenca de
 * natureza entre os dois processadores deste port.
 *
 * O mapa que o Z80 enxerga (montado em runtime/z80_bus.c):
 *   $0000-$1FFF  RAM propria, 8 KB
 *   $2000-$3FFF  espelho da RAM
 *   $4000-$4003  YM2612
 *   $6000        registrador de banco (um bit por escrita, 9 escritas)
 *   $7F11        PSG
 *   $8000-$FFFF  janela de 32 KB para a memoria do 68000, escolhida pelo banco
 */

#ifndef TINYTOON_Z80_H
#define TINYTOON_Z80_H

#include <stdint.h>

/* Bits do registrador F. X e Y sao indocumentados — copias dos bits 3 e 5 do
 * resultado. Nenhum driver de som depende deles, mas implementa-los e' o que
 * permite validar o core contra o zexdoc, que testa tudo. */
#define Z80_FC 0x01u
#define Z80_FN 0x02u
#define Z80_FP 0x04u
#define Z80_FX 0x08u
#define Z80_FH 0x10u
#define Z80_FY 0x20u
#define Z80_FZ 0x40u
#define Z80_FS 0x80u

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a_, f_, b_, c_, d_, e_, h_, l_;   /* conjunto alternativo (EXX) */
    uint16_t ix, iy, sp, pc;
    uint8_t i, r;
    uint8_t iff1, iff2, im;
    uint8_t parado;          /* HALT: so' sai com interrupcao */
    uint8_t irq_pendente;
    uint64_t ciclos;
} Z80;

/* O barramento e' do hospedeiro. O core nao sabe o que ha' em cada endereco —
 * essa e' a fronteira que deixa o mesmo core rodar o zexdoc no teste e o driver
 * de som no jogo. */
uint8_t z80_ler(uint16_t addr);
void z80_escrever(uint16_t addr, uint8_t v);
uint8_t z80_entrada(uint16_t porta);
void z80_saida(uint16_t porta, uint8_t v);

void z80_reset(Z80 *z);

/* Executa uma instrucao. Devolve os ciclos gastos. */
unsigned z80_passo(Z80 *z);

/* Executa ate' gastar pelo menos ``ciclos``. Devolve quantos de fato gastou. */
unsigned z80_rodar(Z80 *z, unsigned ciclos);

/* Interrupcao de V-blank. No Mega Drive o Z80 so' usa modo 1 (vetor $0038). */
void z80_interromper(Z80 *z);

#endif /* TINYTOON_Z80_H */
