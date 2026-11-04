#include "memory.h"

#include "audio.h"
#include "psg.h"
#include "ym2612.h"
#include "host.h"
#include "input.h"
#include "replay.h"
#include "m68k.h"
#include "vdp.h"
#include "z80_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <execinfo.h>
#define TEM_BACKTRACE 1
#endif

/* Mapa de leitura da ROM.
 *
 * Um byte por byte da ROM, dizendo COMO ele foi lido:
 *
 *   bit 0  lido pela CPU (operando, tabela, fluxo comprimido)
 *   bit 1  lido pelo DMA do VDP (arte indo direto para a VRAM)
 *   bit 2  lido pela janela de banco do Z80 (driver de som e amostras)
 *
 * Isto e' o que analise estatica sozinha nao da'. Os dados deste jogo sao
 * alcancados por tabelas de ponteiros indexadas em tempo de execucao — segui-las
 * a partir do codigo exigiria analise de fluxo de dados. Executar o jogo
 * responde de graca, e com evidencia melhor: nao "acho que e' dado", e sim "o
 * jogo leu estes bytes, por este caminho".
 *
 * A consequencia pratica e' boa: JOGAR O JOGO VIRA A FERRAMENTA DE EXTRACAO.
 * Cada tela nova visitada classifica mais ROM. */
static uint8_t rom_lido[0x400000];

/* E, para cada byte, QUEM o leu: o endereco de ROM da funcao que estava
 * executando na hora.
 *
 * Este e' o mapa que vale de verdade. Saber que um bloco foi lido diz que ele e'
 * dado; saber que foi lido por sub_012A4E diz o que ele SIGNIFICA, porque a
 * funcao esta' desmontada e da' para ler o que ela faz com aquilo. E blocos
 * contiguos lidos pela mesma funcao sao uma estrutura so' — o mapa recorta os
 * limites sozinho, sem ninguem chutar onde uma tabela acaba.
 *
 * Guarda o PRIMEIRO leitor. Um bloco costuma ter um dono e varios visitantes; o
 * primeiro a chegar e' quase sempre o dono. */
static uint32_t rom_leitor[0x100000];

/* Quais funcoes ja' executaram, um bit por byte de ROM.
 *
 * O mapa de LEITURA diz onde o jogo foi buscar dado. Este diz por onde o jogo
 * PASSOU — e e' um sinal muito mais sensivel para saber se uma sequencia de
 * botoes chegou a uma tela nova. Trocar de menu quase sempre executa codigo
 * novo; nem sempre le' ROM nova.
 *
 * E' o retorno que faz o explorador guiado funcionar: sem ele, o fuzzer nao tem
 * como distinguir "andei em circulos" de "cheguei em algum lugar". */
uint8_t md_visto[0x100000 / 8];

uint32_t md_funcao_atual;
int md_mapa_leitura;
void md_funcao_restaurar(uint32_t *ant) { md_funcao_atual = *ant; }

static uint8_t *rom;
static size_t rom_size;
static uint8_t ram[MD_RAM_SIZE];

/* A RAM do Z80 ($A00000-$A01FFF) mora em z80_bus.c, porque agora ela e'
 * memoria de um processador que EXECUTA. Deixar a copia aqui daria dois donos
 * para o mesmo estado — o 68000 escreveria num e o Z80 leria do outro. */

/* Faixa do VDP: $C00000-$C0001F, com espelhos a cada 32 bytes ate' $C0FFFF.
 *
 *   $00-$03  porta de dados
 *   $04-$07  porta de controle na escrita, status na leitura
 *   $08-$0F  contador HV
 *   $11      PSG
 */
static inline int e_vdp(uint32_t a) { return a >= 0xC00000u && a < 0xC10000u; }

/* Registrador de versao do console, $A10001.
 *
 *   bit 7  0 = Mega CD conectado, 1 = nao
 *   bit 6  0 = domestico (Japao), 1 = overseas
 *   bit 5  0 = NTSC, 1 = PAL
 *   bits 3-0  versao do hardware
 *
 * $E0 = sem Mega CD, overseas, PAL — o console europeu, que e' o que o header
 * desta ROM declara (regiao 'E').
 *
 * Isto nao e' detalhe cosmetico: $003EC2 comeca testando o bit 6 desse registrador e
 * **retorna na hora** se for overseas. Devolvendo zero, o jogo tomava o caminho
 * domestico e caia num "bra *" esperando uma interrupcao que ainda estava
 * mascarada — parecia bug de traducao, e era hardware faltando. */
#define VERSAO_CONSOLE 0xE0u

unsigned long md_unimplemented_accesses;
unsigned long md_frames;
unsigned long md_frame_limit;
unsigned long long md_ciclos_handler;
unsigned long md_quadros_estourados;
unsigned long md_pior_handler;

static md_handler vblank;
static struct M68K *vblank_cpu;

