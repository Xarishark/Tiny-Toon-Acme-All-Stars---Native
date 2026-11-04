/* Lado da REFERENCIA do teste diferencial: Musashi executa a instrucao.
 *
 * Le' casos.txt (indice, endereco, bytes), monta o mesmo estado inicial que o
 * meu lado monta, executa UMA instrucao e imprime o estado resultante. O
 * formato de saida e' identico ao do outro lado, para que a comparacao seja um
 * diff de texto — nao ha' logica de comparacao aqui, e portanto nao ha' onde
 * esconder um erro que faca o teste passar.
 *
 * Instrucao que tira excecao (divisao por zero, por exemplo) e' marcada e
 * ignorada na comparacao: o meu recompilador nao modela excecoes, e fingir que
 * modela seria pior do que dizer que nao testa.
 */

#include "estado_teste.h"
#include "memoria_teste.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Colchetes, nao aspas, e isso importa: os dois lados do teste tem um arquivo
 * chamado m68k.h. Aspas buscam primeiro no diretorio do proprio .c, que aqui e'
 * o de trabalho, e traria o do runtime. Colchetes pulam esse diretorio e vao
 * pelos -I, com /opt/musashi na frente. */
#include <m68k.h>

/* Musashi chama estas para tudo: busca de instrucao, operandos, pilha. */
unsigned int m68k_read_memory_8(unsigned int a)  { return md_read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return md_read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return md_read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { md_write8(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { md_write16(a, (uint16_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { md_write32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a)  { return md_read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return md_read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return md_read32(a); }

#define MAX_CASOS 4096

static uint32_t caso_addr[MAX_CASOS];
static uint8_t caso_bytes[MAX_CASOS][16];
static unsigned caso_n[MAX_CASOS];
static unsigned total;

static int ler_casos(const char *path) {
    FILE *fh = fopen(path, "r");
    if (!fh) return -1;
    char linha[256];
    while (fgets(linha, sizeof linha, fh)) {
        unsigned idx; unsigned addr; char hex[64];
        if (sscanf(linha, "%u %x %63s", &idx, &addr, hex) != 3) continue;
        if (idx >= MAX_CASOS) continue;
        caso_addr[idx] = addr;
        unsigned n = (unsigned)strlen(hex) / 2;
        for (unsigned i = 0; i < n && i < 16; i++) {
            unsigned b; sscanf(hex + i * 2, "%2x", &b);
            caso_bytes[idx][i] = (uint8_t)b;
        }
        caso_n[idx] = n;
        if (idx + 1 > total) total = idx + 1;
    }
    fclose(fh);
    return 0;
}

int main(int argc, char **argv) {
    unsigned sementes = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 64;
    unsigned semente_base = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 1;

    if (ler_casos("casos.txt") != 0) {
        fprintf(stderr, "nao consegui ler casos.txt\n");
        return 2;
    }

    mt_preencher(0x1234567u);
    /* As instrucoes vao para a memoria ANTES do resguardo, para que os dois
     * lados vejam exatamente a mesma imagem — o meu lado nao busca instrucao,
     * mas um operando pode ler esses bytes. */
    for (unsigned c = 0; c < total; c++) {
        mt_escrever(caso_addr[c], caso_bytes[c], caso_n[c]);
    }
    mt_guardar_ram();

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    for (unsigned c = 0; c < total; c++) {
        if (!caso_n[c]) continue;
        for (unsigned s = 0; s < sementes; s++) {
            uint32_t semente = semente_base + c * 7919u + s;
            EstadoTeste e;
            et_gerar(&e, semente);
            mt_desfazer();
            mt_diario_iniciar();

            /* SR antes de A7: em modo supervisor o A7 do Musashi e' o ponteiro
             * de pilha supervisor, e escrever fora de ordem poria o valor no
             * registrador errado. */
            m68k_set_reg(M68K_REG_SR, 0x2700u | e.ccr);
            for (int i = 0; i < 8; i++) m68k_set_reg(M68K_REG_D0 + i, e.d[i]);
            for (int i = 0; i < 8; i++) m68k_set_reg(M68K_REG_A0 + i, e.a[i]);
            m68k_set_reg(M68K_REG_USP, e.usp);
            m68k_set_reg(M68K_REG_PC, caso_addr[c]);

            /* Uma instrucao, garantida.
             *
             * m68k_execute(1) NAO e' "execute uma instrucao": e' "gaste ate' 1
             * ciclo". Instrucao que custa 16 ciclos deixa o contador em -15, e
             * a chamada seguinte comeca no vermelho e nao executa nada. O
             * sintoma era uma instrucao que o Musashi simplesmente pulava,
             * aparecendo no relatorio como divergencia de memoria — cacei isso
             * ate' reproduzir isolado e ver o PC parado.
             *
             * Insistir ate' o PC mudar e' o que define "um passo" sem depender
             * do modelo de ciclos. Instrucao que tira excecao tambem muda o PC,
             * entao o laco termina do mesmo jeito. */
            for (int t = 0; t < 16; t++) {
                if (m68k_get_reg(NULL, M68K_REG_PC) != caso_addr[c]) break;
                m68k_execute(1);
            }

            unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
            int excecao = (pc != caso_addr[c] + caso_n[c]);

            printf("%u %u", c, s);
            for (int i = 0; i < 8; i++) {
                printf(" %08X", m68k_get_reg(NULL, M68K_REG_D0 + i));
            }
            for (int i = 0; i < 8; i++) {
                printf(" %08X", m68k_get_reg(NULL, M68K_REG_A0 + i));
            }
            /* A pilha INATIVA, que e' o que o meu campo cpu->usp guarda.
             *
             * O Musashi reporta M68K_REG_USP como "o ponteiro de usuario", e em
             * modo usuario isso e' o proprio A7 ativo — nao a pilha guardada.
             * Comparar os dois diretamente acusaria divergencia onde ha' so'
             * convencao de leitura diferente. Em supervisor a inativa e' a de
             * usuario; em usuario e' a de interrupcao. */
            unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
            unsigned inativa = (sr & 0x2000u)
                             ? m68k_get_reg(NULL, M68K_REG_USP)
                             : m68k_get_reg(NULL, M68K_REG_ISP);

            if (getenv("TINYTOON_LISTAR")
                && (unsigned)atoi(getenv("TINYTOON_LISTAR")) == c && s == 0) {
                mt_listar_diferencas("ref", 12);
            }
            uint8_t vd; uint32_t od = mt_primeira_diferenca(&vd);
            printf(" %08X %02X %08X %08X%02X %d\n", inativa, sr & 0x1Fu,
                   mt_hash_ram(), od, vd, excecao);
        }
    }
    return 0;
}
