#include "jogo_carga.h"

#ifdef TINYTOON_JOGO_DLL

#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "release.h"

#ifdef _WIN32
#include <windows.h>
#define JOGO_ARQUIVO "jogo.dll"
#define RECOMP_REFERENCIA "montador\\recomp.sha256"
#else
#include <dlfcn.h>
#define JOGO_ARQUIVO "./jogo.so"
#define RECOMP_REFERENCIA "montador/recomp.sha256"
#endif

/* O que o runtime entrega ao jogo: as nove funcoes de memory.c. */
static const JogoImporta importa = {
    md_read8, md_read16, md_read32,
    md_write8, md_write16, md_write32,
    md_idle, md_unresolved_dispatch, md_untranslated,
};

#ifdef _WIN32
static HMODULE modulo;
static void descarregar(void) { if (modulo) FreeLibrary(modulo); modulo = NULL; }
static void *abrir(void) {
    modulo = LoadLibraryA(JOGO_ARQUIVO);
    return modulo ? (void *)GetProcAddress(modulo, JOGO_LIGAR_NOME) : NULL;
}
#else
static void *modulo;
static void descarregar(void) { if (modulo) dlclose(modulo); modulo = NULL; }
static void *abrir(void) {
    modulo = dlopen(JOGO_ARQUIVO, RTLD_NOW);
    return modulo ? dlsym(modulo, JOGO_LIGAR_NOME) : NULL;
}
#endif

/* 0 = carregado e da ROM certa; -1 = ausente ou ilegivel; -2 = de outra ROM
 * ou outra versao da fronteira (precisa remontar). */
static int tentar(const char *rom_sha1, JogoExporta *saida) {
    jogo_ligar_fn ligar = (jogo_ligar_fn)abrir();
    if (!ligar) { descarregar(); return -1; }
    memset(saida, 0, sizeof *saida);
    if (ligar(&importa, saida) != 0 || saida->versao != JOGO_API_VERSAO) {
        printf("jogo: %s e' de outra versao do programa (fronteira %d, esperada %d)\n",
               JOGO_ARQUIVO, saida->versao, JOGO_API_VERSAO);
        descarregar();
        return -2;
    }
    if (!saida->rom_sha1 || strcmp(saida->rom_sha1, rom_sha1) != 0) {
        printf("jogo: %s foi montado de outra ROM (%s)\n", JOGO_ARQUIVO,
               saida->rom_sha1 ? saida->rom_sha1 : "?");
        descarregar();
        return -2;
    }
    /* O C de referencia do montador INSTALADO. Depois de uma atualizacao que
     * mude o recompilador, ele difere do que a DLL guardou: remontar. Sem o
     * arquivo (desenvolvimento, montagem a mao) nao ha' o que comparar. */
    char atual[80] = "";
    FILE *f = fopen(RECOMP_REFERENCIA, "r");
    if (f) {
        if (fscanf(f, "%64s", atual) != 1) atual[0] = 0;
        fclose(f);
    }
    if (atual[0] && (!saida->recomp_sha256 ||
                     strcmp(saida->recomp_sha256, atual) != 0)) {
        printf("jogo: %s e' de uma versao anterior do recompilador\n", JOGO_ARQUIVO);
        descarregar();
        return -2;
    }
    return 0;
}

#if defined(_WIN32) && defined(TINYTOON_RELEASE)
/* Roda o montador que vem no zip: python embarcado + tools/montar_jogo.py.
 *
 * Pelo RecompLauncher isto nao acontece: o passo `prepare` do
 * recomplauncher.json ja' montou antes do primeiro Play. Aqui e' o caminho de
 * quem extraiu o zip e clicou duas vezes no exe — uma vez so', na primeira
 * partida, numa janela de console que mostra o progresso. */
static int montar(const char *caminho_rom) {
    char linha[2048];
    snprintf(linha, sizeof linha,
             "\"python\\python.exe\" \"montador\\tools\\montar_jogo.py\" "
             "--rom \"%s\" --saida . --pausar-se-falhar", caminho_rom);
    printf("montando o jogo: %s\n", linha);
    fflush(stdout);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, linha, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                        NULL, NULL, &si, &pi)) {
        printf("montador nao iniciou (erro %lu)\n", (unsigned long)GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD codigo = 1;
    GetExitCodeProcess(pi.hProcess, &codigo);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    printf("montador terminou com codigo %lu\n", (unsigned long)codigo);
    return codigo == 0 ? 0 : -1;
}
#else
static int montar(const char *caminho_rom) {
    printf("monte o jogo com: python tools/montar_jogo.py --rom \"%s\" --saida .\n",
           caminho_rom);
    return -1;
}
#endif

int jogo_preparar(const char *caminho_rom, const char *rom_sha1,
                  JogoExporta *saida) {
    int r = tentar(rom_sha1, saida);
    if (r == 0) { printf("jogo: %s carregado\n", JOGO_ARQUIVO); return 0; }

    printf("jogo: %s %s; montando a partir da sua ROM\n", JOGO_ARQUIVO,
           r == -1 ? "ainda nao existe" : "precisa ser remontado");
    if (montar(caminho_rom) != 0) {
        release_erro("Could not build the game",
            "The game is built from your own ROM the first time it runs, and "
            "that step failed.\n\n"
            "The console window that opened says why. If it closed, run the "
            "program again; tinytoon.log has the details.");
        return SAIDA_JOGO_NAO_MONTADO;
    }
    r = tentar(rom_sha1, saida);
    if (r == 0) { printf("jogo: %s montado e carregado\n", JOGO_ARQUIVO); return 0; }
    release_erro("Could not load the game",
        "The game was built but could not be loaded. Delete jogo.dll next to "
        "the program and run it again.");
    return SAIDA_JOGO_INVALIDO;
}

#else
/* Sem TINYTOON_JOGO_DLL o jogo e' linkado junto (build monolitico). */
typedef int jogo_carga_sem_uso;
#endif /* TINYTOON_JOGO_DLL */
