/* Verificacao diferencial da primeira funcao recompilada.
 *
 * Random ($0003E6) so' toca a RAM ($FFFB02), entao roda sem a ROM carregada —
 * o contador md_unimplemented_accesses no fim confirma isso.
 *
 * A saida e' comparada com um modelo independente em Python, escrito a partir
 * da leitura do assembly e nao do C gerado. Se os dois concordarem em 16
 * iteracoes de uma LCG, o tradutor acertou mulu.w, addi.l, divu.w e swap.
 */

#include <stdio.h>

#include "m68k.h"
#include "memory.h"

void Random(M68K *cpu);

int main(void) {
    M68K cpu = {0};
    md_reset_ram();
    for (int i = 0; i < 16; i++) {
        Random(&cpu);
        printf("%04X\n", md_read16(0xFFFFFB02u));
    }
    fprintf(stderr, "acessos a hardware nao implementado: %lu\n",
            md_unimplemented_accesses);
    return 0;
}