void md_set_vblank(md_handler handler, struct M68K *cpu) {
    vblank = handler;
    vblank_cpu = cpu;
}

/* O RELOGIO DO PORT, EM CICLOS DO 68000.
 *
 * Um quadro PAL vale 7.670.454 / 50 = 153.409 ciclos. Esse numero nao e'
 * estimativa: e' a definicao do hardware.
 *
 * Ate' aqui o relogio andava em ACESSOS A MEMORIA — 46.000 por quadro, valor
 * que eu estimei. O problema nao era a imprecisao, era a grandeza: meu codigo
 * recompilado nao busca instrucao da memoria, entao instrucao que so' mexe em
 * registrador custa ZERO acessos e ciclos reais. Medido no jogo rodando:
 *
 *     3.550.000 ciclos por quadro, contra 153.409 do hardware
 *
 * Ou seja, o jogo ganhava 23 vezes mais tempo de CPU por quadro que num Mega
 * Drive. Nada disso e' visivel enquanto sobra folga; aparece quando o original
 * NAO tem folga, e engasga. Um port que nunca engasga onde o original engasga
 * nao esta' reproduzindo o original.
 *
 * Os ciclos sao emitidos instrucao a instrucao pelo recompilador — ver
 * tools/ciclos.py para as tabelas e para o que elas nao modelam. */
#define CICLOS_POR_QUADRO 153409u
/* O Z80 anda a 3.579.545 Hz contra os 7.670.454 do 68000. */
#define CICLOS_Z80_QUADRO (MD_CLOCK_Z80 / 50u)

static int em_vblank;

/* Se o codigo em execucao esta' dentro do handler de V-blank. O VDP usa isto
 * para distinguir escrita de retraco de escrita NO MEIO DO QUADRO, que e' o
 * que efeito raster quer dizer. */
static uint32_t decorrido(void);
int md_em_vblank(void) { return em_vblank; }

/* Ciclos decorridos no quadro corrente. O VDP deriva dai a linha de varredura. */
uint32_t md_ciclos_no_quadro(void) { return decorrido(); }

/* Cobra ciclos do 68000 sem que ele tenha executado nada.
 *
 * E' o que um DMA faz no hardware: o VDP toma o barramento e a CPU PARA. Sem
 * isto o DMA e' de graca aqui, e mover 128 KB para a VRAM nao custaria tempo
 * nenhum — justamente a operacao mais cara que um jogo de Mega Drive faz. */
void md_cobrar_ciclos(uint32_t n) {
    if (vblank_cpu) vblank_cpu->ciclos += n;
}
static uint32_t ciclos_base;            /* ciclos do 68000 no inicio do quadro */
static unsigned long pares_no_quadro;   /* audio ja' gerado neste quadro */
static unsigned long z80_no_quadro, ym_no_quadro;

/* Quanto do quadro ja' passou, em ciclos, limitado ao quadro. */
static uint32_t decorrido(void) {
    if (!vblank_cpu) return 0;
    uint32_t d = vblank_cpu->ciclos - ciclos_base;
    return d > CICLOS_POR_QUADRO ? CICLOS_POR_QUADRO : d;
}

/* Varredura do banco de som: toca os 256 IDs, um a um.
 *
 * Sobrou aqui o maior pedaco opaco da ROM — 69% do que faltava classificar mora
 * em $0E0000-$0FFFFF, e sao faixas e efeitos que nenhuma partida tocou. O sound
 * test do menu cobre isso, mas depende de alguem navegar ate' la' e apertar 256
 * vezes. Isto faz o mesmo pela porta da frente.
 *
 * Nao ha' truque: PlaySound ($0E5064) so' deposita um byte num anel em RAM e
 * avanca o indice de escrita. Escrever no anel e' EXATAMENTE o que o sound test
 * faz — quem entrega ao Z80 continua sendo SoundQueueDrain, no quadro seguinte,
 * com o protocolo de barramento intacto. O jogo nao sabe que o pedido nao veio
 * de um botao.
 *
 * O intervalo precisa ser generoso: cada faixa carrega seu conjunto de
 * instrumentos e so' entao comeca a puxar amostra da ROM pela janela de banco do
 * Z80. Trocar cedo demais classifica o cabecalho da faixa e mais nada. */
#define VARRER_ESPERA  600u     /* quadros de boot antes de comecar */
static unsigned varrer_intervalo;
static unsigned varrer_id, varrer_fim;

/* A faixa de IDs existe para poder PARALELIZAR.
 *
 * Uma faixa de musica so' revela toda a sua amostra de PCM se tocar por um
 * tempo de verdade — trocar de ID a cada poucos segundos classifica o cabecalho
 * e o conjunto de instrumentos, e para por ai. Mas 256 IDs vezes um minuto cada
 * da' quatro horas numa maquina so'.
 *
 * Dividindo em 16 processos de 16 IDs, viram quinze minutos, e os mapas se
 * fundem no fim como qualquer outra corrida de cobertura. */
