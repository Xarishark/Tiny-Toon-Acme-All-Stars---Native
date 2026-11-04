#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHAVES 32
#define TAM_CHAVE  64
#define TAM_VALOR  1024     /* cabe um caminho do Windows com folga */

static struct { char chave[TAM_CHAVE]; char valor[TAM_VALOR]; } tabela[MAX_CHAVES];
static int n_chaves;
static char arquivo[TAM_VALOR] = CONFIG_ARQUIVO;

static char *aparar(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *f = s + strlen(s);
    while (f > s && isspace((unsigned char)f[-1])) *--f = 0;
    return s;
}

/* Separa `chave = valor` de uma linha. Devolve 0 se a linha nao e' atribuicao
 * (vazia, comentario, cabecalho de tabela).
 *
 * ESCAPES: so' \\ e \". Um caminho do Windows escrito sem escape — "C:\roms\
 * new.bin" — tem \r e \n que pelo TOML seriam controle; aqui ficam literais.
 * Nao sabemos como o launcher escreve o caminho que ele poe em `rom`, e nenhum
 * caminho de arquivo contem caractere de controle, entao aceitar as duas
 * formas nao custa nada e nao erra nenhuma. */
static int analisar(const char *linha, char *chave, char *valor) {
    char buf[TAM_VALOR + TAM_CHAVE + 8];
    snprintf(buf, sizeof buf, "%s", linha);
    char *s = aparar(buf);
    if (!*s || *s == '#' || *s == '[') return 0;
    char *igual = strchr(s, '=');
    if (!igual) return 0;
    *igual = 0;
    snprintf(chave, TAM_CHAVE, "%s", aparar(s));
    if (!*chave) return 0;

    char *v = aparar(igual + 1);
    size_t o = 0;
    if (*v == '"') {
        for (v++; *v && *v != '"' && o + 1 < TAM_VALOR; v++) {
            if (*v == '\\' && (v[1] == '\\' || v[1] == '"')) v++;
            valor[o++] = *v;
        }
    } else if (*v == '\'') {
        for (v++; *v && *v != '\'' && o + 1 < TAM_VALOR; v++) valor[o++] = *v;
    } else {
        for (; *v && *v != '#' && o + 1 < TAM_VALOR; v++) valor[o++] = *v;
        while (o > 0 && isspace((unsigned char)valor[o - 1])) o--;
    }
    valor[o] = 0;
    return 1;
}

static void guardar(const char *chave, const char *valor) {
    for (int i = 0; i < n_chaves; i++) {
        if (strcmp(tabela[i].chave, chave) == 0) {
            snprintf(tabela[i].valor, TAM_VALOR, "%s", valor);
            return;
        }
    }
    if (n_chaves == MAX_CHAVES) return;
    snprintf(tabela[n_chaves].chave, TAM_CHAVE, "%s", chave);
    snprintf(tabela[n_chaves].valor, TAM_VALOR, "%s", valor);
    n_chaves++;
}

void config_carregar(const char *caminho) {
    snprintf(arquivo, sizeof arquivo, "%s", caminho);
    n_chaves = 0;
    FILE *f = fopen(arquivo, "r");
    if (!f) { printf("config: sem %s, usando os padroes\n", arquivo); return; }
    char linha[TAM_VALOR + TAM_CHAVE + 8], chave[TAM_CHAVE], valor[TAM_VALOR];
    while (fgets(linha, sizeof linha, f)) {
        if (analisar(linha, chave, valor)) guardar(chave, valor);
    }
    fclose(f);
    printf("config: %s, %d chave(s)\n", arquivo, n_chaves);
    for (int i = 0; i < n_chaves; i++)
        printf("  %s = %s\n", tabela[i].chave, tabela[i].valor);
}

const char *config_texto(const char *chave) {
    for (int i = 0; i < n_chaves; i++)
        if (strcmp(tabela[i].chave, chave) == 0) return tabela[i].valor;
    return NULL;
}

int config_int(const char *chave, int padrao) {
    const char *v = config_texto(chave);
    if (!v || !*v) return padrao;
    char *fim;
    long n = strtol(v, &fim, 10);
    return *fim ? padrao : (int)n;
}

int config_bool(const char *chave, int padrao) {
    const char *v = config_texto(chave);
    if (!v) return padrao;
    if (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes")) return 1;
    if (!strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no")) return 0;
    return padrao;
}

int config_gravar_texto(const char *chave, const char *valor) {
    /* Le' o arquivo inteiro e reescreve trocando so' a linha da chave. O
     * arquivo e' pequeno; o que importa e' nao perder o que o launcher ou o
     * jogador escreveram e este programa nao conhece. */
    char *antigo = NULL;
    size_t tam = 0;
    FILE *f = fopen(arquivo, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 1 << 20) {
            antigo = malloc((size_t)n + 1);
            if (antigo) { tam = fread(antigo, 1, (size_t)n, f); antigo[tam] = 0; }
        }
        fclose(f);
    }

    char nova[TAM_CHAVE + 2 * TAM_VALOR + 8];
    size_t o = (size_t)snprintf(nova, sizeof nova, "%s = \"", chave);
    for (const char *p = valor; *p && o + 4 < sizeof nova; p++) {
        if (*p == '\\' || *p == '"') nova[o++] = '\\';
        nova[o++] = *p;
    }
    nova[o++] = '"'; nova[o] = 0;

    f = fopen(arquivo, "wb");
    if (!f) { free(antigo); return -1; }
    int trocou = 0;
    for (char *l = antigo; l && *l; ) {
        char *fim = strchr(l, '\n');
        size_t n = fim ? (size_t)(fim - l + 1) : strlen(l);
        char linha[TAM_VALOR + TAM_CHAVE + 8], k[TAM_CHAVE], v[TAM_VALOR];
        snprintf(linha, sizeof linha, "%.*s", (int)n, l);
        if (!trocou && analisar(linha, k, v) && strcmp(k, chave) == 0) {
            fprintf(f, "%s\n", nova);
            trocou = 1;
        } else {
            fwrite(l, 1, n, f);
            if (!fim) fputc('\n', f);
        }
        l += n;
    }
    if (!trocou) fprintf(f, "%s\n", nova);
    int erro = ferror(f);
    fclose(f);
    free(antigo);
    if (erro) return -1;
    guardar(chave, valor);
    return 0;
}
