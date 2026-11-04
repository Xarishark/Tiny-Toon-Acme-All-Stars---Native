/* Execucao do jogo recompilado.
 *
 * Chama o entry point da ROM exatamente como o console faria: SP inicial do
 * header e nada mais. O laco ocioso da ROM ("bra *") chama md_idle, que entrega
 * um frame chamando o handler de V-blank — e' assim que o laco de espera do
 * original vira o laco principal do port.
 *
 * Antes de chamar o jogo: acha e confere a ROM, le' o tinytoon.toml e, no
 * build de distribuicao, carrega a jogo.dll montada da ROM do jogador.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* O diagnostico de travamento por backtrace depende de glibc e de sinais
 * POSIX. No Windows nao existe equivalente direto, e a build cruzada e' so'
 * para o usuario ver o programa rodar — entao ele sai fora. */
#ifndef _WIN32
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#define TEM_BACKTRACE 1
#endif

#include "audio.h"
#include "config.h"
#include "host.h"
#include "input.h"
#include "m68k.h"
#include "memory.h"
#include "mods.h"
#include "release.h"
#include "replay.h"
#include "rom_baseline.h"
#include "rom_check.h"
#include "vdp.h"
#include "versao.h"
#include "z80_bus.h"
#include "ym2612.h"

/* De onde vem o jogo.
 *
 * Monolitico (desenvolvimento): o C recompilado e' linkado neste executavel.
 * TINYTOON_JOGO_DLL (distribuicao): o executavel e' so' o runtime, e o jogo
 * vem da jogo.dll montada na maquina do jogador a partir da ROM dele — porque
 * o C recompilado e' derivado da ROM e nao e' distribuido. Ver jogo_api.h. */
#ifdef TINYTOON_JOGO_DLL
#include "jogo_carga.h"
static void (*jogo_entry)(M68K *cpu);
static void (*jogo_vblank)(M68K *cpu);
#else
void Entry(M68K *cpu);
void Vec_level_6_irq_vblank(M68K *cpu);
#define jogo_entry  Entry
#define jogo_vblank Vec_level_6_irq_vblank
#endif

/* A ROM recusada nao pode continuar lembrada.
 *
 * O seletor grava o caminho escolhido em tinytoon.toml antes de a ROM ser
 * verificada. Se o arquivo era o errado e o caminho ficasse la', toda partida
 * seguinte leria o mesmo arquivo, mostraria o mesmo erro e sairia — sem nunca
 * mais abrir o seletor. Esquecer devolve o jogador ao seletor na proxima vez.
 * So' quando o caminho veio da memoria: um argumento de linha de comando (ou
 * do RecompLauncher) nao e' nosso para apagar. */
static void esquecer_rom_recusada(const char *caminho) {
    const char *lembrado = config_texto("rom");
    if (lembrado && strcmp(lembrado, caminho) == 0)
        config_gravar_texto("rom", "");
}

#define SP_INICIAL 0xFFFFFF00u      /* vetor 0 do header */
#define SEGUNDOS_ATE_DESISTIR 90

/* O alarme de travamento existe para o build headless de diagnostico. Com
 * janela ele mataria a sessao no meio da partida — la' quem decide quando
 * parar e' quem esta' jogando. */
#if defined(TEM_BACKTRACE) && !defined(TINYTOON_SDL)
#define USAR_ALARME 1
#endif

#ifdef TEM_BACKTRACE
/* Falha de memoria com a pilha em nomes do jogo.
 *
 * Sem isto, um estouro de buffer aparece como "dumped core" e nada mais — foi
 * o que aconteceu com o vetor de audio, e custou uma rodada de deducao que a
 * pilha teria respondido de imediato. */
static void ao_estourar(int sinal) {
    void *pilha[24];
    int n = backtrace(pilha, 24);
    const char *aviso = "\n=== falha de memoria; pilha ===\n";
    ssize_t ignorado = write(2, aviso, 32);
    (void)ignorado; (void)sinal;
    backtrace_symbols_fd(pilha, n, 2);
    _exit(6);
}

/* Um laco que nao toca memoria escapa do orcamento de acessos e gira para
 * sempre. O alarme imprime a pilha de onde estiver: como cada rotina do jogo
 * e' uma funcao C de verdade, a pilha sai em nomes do jogo. */
static void ao_travar(int sinal) {
    void *pilha[24];
    int n = backtrace(pilha, 24);
    const char *aviso = "\n=== travou; pilha ===\n";
    ssize_t ignorado = write(2, aviso, 23);
    (void)ignorado; (void)sinal;
    backtrace_symbols_fd(pilha, n, 2);
    md_report_irq();
    fflush(stdout);
    _exit(4);
}
#endif

/* O jogo termina de tres jeitos — limite de frames, orcamento estourado ou
 * abort — e nenhum deles passa por um ponto comum. atexit garante que o que o
 * VDP acumulou vire imagem em qualquer um deles. */