void md_varrer_som(unsigned intervalo, unsigned inicio, unsigned fim) {
    varrer_intervalo = intervalo ? intervalo : 200u;
    varrer_id = inicio;
    varrer_fim = fim > 0x100u ? 0x100u : fim;
}

static void varrer_som(void) {
    if (!varrer_intervalo || md_frames < VARRER_ESPERA) return;
    if ((md_frames - VARRER_ESPERA) % varrer_intervalo) return;
    if (varrer_id >= varrer_fim) return;

    uint16_t escrita = (uint16_t)((ram[0xD510] << 8) | ram[0xD511]);
    ram[0xD410 + escrita] = (uint8_t)varrer_id;
    escrita = (uint16_t)((escrita + 1) & 0xFF);
    ram[0xD510] = (uint8_t)(escrita >> 8);
    ram[0xD511] = (uint8_t)escrita;
    ram[0xD401] = 0;
    varrer_id++;
    if (varrer_id >= varrer_fim) {
        fprintf(stdout, "varredura de som: ate' o ID $%02X, pedidos\n");
        fflush(stdout);
    }
}

/* Entrega o V-blank na hora certa, como o hardware faria.
 *
 * Respeita a mascara de interrupcao do SR: o jogo eleva a mascara de interrupcao do SR para 7 em volta
 * de secao critica — o descompressor, por exemplo — e a interrupcao nao pode
 * chegar la' dentro. O guarda de reentrancia existe porque o proprio handler
 * acessa memoria, o que dispararia o tick de novo. */
static void tick(void) {
    if (!vblank || em_vblank) return;
    if (decorrido() < CICLOS_POR_QUADRO) return;
    /* Duas travas, nao uma. A mascara do SR e' a da CPU; o bit 5 do registrador
     * $01 e' a do VDP, e e' com ele que o jogo diz "agora pode me interromper".
     * Ignorar a segunda fazia o handler rodar durante o boot, lendo sombras de
     * registrador ainda zeradas. */
    if (!vdp_vint_habilitada()) { ciclos_base = vblank_cpu->ciclos; return; }
    if (((vblank_cpu->sr_high >> 8) & 7) >= 6) {
        /* Interrupcao mascarada por tempo demais: alguem entrou em secao
         * critica e nao saiu. Relata uma vez, com a pilha, e segue. */
        static int relatado;
        /* O laco de checksum do boot roda mascarado por bastante tempo de
         * proposito, entao o limiar tem que ficar acima dele — senao o aviso
         * dispara todo boot e vira ruido que o usuario aprende a ignorar. */
        if (!relatado && vblank_cpu->ciclos - ciclos_base
                        > 60u * CICLOS_POR_QUADRO) {
            relatado = 1;
            fprintf(stdout, "\n[nota] interrupcao mascarada ha' %u ciclos "
                            "(secao critica longa; pilha abaixo)\n",
                    vblank_cpu->ciclos - ciclos_base);
            fflush(stdout);
#ifdef TEM_BACKTRACE
            void *pilha[20];
            int n = backtrace(pilha, 20);
            backtrace_symbols_fd(pilha, n, 1);
#endif
        }
        return;
    }
    /* Completa o quadro antes de virar: um quadro em que o jogo fez poucos
     * acessos nao pode render menos som que os outros. */
    if (pares_no_quadro < MD_PARES_QUADRO) {
        audio_gerar((unsigned)(MD_PARES_QUADRO - pares_no_quadro));
    }
    if (z80_no_quadro < CICLOS_Z80_QUADRO) {
        z80_bus_executar((unsigned)(CICLOS_Z80_QUADRO - z80_no_quadro));
    }
    if (ym_no_quadro < CICLOS_POR_QUADRO) {
        ym2612_avancar((unsigned)(CICLOS_POR_QUADRO - ym_no_quadro));
    }
    pares_no_quadro = z80_no_quadro = ym_no_quadro = 0;
    ciclos_base = vblank_cpu->ciclos;

    md_frames++;
    /* Zera o mapa de leitura depois do boot.
     *
     * Sem isto o mapa marca 99,95% da ROM e nao diz nada: o boot roda o laco de
     * checksum do header, que le' $200..$FFFFF byte a byte. Ler tudo uma vez
     * para somar nao e' evidencia de que um byte SEJA alguma coisa.
     *
     * Descartar o boot custa as leituras que so' acontecem la' — e essas ja'
     * estao classificadas como codigo pela analise estatica. O que sobra e' o
     * que o jogo procura enquanto joga, que e' exatamente o que falta saber. */
    md_replay_quadro(md_frames);
    md_explorar_quadro();
    varrer_som();
    if (md_frames == 200) memset(rom_lido, 0, sizeof rom_lido);
    if (md_frames == 200) memset(rom_leitor, 0, sizeof rom_leitor);
    if (md_frame_limit && md_frames > md_frame_limit) {
        fprintf(stdout, "limite de %lu frames atingido\n", md_frame_limit);
        md_report();
        fflush(stdout);
        exit(0);
    }
    /* A pilha tem que voltar de onde saiu. Um handler que devolve a7 diferente
     * significa push/pop desbalanceado na traducao, e o estrago so' aparece
     * centenas de frames depois, quando a7 sai da RAM. Detectar aqui e' barato
     * e aponta para a causa, nao para o sintoma. */
    vdp_tick_quadro();
    z80_bus_vblank();
    em_vblank = 1;
    uint32_t sp_antes = vblank_cpu->a[7];
    uint32_t c_antes = vblank_cpu->ciclos;
    vblank(vblank_cpu);
    /* Quanto o handler custou, em ciclos do 68000.
     *
     * Diagnostico permanente porque e' a metrica que diz se o port reproduz o
     * TEMPO do original, e nao so' a logica: quadro que estoura o orcamento e'
     * quadro em que o Mega Drive engasga. Com o relogio antigo, medido em
     * acessos a memoria, NENHUM quadro estourava — o jogo tinha 23 vezes mais
     * folga que no console, e o engasgo nunca acontecia. */
    {
        uint32_t usado = vblank_cpu->ciclos - c_antes;
        md_ciclos_handler += usado;
        if (usado > md_pior_handler) md_pior_handler = usado;
        if (usado >= CICLOS_POR_QUADRO) md_quadros_estourados++;
    }
    /* O V-blank do jogo e' o frame. Apresentar aqui, e nao num laco proprio,
     * e' o que mantem logica e video no mesmo relogio — desacoplar taxa de
     * render da taxa de logica e' melhoria moderna, posterior a fidelidade. */
    host_frame();
    if (vblank_cpu->a[7] != sp_antes) {
        fprintf(stdout, "\n=== pilha desbalanceada no V-blank do frame %lu: "
                        "a7 %08X -> %08X (%+d) ===\n",
                md_frames, sp_antes, vblank_cpu->a[7],
                (int)(vblank_cpu->a[7] - sp_antes));
        fflush(stdout);
        exit(5);
    }
    em_vblank = 0;
}

