/* Barramento de memoria do Mega Drive visto pelo 68000.
 *
 * A logica recompilada so' toca hardware por aqui. Essa fronteira e' o que
 * depois permite trocar a implementacao do VDP sem editar codigo de jogo.
 *
 * Mapa (de MEMORY_REGIONS em tools/romlib.py):
 *   $000000-$3FFFFF  ROM do cartucho
 *   $A00000-$A0FFFF  espaco do Z80
 *   $A10000-$A1001F  I/O
 *   $C00000-$C0001F  VDP
 *   $FF0000-$FFFFFF  RAM de trabalho, 64 KB
 */

#ifndef TINYTOON_MEMORY_H
#define TINYTOON_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define MD_RAM_SIZE 0x10000u

/* Carrega a ROM. Devolve 0 em sucesso. */
int md_load_rom(const char *path);
void md_reset_ram(void);

/* RAM de trabalho para leitura, sem copia. Indice 0 e' $FF0000. E' o que os
 * mods recebem em MdModHost; ver runtime/mod.h. */
const uint8_t *md_ram_ponteiro(void);

/* A ROM carregada, para quem precisa examinar o arquivo em vez de executar
 * a partir dele: runtime/rom_check.c decide se e' a ROM certa. */
const uint8_t *md_rom_dados(void);
size_t md_rom_tamanho(void);

uint8_t md_read8(uint32_t addr);
uint16_t md_read16(uint32_t addr);
uint32_t md_read32(uint32_t addr);

/* Leitura crua, sem contar acesso nem adiantar o relogio de frames. E' o
 * caminho do DMA do VDP, que no hardware usa o barramento sem passar pela
 * CPU — e que move ate' 128 KB por vez. */
uint8_t md_peek8(uint32_t addr);
uint8_t md_peek8_z80(uint32_t addr, uint16_t pc_z80);
void md_poke8(uint32_t addr, uint8_t v);

/* RAM crua, 64 KB. Diagnostico: a tabela de objetos em $FFC000 e' de onde
 * saem as posicoes dos sprites. */
int md_dump_ram(const char *path);

/* Mapa de leitura da ROM: um byte por byte, bit 0 = lido pela CPU, bit 1 = pelo
 * DMA. Acumula entre sessoes — e' o que faz jogar virar extracao de dados. */
int md_dump_rom_lido(const char *path);
int md_dump_rom_leitor(const char *path);
int md_dump_visto(const char *path);

/* Gabarito dos descompressores: o que os codecs do jogo leram e produziram, para
 * conferir contra tools/codecs.py. Ver a justificativa em memory.c. */
void md_codecs_ligar(void);
int md_dump_codecs(const char *path);

/* Varredura do banco de som: pede os 256 IDs, um a cada `intervalo` quadros,
 * pelo mesmo anel em RAM que o sound test usa. Ver memory.c. */
void md_varrer_som(unsigned intervalo, unsigned inicio, unsigned fim);

/* Instrumentacao do mapa de leitura.
 *
 * MD_FUNCAO marca quem esta' executando. O atributo cleanup do gcc restaura o
 * valor anterior em QUALQUER saida da funcao — inclusive nos varios rts no meio
 * do corpo, que sao a regra e nao a excecao no codigo recompilado. Salvar na
 * entrada e restaurar "no fim" a mao erraria em quase toda funcao.
 *
 * Custa duas operacoes de memoria por chamada e so' grava mapa quando ligado. */
extern uint8_t md_visto[0x100000 / 8];
extern uint32_t md_funcao_atual;
extern int md_mapa_leitura;
void md_funcao_restaurar(uint32_t *ant);
#ifdef TINYTOON_JOGADOR
/* Build do jogador (jogo.dll, montada pelo TinyCC na maquina de quem joga):
 * sem mapa de leitura. Ele e' ferramenta de analise, nao de jogo — e e' o
 * unico motivo de o codigo gerado precisar de variaveis do runtime e de um
 * atributo do gcc. Sem ele, a fronteira jogo/runtime fica em nove funcoes,
 * todas em jogo_api.h. */
