/* Carregador de mods.
 *
 * Varre `mods/` ao lado do executavel, abre cada biblioteca compartilhada e
 * chama os ganchos que ela implementar. A interface esta' em runtime/mod.h, que
 * e' o unico arquivo que um autor de mod precisa.
 *
 * POR QUE PONTEIRO DE FUNCAO E NAO SIMBOLO EXPORTADO
 *
 * O mod recebe um MdModHost com tudo de que precisa. Ele nunca resolve um nome
 * do executavel. A alternativa obvia — o mod chamar md_frames() e o linker
 * resolver — nao funcionaria aqui: o build de release e' estatico e passa por
 * `-s`, entao nao ha' tabela de simbolos para ninguem consultar. Exportar
 * simbolos so' para os mods significaria um executavel diferente do que se
 * distribui, e um mod testado num e quebrado no outro.
 *
 * O efeito colateral e' bom: a fronteira fica explicita. O que esta' em
 * MdModHost e' contrato; o resto do runtime pode ser reescrito sem avisar
 * ninguem.
 *
 * FALHA E' RELATADA, NUNCA SILENCIOSA
 *
 * Mod que nao abre, que declara ABI desconhecida ou que recusa em md_mod_iniciar
 * aparece no log com o motivo. Um mod que nao carrega e nao diz nada e' pior que
 * um que nao existe: a pessoa fica achando que o filtro esta' ligado e que o
 * jogo e' que esta' estranho.
 */

#include "mods.h"

#include "memory.h"
#include "vdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE Biblioteca;
#define ABRIR(p)        LoadLibraryA(p)
#define SIMBOLO(h, n)   ((void *)GetProcAddress((h), (n)))
#define FECHAR(h)       FreeLibrary(h)
#define EXTENSAO        ".dll"
#else
#include <dlfcn.h>
#include <dirent.h>
typedef void *Biblioteca;
#define ABRIR(p)        dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define SIMBOLO(h, n)   dlsym((h), (n))
#define FECHAR(h)       dlclose(h)
#if defined(__APPLE__)
#define EXTENSAO        ".dylib"
#else
#define EXTENSAO        ".so"
#endif
#endif

#define MAX_MODS 16

typedef struct {
    Biblioteca lib;
    MdModInfo *info;
    void (*quadro)(void);
    void (*pixels)(unsigned char *, int, int);
    void (*encerrar)(void);
} Mod;

static Mod mods[MAX_MODS];
static int n_mods;

/* O mod corrente, para o log saber de quem e' a linha. Nao ha' concorrencia:
 * os ganchos sao chamados em sequencia, no laco de quadro. */
static const char *quem;

static void mod_log(const char *fmt, ...) {
    va_list ap;
    printf("[mod %s] ", quem ? quem : "?");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static unsigned long host_quadro(void) { return md_frames; }
static const uint8_t  *host_ram(void)  { return md_ram_ponteiro(); }
static const uint8_t  *host_vram(void) { return vdp_vram_ponteiro(); }
static const uint16_t *host_cram(void) { return vdp_cram_ponteiro(); }
static const uint8_t  *host_regs(void) { return vdp_regs_ponteiro(); }
static void host_tela(int *w, int *h)  { vdp_screen_size(w, h); }

static const MdModHost HOST = {
    MD_MOD_ABI, mod_log, host_quadro, host_ram, host_vram, host_cram,
    host_regs, host_tela,
};

/* Abre uma biblioteca e a aceita, ou explica por que nao. */
static void carregar(const char *caminho, const char *nome_arquivo) {
    if (n_mods >= MAX_MODS) {
        printf("[mods] %s ignorado: limite de %d mods\n", nome_arquivo, MAX_MODS);
        return;
    }
    Biblioteca lib = ABRIR(caminho);
    if (!lib) {
        printf("[mods] %s NAO abriu (dependencia faltando, ou nao e' uma "
               "biblioteca deste sistema)\n", nome_arquivo);
        fflush(stdout);
        return;
    }

    MdModInfo *(*info_fn)(void) = (MdModInfo *(*)(void))SIMBOLO(lib, "md_mod_info");
    if (!info_fn) {
        printf("[mods] %s NAO carregado: nao exporta md_mod_info\n", nome_arquivo);
        FECHAR(lib);
        fflush(stdout);
        return;
    }
    MdModInfo *info = info_fn();
    if (!info || info->abi != MD_MOD_ABI) {
        printf("[mods] %s NAO carregado: ABI %d, este runtime fala %d\n",
               nome_arquivo, info ? info->abi : -1, MD_MOD_ABI);
        FECHAR(lib);
        fflush(stdout);
        return;
    }

    int (*iniciar)(const MdModHost *) =
        (int (*)(const MdModHost *))SIMBOLO(lib, "md_mod_iniciar");
    if (iniciar) {
        quem = info->nome;
        int r = iniciar(&HOST);
        quem = NULL;
        if (r != 0) {
            printf("[mods] %s recusou iniciar (codigo %d)\n", info->nome, r);
            FECHAR(lib);
            fflush(stdout);
            return;
        }
    }

    Mod *m = &mods[n_mods++];
    m->lib = lib;
    m->info = info;
    m->quadro   = (void (*)(void))SIMBOLO(lib, "md_mod_quadro");
    m->pixels   = (void (*)(unsigned char *, int, int))SIMBOLO(lib, "md_mod_pixels");
    m->encerrar = (void (*)(void))SIMBOLO(lib, "md_mod_encerrar");

    printf("[mods] %s %s por %s — %s\n", info->nome,
           info->versao ? info->versao : "", info->autor ? info->autor : "?",
           info->descricao ? info->descricao : "");
    fflush(stdout);
}

void mods_carregar(const char *dir) {
    char caminho[1024];

#if defined(_WIN32)
    snprintf(caminho, sizeof caminho, "%s\\*" EXTENSAO, dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(caminho, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        snprintf(caminho, sizeof caminho, "%s\\%s", dir, fd.cFileName);
        carregar(caminho, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    size_t n = strlen(EXTENSAO);
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= n || strcmp(e->d_name + len - n, EXTENSAO) != 0) continue;
        snprintf(caminho, sizeof caminho, "%s/%s", dir, e->d_name);
        carregar(caminho, e->d_name);
    }
    closedir(d);
#endif

    if (n_mods) {
        printf("[mods] %d carregado(s). Uma sessao com mod NAO e' uma sessao "
               "limpa — cite isto em qualquer relato de bug.\n", n_mods);
        fflush(stdout);
    }
}

void mods_quadro(void) {
    for (int i = 0; i < n_mods; i++) {
        if (mods[i].quadro) {
            quem = mods[i].info->nome;
            mods[i].quadro();
            quem = NULL;
        }
    }
}

void mods_pixels(unsigned char *rgb, int largura, int altura) {
    /* Em ordem de carregamento, cada um em cima do anterior. Dois filtros
     * empilhados fazem o que a ordem alfabetica dos arquivos disser — e' pouco,
     * mas e' previsivel, que e' o que importa para depurar. */
    for (int i = 0; i < n_mods; i++) {
        if (mods[i].pixels) {
            quem = mods[i].info->nome;
            mods[i].pixels(rgb, largura, altura);
            quem = NULL;
        }
    }
}

int mods_ativos(void) { return n_mods; }

void mods_encerrar(void) {
    for (int i = 0; i < n_mods; i++) {
        if (mods[i].encerrar) {
            quem = mods[i].info->nome;
            mods[i].encerrar();
            quem = NULL;
        }
        FECHAR(mods[i].lib);
    }
    n_mods = 0;
}