/* Faz o resto do console andar junto com o 68000.
 *
 * Tudo aqui e' proporcional ao QUANTO DO QUADRO JA' PASSOU, em ciclos, e tudo
 * fecha exato na borda do quadro. Isso da' as duas propriedades que o som e a
 * musica precisam ao mesmo tempo:
 *
 *   dentro do quadro   o DAC e' amostrado no instante certo, e o Z80 executa
 *                      intercalado com o 68000 como no hardware;
 *   entre quadros      a quantidade e' sempre a mesma, entao nada acumula nem
 *                      falta.
 *
 * O YM2612 recebe o MESMO relogio do 68000, entao para ele o alvo e' o proprio
 * numero de ciclos decorridos.
 */
static void avancar_som(void) {
    static int dentro;
    if (dentro || !vblank_cpu) return;   /* o Z80 acessa memoria; nao reentrar */
    dentro = 1;

    uint32_t d = decorrido();

    unsigned long alvo_z80 = (unsigned long)((uint64_t)d * CICLOS_Z80_QUADRO
                                             / CICLOS_POR_QUADRO);
    if (alvo_z80 > z80_no_quadro) {
        z80_bus_executar((unsigned)(alvo_z80 - z80_no_quadro));
        z80_no_quadro = alvo_z80;
    }

    if (d > ym_no_quadro) {
        ym2612_avancar((unsigned)(d - ym_no_quadro));
        ym_no_quadro = d;
    }

    unsigned long alvo_par = (unsigned long)((uint64_t)d * MD_PARES_QUADRO
                                             / CICLOS_POR_QUADRO);
    if (alvo_par > pares_no_quadro) {
        audio_gerar((unsigned)(alvo_par - pares_no_quadro));
        pares_no_quadro = alvo_par;
    }
    dentro = 0;
}

/* Laco "bra *": o jogo esperando de forma explicita.
 *
 * Adiantar o relogio ate' a borda do quadro e' honesto: no console essa espera
 * de fato consome o resto do quadro. E e' necessario — um laco que nao toca
 * memoria nem gasta ciclos de instrucao suficientes travaria o relogio. */
void md_idle(void) {
    if (vblank_cpu) vblank_cpu->ciclos = ciclos_base + CICLOS_POR_QUADRO;
    avancar_som();
    tick();
}

static unsigned long accesses;

static uint32_t watch_lo, watch_hi;
static unsigned watch_restantes;

void md_watch(uint32_t inicio, uint32_t fim, unsigned max_relatos) {
    watch_lo = inicio; watch_hi = fim; watch_restantes = max_relatos;
}

