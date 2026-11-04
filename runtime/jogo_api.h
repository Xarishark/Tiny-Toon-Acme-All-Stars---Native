/* A fronteira entre o runtime e o jogo recompilado, quando os dois sao
 * binarios separados.
 *
 * POR QUE SEPARAR. O C recompilado e' traducao mecanica do codigo da Konami —
 * obra derivada da ROM — e o projeto nao o distribui (public/LEGAL.md). Entao
 * o executavel distribuido e' so' o runtime, e o jogo vira jogo.dll (jogo.so
 * no Linux), montada NA MAQUINA DE QUEM JOGA, a partir da ROM dele, pelo
 * montador (tools/montar_jogo.py) com o TinyCC embarcado.
 *
 * A FRONTEIRA E' PEQUENA, e isso foi medido, nao suposto: com MD_FUNCAO
 * desligada (TINYTOON_JOGADOR), o codigo gerado chama exatamente as nove
 * funcoes da tabela abaixo, e o runtime precisa exatamente de duas funcoes do
 * jogo. Nada de variavel global atravessando a fronteira — so' funcoes, por
 * ponteiro, o que funciona igual em DLL do Windows e em .so, e com qualquer
 * compilador dos dois lados.
 *
 * O build de desenvolvimento continua monolitico (tudo num executavel, gcc),
 * e nem inclui este arquivo.
 */

#ifndef TINYTOON_JOGO_API_H
#define TINYTOON_JOGO_API_H

#include <stdint.h>

struct M68K;

/* Muda quando a tabela ou o significado de um campo muda. O runtime recusa
 * uma jogo.dll de outra versao em vez de chamar ponteiro no lugar errado. */
#define JOGO_API_VERSAO 1

/* O que o runtime oferece ao jogo. */
typedef struct {
    uint8_t  (*read8)(uint32_t addr);
    uint16_t (*read16)(uint32_t addr);
    uint32_t (*read32)(uint32_t addr);
    void (*write8)(uint32_t addr, uint8_t value);
    void (*write16)(uint32_t addr, uint16_t value);
    void (*write32)(uint32_t addr, uint32_t value);
    void (*idle)(void);
    void (*unresolved_dispatch)(uint32_t site, uint32_t target);
    void (*untranslated)(uint32_t addr, const char *motivo);
} JogoImporta;

/* O que o jogo devolve ao runtime. */
typedef struct {
    int versao;                       /* JOGO_API_VERSAO de quem montou */
    const char *rom_sha1;             /* SHA-1 da ROM de que foi montado */
    /* SHA-256 do C de que foi montado (montador/recomp.sha256 da epoca).
     * Uma atualizacao que corrija o recompilador muda esse hash; sem esta
     * comparacao a jogo.dll velha — mesma ROM, mesma fronteira — continuaria
     * sendo usada, com o bug que a atualizacao veio corrigir. */
    const char *recomp_sha256;
    void (*entry)(struct M68K *cpu);  /* Entry ($000206) */
    void (*vblank)(struct M68K *cpu); /* Vec_level_6_irq_vblank */
} JogoExporta;

/* A unica funcao exportada pela jogo.dll. Guarda a tabela e preenche `saida`.
 * Devolve 0 se as versoes conferem. */
typedef int (*jogo_ligar_fn)(const JogoImporta *imp, JogoExporta *saida);
#define JOGO_LIGAR_NOME "jogo_ligar"

#endif /* TINYTOON_JOGO_API_H */
