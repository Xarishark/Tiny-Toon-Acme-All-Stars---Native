/* Verificacao de SinMul ($0045A2).
 *
 * Exercita o que Random nao exercitava: enderecamento PC-indexado
 * ($45BA(pc,d0.w)), desvio condicional traduzido para goto, e neg.l.
 *
 * Nao carrega a ROM do jogo, e sim um fixture com apenas a tabela de seno
 * posicionada em $45BA. Testar uma funcao nao exige o cartucho inteiro.
 */

#include <stdio.h>

#include "m68k.h"
#include "memory.h"

void SinMul(M68K *cpu);

int main(int argc, char **argv) {
    if (argc < 2 || md_load_rom(argv[1]) != 0) {
        fprintf(stderr, "uso: %s fixture.bin\n", argv[0]);
        return 2;
    }
    md_reset_ram();
    for (unsigned angle = 0; angle < 256; angle += 7) {
        static const unsigned magnitudes[] = {1, 100, 1000, 0xFFFF};
        for (unsigned m = 0; m < 4; m++) {
            M68K cpu = {0};
            set_d16(&cpu, 0, (uint16_t)angle);
            set_d16(&cpu, 1, (uint16_t)magnitudes[m]);
            SinMul(&cpu);
            printf("%08X\n", cpu.d[2]);
        }
    }
    fprintf(stderr, "acessos a hardware nao implementado: %lu\n",
            md_unimplemented_accesses);
    return 0;
}