static void relatar_escrita(uint32_t addr, uint8_t valor) {
    /* Zero nao interessa: o boot limpa a RAM inteira e gastaria todos os
     * relatos. O que se procura e' quem escreve valor nao-zero. */
    if (!watch_restantes || !valor || addr < watch_lo || addr > watch_hi) return;
    watch_restantes--;
    fprintf(stdout, "\n[vigia] escrita $%02X em $%06X (acesso %lu, frame %lu)\n",
            valor, addr, accesses, md_frames);
#ifdef TEM_BACKTRACE
    void *pilha[12];
    int n = backtrace(pilha, 12);
    fflush(stdout);
    backtrace_symbols_fd(pilha, n, 1);
#endif
}

static unsigned long budget;
static uint32_t hw_addr[MD_HW_SLOTS];
static unsigned long hw_count[MD_HW_SLOTS];
static unsigned hw_used;

void md_set_budget(unsigned long acessos) { budget = acessos; accesses = 0; }

/* Enquanto o numero acima for estimativa, ele precisa ser facil de varrer. */

static void note_hw(uint32_t addr) {
    md_unimplemented_accesses++;
    for (unsigned i = 0; i < hw_used; i++) {
        if (hw_addr[i] == addr) { hw_count[i]++; return; }
    }
    if (hw_used < MD_HW_SLOTS) {
        hw_addr[hw_used] = addr;
        hw_count[hw_used] = 1;
        hw_used++;
    }
}

void md_report(void) {
    if (md_frames) {
        fprintf(stdout, "tempo: handler de V-blank usa %llu ciclos por quadro em "
                        "media (%llu%% dos %u disponiveis)\n",
                md_ciclos_handler / md_frames,
                md_ciclos_handler * 100 / md_frames / CICLOS_POR_QUADRO,
                CICLOS_POR_QUADRO);
        fprintf(stdout, "  quadros que estouraram o orcamento: %lu de %lu | "
                        "pior caso %lu ciclos (%lu quadros)\n",
                md_quadros_estourados, md_frames, md_pior_handler,
                md_pior_handler / CICLOS_POR_QUADRO);
    }
    fprintf(stdout, "acessos totais: %lu\n", accesses);
    fprintf(stdout, "acessos a hardware nao implementado: %lu, em %u endereco(s)\n",
            md_unimplemented_accesses, hw_used);
    for (unsigned i = 0; i < hw_used; i++) {
        fprintf(stdout, "  $%06X  %lu\n", hw_addr[i], hw_count[i]);
    }
}

void md_report_irq(void) {
    unsigned mascara = vblank_cpu ? ((vblank_cpu->sr_high >> 8) & 7) : 99;
    fprintf(stdout, "frames: %lu | mascara de interrupcao: %u | em_vblank: %d | "
                    "acessos desde o ultimo frame: %lu\n",
            md_frames, mascara, em_vblank,
            vblank_cpu ? (unsigned long)(vblank_cpu->ciclos - ciclos_base) : 0);
}

/* O Z80 e os temporizadores do YM2612 andam no mesmo relogio do 68000.
 *
 * Nao ha' contagem de ciclos do 68000 neste port, entao a referencia e' a mesma
 * do V-blank: um quadro vale ACESSOS_POR_FRAME acessos a memoria. Dai saem
 * quantos ciclos de Z80 e de YM cabem em cada acesso — com a fracao acumulada,
 * porque 1,5 ciclo por acesso truncado para 1 deixaria o som 35% lento.
 *
 * Herda a mesma incerteza de ACESSOS_POR_FRAME: e' parametro do modelo, nao
 * fato. Contar ciclos por instrucao no codigo recompilado conserta os dois de
 * uma vez. */
static void check_budget(void) {
    avancar_som();
    tick();
    if (budget && ++accesses > budget) {
        fprintf(stdout, "\norcamento de %lu acessos estourado - "
                        "provavel espera por hardware nao implementado\n", budget);
        md_report();
        fflush(stdout);
        exit(3);
    }
}

void md_untranslated(uint32_t addr, const char *motivo) {
    fprintf(stdout, "funcao nao traduzida alcancada: $%06X (%s)\n", addr, motivo);
    md_report();
    fflush(stdout);
    abort();
}

void md_unresolved_dispatch(uint32_t site, uint32_t target) {
    fprintf(stdout,
            "despacho indireto sem entrada catalogada: site $%06X -> $%06X\n"
            "acrescente a entrada em analysis/jump-tables.csv\n",
            site, target);
    fprintf(stdout, "frames entregues ate' aqui: %lu\n", md_frames);
    md_report();
    fflush(stdout);
    abort();
}

int md_load_rom(const char *path) {
    FILE *fh = fopen(path, "rb");
    if (!fh) return -1;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (size <= 0) { fclose(fh); return -1; }
    rom = malloc((size_t)size);
    if (!rom) { fclose(fh); return -1; }
    rom_size = fread(rom, 1, (size_t)size, fh);
    fclose(fh);
    return rom_size == (size_t)size ? 0 : -1;
}

