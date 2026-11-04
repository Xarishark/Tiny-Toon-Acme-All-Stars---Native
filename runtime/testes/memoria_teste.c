/* Barramento de memoria para o teste diferencial.
 *
 * Plano e burro de proposito: 16 MB de RAM, sem VDP, sem joypad, sem relogio de
 * quadros. O que esta' sendo testado e' a SEMANTICA DAS INSTRUCOES — se o
 * barramento de teste tiver comportamento proprio, ele entra no resultado e o
 * teste passa a medir duas coisas ao mesmo tempo.
 *
 * O lado da referencia (Musashi) usa exatamente esta mesma imagem de memoria.
 */

#include "memoria_teste.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t mem[MT_TAMANHO];

uint8_t *mt_memoria(void) { return mem; }

/* Padrao deterministico, igual dos dois lados. Nao pode ser zero: instrucao que
 * le' memoria e devolve sempre 0 passaria no teste sem exercitar nada. */
void mt_preencher(uint32_t semente) {
    uint32_t x = semente ? semente : 1u;
    for (uint32_t i = 0; i < MT_TAMANHO; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        mem[i] = (uint8_t)(x >> 16);
    }
}

/* Copia de resguardo da RAM ($FF0000-$FFFFFF), usada para RELATAR o que a
 * instrucao mudou. Nao serve para restaurar: escrita pode cair em qualquer
 * lugar dos 16 MB. Ver o diario, abaixo. */
static uint8_t resguardo[MT_RAM_TAM];

/* Diario de escritas.
 *
 * Entre um caso e outro a memoria tem que voltar EXATAMENTE ao estado inicial —
 * inclusive fora da RAM. O motivo e' uma assimetria entre os dois lados do
 * teste: o meu nao busca instrucao da memoria (a traducao esta' compilada), mas
 * o Musashi busca. Um caso que escreve por cima dos bytes de outro caso faz a
 * referencia executar codigo diferente do meu, e a divergencia aparece como
 * "semantica de instrucao" quando e' contaminacao.
 *
 * Restaurar 16 MB por caso seria inviavel. O diario guarda o valor anterior de
 * cada byte escrito e desfaz na ordem inversa: exato e proporcional ao que a
 * instrucao mexeu, que sao poucos bytes. */
#define MT_DIARIO_MAX 8192
static uint32_t diario_addr[MT_DIARIO_MAX];
static uint8_t diario_antes[MT_DIARIO_MAX];
static unsigned diario_n;
static int diario_ligado;
static int diario_estourou;

void mt_diario_iniciar(void) { diario_n = 0; diario_ligado = 1; }

void mt_desfazer(void) {
    while (diario_n) {
        diario_n--;
        mem[diario_addr[diario_n]] = diario_antes[diario_n];
    }
}

int mt_diario_estourou(void) { return diario_estourou; }

void mt_guardar_ram(void) {
    for (uint32_t i = 0; i < MT_RAM_TAM; i++) resguardo[i] = mem[MT_RAM_BASE + i];
}

void mt_restaurar_ram(void) {
    for (uint32_t i = 0; i < MT_RAM_TAM; i++) mem[MT_RAM_BASE + i] = resguardo[i];
}

/* Onde a instrucao escreveu, comparado com o resguardo.
 *
 * O hash diz QUE a memoria ficou diferente; isto diz ONDE e COM O QUE. Sem
 * essa informacao, uma divergencia de RAM vira adivinhacao — e adivinhacao
 * sobre memoria e' cara, porque os candidatos sao 65.536. */
uint32_t mt_primeira_diferenca(uint8_t *valor) {
    for (uint32_t i = 0; i < MT_RAM_TAM; i++) {
        if (mem[MT_RAM_BASE + i] != resguardo[i]) {
            *valor = mem[MT_RAM_BASE + i];
            return MT_RAM_BASE + i;
        }
    }
    *valor = 0;
    return 0xFFFFFFFFu;
}

/* Todas as posicoes alteradas, nao so' a primeira. Quando os dois lados
 * concordam na primeira escrita mas discordam no hash, a resposta esta' aqui. */
void mt_listar_diferencas(const char *rotulo, unsigned max) {
    unsigned n = 0;
    fprintf(stderr, "[%s] alterou:", rotulo);
    for (uint32_t i = 0; i < MT_RAM_TAM && n < max; i++) {
        if (mem[MT_RAM_BASE + i] != resguardo[i]) {
            fprintf(stderr, " $%06X=%02X(era %02X)",
                    MT_RAM_BASE + i, mem[MT_RAM_BASE + i], resguardo[i]);
            n++;
        }
    }
    fprintf(stderr, "%s\n", n ? "" : " nada");
}

uint32_t mt_hash_ram(void) {
    uint32_t h = 2166136261u;                    /* FNV-1a */
    for (uint32_t i = 0; i < MT_RAM_TAM; i++) {
        h ^= mem[MT_RAM_BASE + i];
        h *= 16777619u;
    }
    return h;
}

void mt_escrever(uint32_t addr, const uint8_t *dados, unsigned n) {
    for (unsigned i = 0; i < n; i++) mem[(addr + i) & MT_MASCARA] = dados[i];
}

/* --- a interface que o codigo recompilado usa ---------------------------- */

uint8_t md_read8(uint32_t addr) { return mem[addr & MT_MASCARA]; }
uint8_t md_peek8(uint32_t addr) { return mem[addr & MT_MASCARA]; }

void md_write8(uint32_t addr, uint8_t v) {
    uint32_t a = addr & MT_MASCARA;
    if (diario_ligado) {
        if (diario_n < MT_DIARIO_MAX) {
            diario_addr[diario_n] = a;
            diario_antes[diario_n] = mem[a];
            diario_n++;
        } else {
            diario_estourou = 1;   /* silencio aqui seria contaminacao adiante */
        }
    }
    mem[a] = v;
}

uint16_t md_read16(uint32_t addr) {
    return (uint16_t)((md_read8(addr) << 8) | md_read8(addr + 1));
}
uint32_t md_read32(uint32_t addr) {
    return ((uint32_t)md_read16(addr) << 16) | md_read16(addr + 2);
}
void md_write16(uint32_t addr, uint16_t v) {
    md_write8(addr, (uint8_t)(v >> 8));
    md_write8(addr + 1, (uint8_t)v);
}
void md_write32(uint32_t addr, uint32_t v) {
    md_write16(addr, (uint16_t)(v >> 16));
    md_write16(addr + 2, (uint16_t)v);
}

/* Stubs: nenhum caso de teste chega aqui, e chegar significa que a selecao de
 * instrucoes deixou passar algo que nao devia. Abortar diz isso na hora. */
void md_idle(void) { }

void md_unresolved_dispatch(uint32_t site, uint32_t alvo) {
    fprintf(stderr, "difftest: despacho indireto em $%06X -> $%06X\n", site, alvo);
    abort();
}

void md_untranslated(uint32_t addr, const char *motivo) {
    fprintf(stderr, "difftest: funcao nao traduzida $%06X (%s)\n", addr, motivo);
    abort();
}
