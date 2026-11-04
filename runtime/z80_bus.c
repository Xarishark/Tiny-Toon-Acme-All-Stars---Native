/* O que o Z80 do Mega Drive enxerga, e como o 68000 fala com ele.
 *
 *   $0000-$1FFF  RAM propria, 8 KB
 *   $2000-$3FFF  espelho da RAM
 *   $4000-$5FFF  YM2612 (4 portas, espelhadas)
 *   $6000-$60FF  registrador de banco
 *   $7F00-$7FFF  VDP visto pelo Z80; o PSG fica em $7F11
 *   $8000-$FFFF  janela de 32 KB para a memoria do 68000
 *
 * O registrador de banco e' um deslocador: cada escrita empurra UM bit para o
 * topo de um registrador de 9 bits. Trocar de banco custa nove escritas. Nao e'
 * capricho do hardware — e' o que cabia nos pinos.
 */

#include "z80_bus.h"

#include "memory.h"
#include "psg.h"
#include "ym2612.h"
#include "z80.h"

#include <stdio.h>
#include <string.h>

static uint8_t ram[0x2000];
static uint16_t banco;              /* 9 bits */
static int busreq;                  /* 68000 pediu o barramento */
static int em_reset = 1;            /* o Z80 nasce em reset */
static Z80 cpu;
static int saldo;   /* divida de ciclos do Z80; ver z80_bus_executar */

unsigned long z80_instrucoes;
unsigned long z80_escritas_ym, z80_escritas_psg;

/* --------------------------------------------------------- mapa do Z80 */

uint8_t z80_ler(uint16_t a) {
    if (a < 0x4000) return ram[a & 0x1FFF];
    if (a < 0x6000) return ym2612_ler();
    if (a < 0x8000) return 0xFF;    /* banco e PSG sao so' de escrita */
    /* A janela de banco do Z80 ve' o barramento do 68000 — e' por aqui que o
     * driver de som busca instrumento e amostra de PCM na ROM. Marcada com via
     * propria porque nao e' nem CPU nem DMA, e o que ela le' e' audio. */
    return md_peek8_z80((uint32_t)(banco << 15) + (a - 0x8000), cpu.pc);
}

void z80_escrever(uint16_t a, uint8_t v) {
    if (a < 0x4000) { ram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { ym2612_escrever(a & 3, v); z80_escritas_ym++; return; }
    if (a < 0x6100) {
        /* Um bit por escrita, entrando pelo topo. */
        banco = (uint16_t)((banco >> 1) | ((v & 1) << 8));
        return;
    }
    if (a >= 0x7F00 && a < 0x8000) {
        if ((a & 0xFF) == 0x11) { psg_escrever(v); z80_escritas_psg++; }
        return;
    }
    /* Escrita na janela do 68000: cai na RAM de trabalho quando aponta para la'.
     * Nao ha' caminho de escrita generico para ROM, e o driver nao usa. */
    md_poke8((uint32_t)(banco << 15) + (a - 0x8000), v);
}

/* O Z80 do Mega Drive nao tem nada nas portas de I/O. Existe porque o core
 * precisa da funcao, nao porque o hardware responda. */
uint8_t z80_entrada(uint16_t porta) { (void)porta; return 0xFF; }
void z80_saida(uint16_t porta, uint8_t v) { (void)porta; (void)v; }

/* ------------------------------------------------ o lado do 68000 */

void z80_bus_reset(void) {
    memset(ram, 0, sizeof ram);
    banco = 0;
    busreq = 0;
    em_reset = 1;
    saldo = 0;
    z80_reset(&cpu);
    z80_instrucoes = z80_escritas_ym = z80_escritas_psg = 0;
}

uint8_t z80_bus_ler_ram(uint32_t addr) { return ram[addr & 0x1FFF]; }
void z80_bus_escrever_ram(uint32_t addr, uint8_t v) { ram[addr & 0x1FFF] = v; }

void z80_bus_busreq(uint8_t v) { busreq = (v & 1) != 0; }

/* Bit 0 = 1 significa "o Z80 ainda esta' com o barramento". O jogo faz
 * testando o bit 0 num laco, esperando esse bit cair. Como aqui a concessao e' imediata,
 * devolver 0 quando o pedido esta' de pe' e' o que destrava o laco. */
uint8_t z80_bus_estado(void) { return (uint8_t)(busreq ? 0 : 1); }

void z80_bus_reset_linha(uint8_t v) {
    int novo = !(v & 1);            /* bit 0 em zero = mantido em reset */
    if (novo && !em_reset) z80_reset(&cpu);
    em_reset = novo;
}

int z80_bus_rodando(void) { return !em_reset && !busreq; }

/* Saldo de ciclos, e ele precisa ser PERSISTENTE.
 *
 * A versao anterior zerava a conta a cada chamada: pedia 1 ciclo, executava uma
 * instrucao de 4 e esquecia os 3 de sobra. Como a chamada vem a cada acesso do
 * 68000 e quase sempre pede 1 ou 2 ciclos, o resultado era uma instrucao de Z80
 * por acesso — 145 milhoes por 3.000 quadros, tres vezes o que o chip faz. Som
 * rodando rapido demais e' o sintoma que isso produz.
 *
 * Guardando a divida, o excesso de uma chamada e' descontado da seguinte. */

void z80_bus_executar(unsigned ciclos) {
    if (!z80_bus_rodando()) return;
    saldo += (int)ciclos;
    while (saldo > 0) {
        saldo -= (int)z80_passo(&cpu);
        z80_instrucoes++;
    }
}

void z80_bus_vblank(void) {
    if (z80_bus_rodando()) z80_interromper(&cpu);
}

void z80_bus_relatorio(void) {
    printf("Z80: %s | %lu instrucoes | banco $%02X (-> $%06X)\n",
           em_reset ? "em reset" : (busreq ? "barramento com o 68000" : "rodando"),
           z80_instrucoes, banco, (unsigned)(banco << 15));
    printf("  escritas: YM2612 %lu, PSG %lu | PC=$%04X SP=$%04X\n",
           z80_escritas_ym, z80_escritas_psg, cpu.pc, cpu.sp);
}