const uint8_t *md_ram_ponteiro(void) { return ram; }
const uint8_t *md_rom_dados(void) { return rom; }
size_t md_rom_tamanho(void) { return rom_size; }

void md_reset_ram(void) { memset(ram, 0, sizeof ram); }

/* RAM crua, 64 KB, para analisar fora do runtime.
 *
 * A tabela de objetos do jogo vive em $FFC000 (20 entradas) e e' de onde saem
 * as posicoes dos sprites. Poder ler isso em Python e' o que permite responder
 * "a logica calculou errado ou o renderizador desenhou errado" sem instrumentar
 * o codigo gerado. */
/* Grava o mapa de leitura, ACUMULANDO com o que ja' estiver no arquivo.
 *
 * Acumular e' o ponto: cada sessao de jogo visita telas diferentes, e o mapa
 * cresce ate' cobrir o jogo inteiro. Sobrescrever perderia o trabalho das
 * sessoes anteriores. */
/* Grava o mapa de leitor: 4 bytes big-endian por byte de ROM. Acumula tambem,
 * mas sem sobrescrever leitor ja' registrado — primeiro leitor vence. */
int md_dump_rom_leitor(const char *path) {
    static uint32_t antes[0x100000];
    size_t tam = rom_size && rom_size < 0x100000 ? rom_size : 0x100000;

    FILE *fh = fopen(path, "rb");
    if (fh) {
        size_t n = fread(antes, 4, tam, fh);
        for (size_t i = 0; i < n; i++) {
            uint32_t v = __builtin_bswap32(antes[i]);
            if (v && !rom_leitor[i]) rom_leitor[i] = v;
        }
        fclose(fh);
    }
    fh = fopen(path, "wb");
    if (!fh) return -1;
    for (size_t i = 0; i < tam; i++) antes[i] = __builtin_bswap32(rom_leitor[i]);
    size_t n = fwrite(antes, 4, tam, fh);
    return (fclose(fh) == 0 && n == tam) ? 0 : -1;
}

/* Grava o mapa de funcoes executadas, acumulando como os outros. */
int md_dump_visto(const char *path) {
    static uint8_t antes[sizeof md_visto];

    FILE *fh = fopen(path, "rb");
    if (fh) {
        size_t n = fread(antes, 1, sizeof antes, fh);
        for (size_t i = 0; i < n; i++) md_visto[i] |= antes[i];
        fclose(fh);
    }
    fh = fopen(path, "wb");
    if (!fh) return -1;
    size_t n = fwrite(md_visto, 1, sizeof md_visto, fh);
    return (fclose(fh) == 0 && n == sizeof md_visto) ? 0 : -1;
}

int md_dump_rom_lido(const char *path) {
    static uint8_t antes[0x400000];
    size_t tam = rom_size ? rom_size : 0x100000;

    FILE *fh = fopen(path, "rb");
    if (fh) {
        size_t n = fread(antes, 1, tam, fh);
        for (size_t i = 0; i < n; i++) rom_lido[i] |= antes[i];
        fclose(fh);
    }
    fh = fopen(path, "wb");
    if (!fh) return -1;
    size_t n = fwrite(rom_lido, 1, tam, fh);
    return (fclose(fh) == 0 && n == tam) ? 0 : -1;
}

int md_dump_ram(const char *path) {
    FILE *fh = fopen(path, "wb");
    if (!fh) return -1;
    size_t n = fwrite(ram, 1, sizeof ram, fh);
    return (fclose(fh) == 0 && n == sizeof ram) ? 0 : -1;
}

/* O 68000 do Mega Drive so' tem 24 linhas de endereco: tudo acima de $FFFFFF
 * e' espelhado. Sem esta mascara, o endereco absoluto curto estendido com
 * sinal ($FFFFFB04) cairia fora do mapa. */
static inline uint32_t bus(uint32_t addr) { return addr & 0xFFFFFFu; }

/* Leitura sem efeito colateral, para o DMA do VDP.
 *
 * O DMA move ate' 128 KB de uma vez. Se cada byte passasse por md_read8, cada
 * transferencia adiantaria o relogio de frames em milhares de acessos e
 * dispararia V-blank no meio de si mesma. No hardware o DMA e' o barramento
 * sendo usado pelo VDP, nao pela CPU — e' outro caminho, e aqui tambem. */
/* Escrita crua, sem contar acesso: o outro lado da md_peek8, usada pela janela
 * de 32 KB que o Z80 enxerga em $8000-$FFFF. */
void md_poke8(uint32_t addr, uint8_t v) {
    uint32_t a = bus(addr);
    if (a >= 0xFF0000u) { ram[a & 0xFFFFu] = v; return; }
    if (a >= 0xA00000u && a < 0xA02000u) { z80_bus_escrever_ram(a, v); return; }
}