static void despejar(void) {
    vdp_report();
    z80_bus_relatorio();
    ym2612_relatorio();
    audio_relatorio();
    /* Um release nao espalha PNG de diagnostico no diretorio de quem so' quer
     * jogar. O build de desenvolvimento continua despejando: la' isso e' a
     * saida principal, nao lixo. */
    if (release_ativo()) { fflush(stdout); return; }
    struct { const char *nome; int (*fn)(const char *); } saidas[] = {
        {"tela.png",  vdp_dump_screen},
        {"vram.png",  vdp_dump_tiles},
        {"cram.png",  vdp_dump_cram},
    };
    for (unsigned i = 0; i < sizeof saidas / sizeof *saidas; i++) {
        if (saidas[i].fn(saidas[i].nome) == 0) printf("  gravado %s\n", saidas[i].nome);
        else printf("  FALHOU ao gravar %s\n", saidas[i].nome);
    }
    /* Camadas separadas so' quando pedidas. Compor as tres e olhar o resultado
     * diz que algo esta' errado; nao diz qual. */
    if (getenv("TINYTOON_CAMADAS")) {
        if (vdp_dump_layers("camada") == 0) printf("  gravado camada-*.png\n");
        if (vdp_dump_vram_bin("vram.bin") == 0) printf("  gravado vram.bin\n");
        if (md_dump_ram("ram.bin") == 0) printf("  gravado ram.bin\n");
    }
    {
        const char *nome = getenv("TINYTOON_ROMLIDO");
        if (nome && md_dump_rom_lido(nome) == 0) printf("  gravado %s\n", nome);
        if (nome && md_dump_rom_leitor("romleitor.bin") == 0)
            printf("  gravado romleitor.bin\n");
        if (nome && md_dump_visto("visto.bin") == 0)
            printf("  gravado visto.bin\n");
    }
    if (getenv("TINYTOON_CODECS") && md_dump_codecs("codecs.bin") == 0)
        printf("  gravado codecs.bin\n");
    if (md_replay_fechar() == 0)
        printf("  gravado %s\n", getenv("TINYTOON_GRAVAR"));
    fflush(stdout);
}

