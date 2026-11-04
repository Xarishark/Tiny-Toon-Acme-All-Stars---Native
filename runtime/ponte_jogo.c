/* O lado do JOGO da fronteira de jogo_api.h. Compilado dentro da jogo.dll,
 * junto com o C recompilado, pelo montador — nunca dentro do executavel.
 *
 * O codigo gerado chama md_read8, md_write16 e companhia como funcoes comuns.
 * Aqui elas existem e repassam pela tabela que o runtime entregou em
 * jogo_ligar. Assim o C gerado e' o MESMO do build monolitico, byte a byte; so'
 * o que esta' do outro lado da chamada muda.
 *
 * JOGO_ROM_SHA1 vem da linha de comando do montador: e' o SHA-1 da ROM de que
 * esta DLL foi gerada. O runtime confere contra a ROM que carregou, e uma DLL
 * velha depois de o jogador trocar de arquivo vira mensagem, nao jogo
 * corrompido.
 */
#include "jogo_api.h"
#include "m68k.h"
#include "memory.h"

#ifndef JOGO_ROM_SHA1
#error "o montador passa -DJOGO_ROM_SHA1=\"...\""
#endif
#ifndef JOGO_RECOMP_SHA256
#define JOGO_RECOMP_SHA256 ""   /* montado sem referencia (fora do pacote) */
#endif

#ifdef _WIN32
#define EXPORTA __declspec(dllexport)
#else
#define EXPORTA __attribute__((visibility("default")))
#endif

void Entry(M68K *cpu);
void Vec_level_6_irq_vblank(M68K *cpu);

static JogoImporta rt;

uint8_t  md_read8(uint32_t a)               { return rt.read8(a); }
uint16_t md_read16(uint32_t a)              { return rt.read16(a); }
uint32_t md_read32(uint32_t a)              { return rt.read32(a); }
void md_write8(uint32_t a, uint8_t v)       { rt.write8(a, v); }
void md_write16(uint32_t a, uint16_t v)     { rt.write16(a, v); }
void md_write32(uint32_t a, uint32_t v)     { rt.write32(a, v); }
void md_idle(void)                          { rt.idle(); }
void md_unresolved_dispatch(uint32_t s, uint32_t t) { rt.unresolved_dispatch(s, t); }
void md_untranslated(uint32_t a, const char *m)     { rt.untranslated(a, m); }

EXPORTA int jogo_ligar(const JogoImporta *imp, JogoExporta *saida) {
    rt = *imp;
    saida->versao = JOGO_API_VERSAO;
    saida->rom_sha1 = JOGO_ROM_SHA1;
    saida->recomp_sha256 = JOGO_RECOMP_SHA256;
    saida->entry = Entry;
    saida->vblank = Vec_level_6_irq_vblank;
    return 0;
}