/* Verdade de campo para os descompressores em Python.
 *
 * tools/codecs.py reimplementa LzDecompressA, LzDecompressB e RleUnpack. Saber
 * que a reimplementacao CONSOME os mesmos bytes nao basta: consumir igual nao e'
 * produzir igual. Medido: um fluxo aceito pelo LzA nao prova nada sozinho, porque
 * 99,5% de offsets aleatorios da ROM tambem "decodificam" — o codec nao tem
 * checksum e o token de fim aparece por acaso.
 *
 * Entao o jogo entrega o gabarito. Enquanto um dos tres codecs executa, isto
 * anota de onde ele leu e o que ele escreveu. Comparar byte a byte com a saida
 * do Python e' o mesmo padrao de referencia independente que o projeto usa em
 * todos os outros portoes: vasm contra a ROM, Musashi contra o C gerado, Nuked
 * contra o FM.
 *
 * Uma corrida termina quando a leitura deixa de ser sequencial — os tres codecs
 * varrem a origem para frente, sem voltar. */
#define CODEC_MAX      64        /* corridas gravadas por sessao */
#define CODEC_SAIDA  65536       /* teto de bytes por corrida */

typedef struct {
    uint32_t leitor, origem, consumido, produzido;
    uint8_t saida[CODEC_SAIDA];
} Corrida;

static Corrida *codec_corridas;
static int codec_n;
static uint32_t codec_leitor_atual, codec_prox_esperado;

static int e_codec(uint32_t fn) {
    return fn == 0x001DAEu || fn == 0x001E7Cu || fn == 0x01AD02u;
}

static void codec_ler(uint32_t a) {
    if (!codec_corridas || !e_codec(md_funcao_atual)) { codec_leitor_atual = 0; return; }
    if (md_funcao_atual != codec_leitor_atual || a != codec_prox_esperado) {
        if (codec_n >= CODEC_MAX) { codec_leitor_atual = 0; return; }
        codec_n++;
        codec_corridas[codec_n - 1].leitor = md_funcao_atual;
        codec_corridas[codec_n - 1].origem = a;
        codec_leitor_atual = md_funcao_atual;
    }
    if (codec_n) codec_corridas[codec_n - 1].consumido = a - codec_corridas[codec_n - 1].origem + 1;
    codec_prox_esperado = a + 1;
}

/* O anel dos dois LZ mora em $FFFF8800..$FFFF8BFF e e' escrito pela MESMA
 * instrucao-irma que escreve a saida — os dois passam por md_write8. Sem excluir
 * a faixa do anel, o gabarito sairia com a saida e o anel intercalados byte a
 * byte, e a comparacao com o Python nunca fecharia. */
static void codec_escrever(uint32_t a, uint8_t v) {
    if (!codec_corridas || !codec_n || !e_codec(md_funcao_atual)) return;
    uint16_t off = (uint16_t)(a & 0xFFFFu);
    if (off >= 0x8800u && off < 0x8C00u) return;
    Corrida *c = &codec_corridas[codec_n - 1];
    if (c->leitor != md_funcao_atual) return;
    if (c->produzido < CODEC_SAIDA) c->saida[c->produzido++] = v;
}

int md_dump_codecs(const char *path) {
    if (!codec_corridas || !codec_n) return -1;
    FILE *fh = fopen(path, "wb");
    if (!fh) return -1;
    for (int i = 0; i < codec_n; i++) {
        Corrida *c = &codec_corridas[i];
        uint32_t cab[4] = { c->leitor, c->origem, c->consumido, c->produzido };
        for (int k = 0; k < 4; k++) cab[k] = __builtin_bswap32(cab[k]);
        fwrite(cab, 4, 4, fh);
        fwrite(c->saida, 1, c->produzido, fh);
    }
    return fclose(fh) == 0 ? 0 : -1;
}

void md_codecs_ligar(void) {
    codec_corridas = calloc(CODEC_MAX, sizeof *codec_corridas);
}

static inline void marcar_leitura(uint32_t a, uint8_t via) {
    rom_lido[a] |= via;
    if (a < 0x100000u && !rom_leitor[a]) rom_leitor[a] = md_funcao_atual;
    codec_ler(a);
}

/* Leitura pela janela de banco do Z80. Registra o PC do Z80 como leitor, com o
 * bit alto ligado para nao confundir com endereco de funcao do 68000: saber
 * QUAL rotina do driver buscou o dado e' o que separa amostra de instrumento. */
uint8_t md_peek8_z80(uint32_t addr, uint16_t pc_z80) {
    uint32_t a = bus(addr);
    if (a < 0x400000u) {
        if (md_mapa_leitura) {
            rom_lido[a] |= 4;
            if (a < 0x100000u && !rom_leitor[a])
                rom_leitor[a] = 0x80000000u | pc_z80;
        }
        return a < rom_size ? rom[a] : 0xFF;
    }
    return md_peek8(addr);
}