#define MD_FUNCAO(addr) (void)(addr)
#else
#define MD_FUNCAO(addr)                                                     \
    uint32_t md_fn_ant __attribute__((cleanup(md_funcao_restaurar))) =      \
        md_funcao_atual;                                                    \
    md_funcao_atual = (addr);                                               \
    if (md_mapa_leitura)                                                    \
        md_visto[(addr) >> 3] |= (uint8_t)(1u << ((addr) & 7));             \
    (void)md_fn_ant
#endif

void md_write8(uint32_t addr, uint8_t value);
void md_write16(uint32_t addr, uint16_t value);
void md_write32(uint32_t addr, uint32_t value);

/* Contador de acessos a hardware ainda nao implementado. Enquanto o runtime
 * nao tiver VDP nem Z80, isso e' o termometro do que falta: se uma funcao
 * recompilada roda com o contador em zero, ela e' pura logica e da' para
 * validar sem emular periferico nenhum. */
extern unsigned long md_unimplemented_accesses;

/* Despacho indireto que caiu fora das entradas catalogadas em
 * analysis/jump-tables.csv. Nao e' um erro do runtime: e' a analise estatica
 * dizendo que a tabela esta' incompleta. Aborta em vez de seguir em silencio,
 * porque continuar significaria executar o alvo errado. */
void md_unresolved_dispatch(uint32_t site, uint32_t target);

/* Instrumentacao da primeira execucao.
 *
 * O codigo recompilado nao tem contador de instrucoes, entao um laco de espera
 * por hardware (o jogo esperando um bit do VDP que nunca muda) travaria para
 * sempre. O orcamento de acessos transforma isso num relatorio: ao estourar,
 * o runtime imprime quais enderecos de hardware foram procurados e quantas
 * vezes, que e' exatamente a lista do que implementar em seguida. */
/* Funcao que o recompilador ainda nao consegue traduzir. Gera um stub em vez
 * de deixar simbolo indefinido: o link fecha e, se a execucao chegar la', o
 * erro diz exatamente qual endereco falta. */
void md_untranslated(uint32_t addr, const char *motivo);

/* Laco ocioso do jogo.
 *
 * O codigo de boot termina em "bra *" — um laco que gira para sempre esperando
 * a interrupcao de V-blank, que e' quem faz todo o trabalho. O recompilador
 * reconhece esse desvio-para-si-mesmo e insere uma chamada a md_idle, que
 * entrega um frame: chama o handler de V-blank e volta. E' assim que o laco
 * ocioso da ROM vira o laco de frames do port. */
struct M68K;
typedef void (*md_handler)(struct M68K *cpu);
void md_set_vblank(md_handler handler, struct M68K *cpu);
void md_idle(void);
extern unsigned long md_frames;
extern unsigned long md_frame_limit;

/* Custo do handler de V-blank, em ciclos do 68000. E' a metrica que diz se o
 * port reproduz o TEMPO do original: quadro que estoura o orcamento de 153.409
 * ciclos e' quadro em que o Mega Drive engasga. */
extern unsigned long long md_ciclos_handler;
extern unsigned long md_quadros_estourados;
extern unsigned long md_pior_handler;

/* Estamos dentro do handler de V-blank? */
int md_em_vblank(void);

/* Ciclos decorridos no quadro corrente, para o VDP derivar a linha. */
uint32_t md_ciclos_no_quadro(void);

/* Cobra ciclos do 68000 sem execucao: e' o que o DMA faz, tomando o barramento. */
void md_cobrar_ciclos(uint32_t n);

/* Vigia de escrita: registra quem escreve numa faixa de RAM, com a pilha no
 * momento da escrita. Como cada rotina do jogo e' uma funcao C de verdade, a
 * pilha identifica o culpado pelo nome. So' existe onde ha' backtrace(). */
void md_watch(uint32_t inicio, uint32_t fim, unsigned max_relatos);

void md_set_budget(unsigned long acessos);


void md_report(void);

/* Por que o V-blank parou de chegar: mascara de interrupcao e reentrancia. */
void md_report_irq(void);

#define MD_HW_SLOTS 32

#endif /* TINYTOON_MEMORY_H */
