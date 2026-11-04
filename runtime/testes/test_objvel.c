/* Verificacao de ObjApplyVelocity ($004502).
 *
 * Exercita o que os outros dois casos nao pegavam: MOVEM salvando e
 * restaurando registrador na pilha, e enderecamento com deslocamento em
 * registrador de endereco.
 *
 * So' toca RAM: a0 aponta para o slot 12 do array de objetos e a7 para uma
 * area de pilha. Nao precisa de ROM nem de fixture.
 */

#include <stdio.h>

#include "m68k.h"
#include "memory.h"

void ObjApplyVelocity(M68K *cpu);

#define OBJ 0xFFA600u
#define PILHA 0xFFFF00u

int main(void) {
    static const uint32_t casos[][6] = {
        /* posX, posY, posZ, velX, velY, velZ */
        {0x00010000u, 0x00020000u, 0x00000000u, 0x00008000u, 0xFFFF8000u, 0x00010000u},
        {0xFFFFFFFFu, 0x00000000u, 0x7FFFFFFFu, 0x00000001u, 0xFFFFFFFFu, 0x00000001u},
        {0x12345678u, 0x9ABCDEF0u, 0x00000000u, 0x00000100u, 0x00000200u, 0xFFFFFF00u},
    };
    md_reset_ram();
    for (unsigned i = 0; i < sizeof casos / sizeof casos[0]; i++) {
        M68K cpu = {0};
        cpu.a[0] = OBJ;
        cpu.a[7] = PILHA;
        cpu.d[0] = 0xDEADBEEFu;          /* MOVEM tem de preservar isto */
        md_write32(OBJ + 0x4, casos[i][0]);
        md_write32(OBJ + 0x8, casos[i][1]);
        md_write32(OBJ + 0xC, casos[i][2]);
        md_write32(OBJ + 0x10, casos[i][3]);
        md_write32(OBJ + 0x14, casos[i][4]);
        md_write32(OBJ + 0x18, casos[i][5]);
        ObjApplyVelocity(&cpu);
        printf("%08X %08X %08X %08X %08X\n",
               md_read32(OBJ + 0x4), md_read32(OBJ + 0x8), md_read32(OBJ + 0xC),
               cpu.d[0], cpu.a[7]);
    }
    fprintf(stderr, "acessos a hardware nao implementado: %lu\n",
            md_unimplemented_accesses);
    return 0;
}