uint8_t md_peek8(uint32_t addr) {
    uint32_t a = bus(addr);
    if (a < 0x400000u) {
        if (md_mapa_leitura) marcar_leitura(a, 2);
        return a < rom_size ? rom[a] : 0xFF;
    }
    if (a >= 0xFF0000u) return ram[a & 0xFFFFu];
    if (a >= 0xA00000u && a < 0xA02000u) return z80_bus_ler_ram(a);
    return 0;
}

uint16_t md_read16(uint32_t addr) {
    uint32_t a = bus(addr);
    if (e_vdp(a)) {
        check_budget();
        switch (a & 0x1Fu) {
        case 0x00: case 0x02: return vdp_read_data();
        case 0x04: case 0x06: return vdp_read_status();
        case 0x08: case 0x0A: case 0x0C: case 0x0E: return vdp_read_hv();
        default: note_hw(a); return 0;
        }
    }
    return (uint16_t)((md_read8(addr) << 8) | md_read8(addr + 1));
}

void md_write16(uint32_t addr, uint16_t value) {
    uint32_t a = bus(addr);
    if (e_vdp(a)) {
        check_budget();
        switch (a & 0x1Fu) {
        case 0x00: case 0x02: vdp_write_data(value); return;
        case 0x04: case 0x06: vdp_write_control(value); return;
        default: note_hw(a); return;
        }
    }
    md_write8(addr, (uint8_t)(value >> 8));
    md_write8(addr + 1, (uint8_t)value);
}

uint8_t md_read8(uint32_t addr) {
    uint32_t a = bus(addr);
    if (e_vdp(a)) {
        uint16_t v = md_read16(a & ~1u);
        return (uint8_t)((a & 1) ? v : (v >> 8));
    }
    check_budget();
    if (a < 0x400000u) {
        if (md_mapa_leitura) marcar_leitura(a, 1);
        return a < rom_size ? rom[a] : 0xFF;
    }
    if (a >= 0xFF0000u) return ram[a & 0xFFFFu];
    if (a >= 0xA00000u && a < 0xA02000u) return z80_bus_ler_ram(a);
    if (a == 0xA11100u || a == 0xA11101u) return z80_bus_estado();
    if (a == 0xA10001u) return VERSAO_CONSOLE;
    /* Portas de controle: $A10003/$A10005 sao dados dos joypads 1 e 2, e
     * $A10009/$A1000B a direcao dos pinos. O jogo alterna a linha TH e le' de
     * volta — o controle e' um multiplexador, nao um registrador. */
    if (a == 0xA10003u) return md_pad_read(0);
    if (a == 0xA10005u) return md_pad_read(1);
    note_hw(a);
    return 0;
}

void md_write8(uint32_t addr, uint8_t value) {
    uint32_t a = bus(addr);
    /* O PSG mora dentro da faixa do VDP, em $C00011, e e' de byte. Tem que vir
     * antes do desvio generico: senao a escrita vira palavra e some. */
    if ((a & 0xFFFF01u) == 0xC00001u && (a & 0x1Fu) == 0x11u) {
        check_budget();
        psg_escrever(value);
        return;
    }
    /* Escrita de byte nas portas do VDP duplica o byte nas duas metades — e'
     * o que o barramento de 16 bits faz, e o jogo depende disso ao escrever
     * um unico byte na porta de dados. */
    if (e_vdp(a)) { md_write16(a & ~1u, (uint16_t)((value << 8) | value)); return; }

    check_budget();
    if (a >= 0xFF0000u) {
        relatar_escrita(a, value);
        codec_escrever(a, value);
        ram[a & 0xFFFFu] = value;
        return;
    }
    if (a < 0x400000u) return;      /* ROM: escrita ignorada, como no cartucho */
    if (a >= 0xA00000u && a < 0xA02000u) { z80_bus_escrever_ram(a, value); return; }
    /* $A11100 e' BUSREQ, $A11200 e' RESET. O jogo escreve palavras ($100/$0),
     * entao o bit que importa cai no byte PAR — o impar recebe zero e nao pode
     * ser interpretado como "solta o barramento". */
    if (a == 0xA11100u) { z80_bus_busreq(value); return; }
    if (a == 0xA11200u) { z80_bus_reset_linha(value); return; }
    if (a == 0xA11101u || a == 0xA11201u) return;
    if (a == 0xA10003u) { md_pad_write(0, value); return; }
    if (a == 0xA10005u) { md_pad_write(1, value); return; }
    if (a == 0xA10009u) { md_pad_set_ctrl(0, value); return; }
    if (a == 0xA1000Bu) { md_pad_set_ctrl(1, value); return; }
    note_hw(a);
}

uint32_t md_read32(uint32_t addr) {
    return ((uint32_t)md_read16(addr) << 16) | md_read16(addr + 2);
}

void md_write32(uint32_t addr, uint32_t value) {
    md_write16(addr, (uint16_t)(value >> 16));
    md_write16(addr + 2, (uint16_t)value);
}
