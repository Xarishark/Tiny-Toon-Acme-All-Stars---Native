#include "release.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#if defined(TINYTOON_RELEASE) && defined(_WIN32)

#include <windows.h>
#include <commdlg.h>

static char dir_exe[MAX_PATH];
static char caminho_rom[MAX_PATH];

int release_ativo(void) { return 1; }

static char dir_mods[MAX_PATH];

/* O diretorio do executavel, nao o diretorio corrente.
 *
 * Os dois sao diferentes com frequencia: atalho na area de trabalho, "abrir
 * com", arrastar-e-soltar. Procurar a ROM no diretorio corrente funcionava no
 * terminal e falhava em todo o resto — e falhava em silencio. */
static void achar_dir_exe(void) {
    DWORD n = GetModuleFileNameA(NULL, dir_exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { dir_exe[0] = '.'; dir_exe[1] = 0; return; }
    char *barra = strrchr(dir_exe, '\\');
    if (barra) *barra = 0;
}

const char *release_dir_mods(void) {
    achar_dir_exe();
    snprintf(dir_mods, sizeof dir_mods, "%s\\mods", dir_exe);
    return dir_mods;
}

void release_iniciar(void) {
    achar_dir_exe();

    /* O log fica ao lado do exe. Se nao der para escrever la' (Program Files
     * sem permissao, por exemplo), o programa segue sem log em vez de morrer:
     * perder o diagnostico e' ruim, nao rodar e' pior. */
    char log[MAX_PATH];
    snprintf(log, sizeof log, "%s\\tinytoon.log", dir_exe);
    FILE *fh = freopen(log, "w", stdout);
    if (fh) {
        /* Sem isto o log so' aparece quando o buffer enche, e um travamento
         * leva junto justamente as linhas que explicariam o travamento. */
        setvbuf(stdout, NULL, _IOLBF, 4096);
        freopen(log, "a", stderr);
    }
    SetCurrentDirectoryA(dir_exe);
}

void release_erro(const char *titulo, const char *corpo) {
    printf("\n*** %s ***\n%s\n", titulo, corpo);
    fflush(stdout);
    MessageBoxA(NULL, corpo, titulo, MB_OK | MB_ICONERROR);
}

static int existe(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

const char *release_achar_rom(const char *argumento) {
    if (argumento && existe(argumento)) return argumento;

    snprintf(caminho_rom, sizeof caminho_rom, "%s\\rom.bin", dir_exe);
    if (existe(caminho_rom)) return caminho_rom;

    /* O caminho lembrado: o que o seletor escolheu da ultima vez, ou o que o
     * RecompLauncher escreveu (game_path do recomplauncher.json). Sem isto o
     * seletor abria em TODA partida — e pelo launcher, em tela cheia e com
     * controle na mao, ele e' o pior primeiro contato possivel. */
    const char *lembrado = config_texto("rom");
    if (lembrado && *lembrado && existe(lembrado)) {
        snprintf(caminho_rom, sizeof caminho_rom, "%s", lembrado);
        return caminho_rom;
    }

    /* Nada achado: perguntar, em vez de desistir.
     *
     * Este e' o erro de primeira execucao mais provavel que existe, porque a
     * ROM nao acompanha nem pode acompanhar o programa. Um seletor transforma
     * "nao funcionou" em cinco segundos de trabalho. */
    MessageBoxA(NULL,
        "This program is the recompiled game, and it needs your own ROM of\n"
        "Tiny Toon Adventures: ACME All-Stars (Europe) to run.\n\n"
        "The ROM does not come with the program: it is copyrighted, and the\n"
        "dump has to be yours.\n\n"
        "Pick the .bin file in the next window. It will be remembered, so you\n"
        "will not be asked again.",
        "Tiny Toon Adventures: ACME All-Stars", MB_OK | MB_ICONINFORMATION);

    caminho_rom[0] = 0;
    OPENFILENAMEA of;
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.lpstrFilter = "Mega Drive ROM (*.bin;*.md;*.gen)\0*.bin;*.md;*.gen\0"
                     "All files\0*.*\0";
    of.lpstrFile = caminho_rom;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = "Pick your Tiny Toon Adventures: ACME All-Stars ROM";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&of)) return NULL;
    /* Gravado antes de verificar: se o arquivo for o errado, a mensagem de
     * diagnostico explica, e na proxima vez o caminho errado nao existe mais
     * ou continua errado e o diagnostico repete. Lembrar so' ROM boa exigiria
     * a verificacao aqui dentro, duplicada da de main.c. */
    if (config_gravar_texto("rom", caminho_rom) != 0)
        printf("nao consegui gravar o caminho da ROM em %s\n", CONFIG_ARQUIVO);
    return caminho_rom;
}

#else

int release_ativo(void) { return 0; }
void release_iniciar(void) { }

/* Fora do release, o diretorio corrente E' o diretorio de trabalho de quem
 * esta' desenvolvendo, e "mods/" relativo e' o que se espera. */
const char *release_dir_mods(void) { return "mods"; }

void release_erro(const char *titulo, const char *corpo) {
    printf("\n*** %s ***\n%s\n", titulo, corpo);
    fflush(stdout);
}

const char *release_achar_rom(const char *argumento) {
    return argumento ? argumento : "rom.bin";
}

#endif
