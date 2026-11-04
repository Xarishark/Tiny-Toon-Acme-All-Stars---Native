/* Roda zexdoc/zexall contra o core Z80.
 *
 * Sao programas CP/M que exercitam cada instrucao com milhoes de combinacoes de
 * operando e flag, e comparam o CRC do resultado com o de um Z80 real. E' o
 * mesmo espirito do teste diferencial do 68000 — referencia escrita por outra
 * pessoa —, so' que aqui a referencia vem embalada como CRC em vez de como
 * emulador.
 *
 *   zexdoc  so' os flags documentados
 *   zexall  inclui os indocumentados X e Y
 *
 * O arnes de CP/M e' o classico: o programa carrega em $0100 e chama o BDOS em
 * $0005. Em vez de emular CP/M, ponho instrucoes de I/O nesses enderecos e trato
 * as portas — $0000 encerra, $0005 imprime.
 *
 *   uso: z80_zex zexdoc.com
 */

#include "../z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[0x10000];
static int terminou;

uint8_t z80_ler(uint16_t a) { return mem[a]; }
void z80_escrever(uint16_t a, uint8_t v) { mem[a] = v; }

static Z80 cpu;

uint8_t z80_entrada(uint16_t porta) {
    /* Trap de BDOS: C=2 imprime o caractere em E; C=9 imprime a string em DE
     * ate' o '$'. Sao as duas unicas chamadas que o zexdoc usa. */
    (void)porta;
    if (cpu.c == 2) {
        fputc(cpu.e, stdout);
    } else if (cpu.c == 9) {
        uint16_t p = (uint16_t)((cpu.d << 8) | cpu.e);
        for (int i = 0; i < 4096 && mem[p] != '$'; i++, p++) fputc(mem[p], stdout);
    }
    fflush(stdout);
    return 0;
}

void z80_saida(uint16_t porta, uint8_t v) {
    (void)porta; (void)v;
    terminou = 1;
}

int main(int argc, char **argv) {
    const char *caminho = argc > 1 ? argv[1] : "zexdoc.com";
    FILE *fh = fopen(caminho, "rb");
    if (!fh) { fprintf(stderr, "nao abri %s\n", caminho); return 2; }
    size_t n = fread(mem + 0x0100, 1, 0x10000 - 0x0100, fh);
    fclose(fh);
    printf("carregado %s: %zu bytes\n", caminho, n);

    /* $0000: OUT (0),A  -> encerra.  $0005: IN A,(0) / RET -> BDOS. */
    mem[0x0000] = 0xD3; mem[0x0001] = 0x00;
    mem[0x0005] = 0xDB; mem[0x0006] = 0x00; mem[0x0007] = 0xC9;

    z80_reset(&cpu);
    cpu.pc = 0x0100;
    cpu.sp = 0xDFFF;

    unsigned long long passos = 0;
    while (!terminou) {
        z80_passo(&cpu);
        if (++passos > 20000000000ULL) {
            printf("\nDESISTI: %llu passos sem terminar\n", passos);
            return 1;
        }
    }
    printf("\n%llu instrucoes, %llu ciclos\n", passos, (unsigned long long)cpu.ciclos);
    return 0;
}