int main(int argc, char **argv) {
    release_iniciar();

    printf("Tiny Toon Adventures: ACME All-Stars - port nativo, "
           "build %s (%s)\n", TINYTOON_VERSAO, TINYTOON_DATA);
    /* Antes de achar a ROM: o caminho lembrado esta' no arquivo. Relativo de
     * proposito — release_iniciar ja' pos o diretorio do exe como corrente, e
     * fora do release o arquivo fica onde o desenvolvedor roda. */
    config_carregar(CONFIG_ARQUIVO);

    const char *caminho = release_achar_rom(argc > 1 ? argv[1] : NULL);
    if (!caminho) return 2;   /* usuario cancelou o seletor */

    /* O mapa de leitura so' e' gravado quando pedido, e so' custa quando ligado.
     * Uma partida normal nao paga por ele. */
    md_mapa_leitura = getenv("TINYTOON_ROMLIDO") != NULL;
    if (getenv("TINYTOON_CODECS")) md_codecs_ligar();
    {
        /* Replay antes de tudo: o script e' o PREFIXO, e o explorador so' age
         * depois que ele acaba. E' essa ordem que faz uma sequencia boa virar
         * ponto de partida em vez de ser jogada fora a cada corrida. */
        const char *s = getenv("TINYTOON_REPLAY");
        if (s) {
            int n = md_replay_carregar(s);
            if (n < 0) { printf("nao consegui abrir o replay '%s'\n", s); return 4; }
            printf("replay: %d eventos, ate' o quadro %lu\n", n, md_replay_ultimo());
        }
        s = getenv("TINYTOON_GRAVAR");
        if (s) { md_replay_gravar_em(s); printf("gravando input em %s\n", s); }
    }
    {
        const char *s = getenv("TINYTOON_EXPLORAR");
        if (s) { md_explorar_ligar((uint32_t)strtoul(s, NULL, 0));
                 printf("explorador de cobertura ligado, semente %s\n", s); }
    }

    {
        /* TINYTOON_VARRER_SOM=intervalo[:inicio:fim] — os IDs sao uma faixa
         * para a varredura poder ser dividida entre varios processos. */
        const char *s = getenv("TINYTOON_VARRER_SOM");
        if (s) {
            unsigned iv = 0, ini = 0, fim = 0x100;
            sscanf(s, "%u:%u:%u", &iv, &ini, &fim);
            md_varrer_som(iv, ini, fim);
            printf("varredura de som: IDs $%02X..$%02X, um a cada %u quadros\n",
                   ini, fim - 1, iv ? iv : 200u);
        }
    }



    if (md_load_rom(caminho) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "Could not read the file:\n\n%s\n\n"
                 "Check that it exists and can be read.", caminho);
        release_erro("Could not open the ROM", msg);
        esquecer_rom_recusada(caminho);
        return 2;
    }
    printf("ROM carregada: %s\n", caminho);

    /* Dizer O QUE esta' errado, e nao so' que algo esta'.
     *
     * A ROM nunca acompanha o executavel — e' dump do usuario. Entao o programa
     * vai receber arquivo errado, e cada tipo de erro tem um conserto
     * diferente. Bootar em cima de lixo e mostrar tela preta nao ajuda ninguem.
     *
     * Revisao diferente NAO impede de rodar: o jogo provavelmente funciona, e
     * negar seria mais rigido que util. Mas fica dito, com os dois hashes, para
     * a diferenca nao ser atribuida a um bug do port depois. */
    {
        RomVeredito v = rom_verificar(md_rom_dados(), md_rom_tamanho());
        if (v != ROM_OK) {
            release_erro(rom_veredito_curto(v), rom_veredito_longo(v));
            if (v == ROM_OUTRA_REVISAO) {
                uint8_t h[20];
                sha1(md_rom_dados(), md_rom_tamanho(), h);
                printf("\n  esperado: %s\n  seu:      ", ROM_SHA1_TEXTO);
                for (int i = 0; i < 20; i++) printf("%02x", h[i]);
                printf("\n\n");
            } else {
                /* Um codigo POR VEREDITO, e nao um 5 para todos: o
                 * RecompLauncher mostra ao jogador a frase do exit_codes que
                 * corresponde ao codigo, em vez de um relatorio de crash. Com
                 * um codigo so', a frase teria de servir para SMD, truncado e
                 * outro jogo ao mesmo tempo — e nao serviria para nenhum. */
                esquecer_rom_recusada(caminho);
                return SAIDA_ROM_BASE + (int)v;
            }
        }
#ifdef TINYTOON_JOGO_DLL
        /* Outra revisao rodava no build monolitico, com o codigo da revisao
         * analisada sobre os dados da do jogador. Aqui o codigo e' montado DA
         * ROM do jogador, e o recompilador so' conhece a revisao analisada:
         * montar de outra daria um jogo com enderecos errados. Recusar e'
         * o unico honesto. */
        if (v == ROM_OUTRA_REVISAO) {
            esquecer_rom_recusada(caminho);
            return SAIDA_ROM_BASE + (int)v;
        }

        uint8_t h[20];
        char hex[41];
        sha1(md_rom_dados(), md_rom_tamanho(), h);
        for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
        JogoExporta jogo;
        int r = jogo_preparar(caminho, hex, &jogo);
        if (r != 0) return r;
        jogo_entry = jogo.entry;
        jogo_vblank = jogo.vblank;
#endif
    }

    md_reset_ram();
    vdp_reset();
    audio_reset();
    z80_bus_reset();
    atexit(despejar);
    if (host_init() != 0) return 3;
    /* Carregar depois de o hardware estar de pe' e a ROM verificada: um mod que
     * le' estado no md_mod_iniciar precisa que haja estado para ler.
     *
     * A ORDEM DOS atexit IMPORTA, e ela e' INVERSA: o ultimo registrado roda
     * primeiro. mods_encerrar vem antes de host_shutdown aqui justamente para
     * rodar DEPOIS dele — senao os mods seriam descarregados enquanto
     * host_shutdown ainda quer aplica-los ao despejo final, e o tela-mod.png
     * saia sem nenhum mod, em silencio. */
    mods_carregar(release_dir_mods());
    atexit(mods_encerrar);
    atexit(host_shutdown);

    /* Sem limites por padrao: o jogo roda ate' alguem fechar a janela. Os dois
     * argumentos existem para o modo de diagnostico, onde a graca e' justamente
     * parar num ponto escolhido. */
    md_set_budget(argc > 2 ? strtoul(argv[2], NULL, 0) : 0);
    md_frame_limit = argc > 3 ? strtoul(argv[3], NULL, 0) : 0;


#ifdef TEM_BACKTRACE
    signal(SIGSEGV, ao_estourar);
    signal(SIGABRT, ao_estourar);
#endif

#ifdef USAR_ALARME
    signal(SIGALRM, ao_travar);
    {
        /* Corrida de cobertura longa precisa de folga maior que o padrao, mas o
         * alarme nao pode simplesmente sumir: e' ele que transforma travamento
         * em pilha legivel em vez de processo pendurado. */
        const char *s = getenv("TINYTOON_ALARME");
        alarm(s ? (unsigned)strtoul(s, NULL, 0) : SEGUNDOS_ATE_DESISTIR);
    }
#endif

    /* Vigia opcional: 4o argumento liga o rastreio de escrita numa faixa. */
    if (argc > 4) {
        unsigned long base = strtoul(argv[4], NULL, 0);
        md_watch((uint32_t)base, (uint32_t)base + 15, 6);
        printf("vigiando escritas em $%06lX..$%06lX\n", base, base + 15);
    }

    M68K cpu = {0};
    cpu.a[7] = SP_INICIAL;
    md_set_vblank(jogo_vblank, &cpu);

    printf("chamando Entry ($000206)...\n\n");
    fflush(stdout);

    jogo_entry(&cpu);

    printf("\nEntry retornou (o console nunca faria isso)\n");
    md_report();
    return 0;
}
